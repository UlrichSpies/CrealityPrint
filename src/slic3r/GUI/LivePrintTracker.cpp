#include "LivePrintTracker.hpp"

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "print_manage/Device/CrealityPrinterSocket.hpp"

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace Slic3r {
namespace GUI {

namespace {

// How often the tracker asks the source for a new position and moves the preview.
constexpr int    POLL_INTERVAL_MS = 33;
// The preview runs this far behind the printer, so there is always a newer report to move
// towards. The printer reports about every 0.25 s, but sometimes stalls for up to a second and
// then sends a burst.
constexpr double DISPLAY_DELAY  = 0.8; // s
// Reports in a burst are spread out to at least this far apart, so the preview doesn't race.
constexpr double MIN_REPORT_SPACING = 0.15; // s
// Reports older than this are dropped.
constexpr double REPORT_HISTORY = 3.;  // s
// A report further than this (XY) from every candidate counts as a miss.
constexpr float  NEAR_MAX_DISTANCE = 0.6f; // mm
// A full search only accepts moves this close (XY) to the report.
constexpr float  SEARCH_ALL_MAX_DISTANCE = 0.8f; // mm
// Full search only looks at moves within this Z of the report (covers z-hops and the bed mesh).
constexpr float  SEARCH_ALL_Z_TOLERANCE = 1.5f; // mm
// Candidates are searched along the toolpath from a little behind the last match to well past
// where the learned speed predicts the nozzle to be.
constexpr double WINDOW_BEHIND    = 15.; // mm
// Cost of a candidate on the layer next to the one the printer reports, in mm.
constexpr double LAYER_WEIGHT = 0.4;
constexpr double WINDOW_AHEAD_MIN = 30.; // mm, plus 3x the predicted travel
// How much a candidate's distance from the predicted toolpath position counts against it, mm per
// mm. The neighbouring infill line is a line length (tens of mm) away along the path, while the
// reported position is only a few tenths of a mm off, so the prediction picks the right line.
constexpr double PATH_WEIGHT = 0.005;
// This many bad reports in a row and the tracker searches the whole layer again.
constexpr int    MISSES_BEFORE_RESYNC = 4;

// Seams are markers, not motion, so they never describe where the nozzle is.
bool is_motion(const GCodeProcessorResult::MoveVertex& move) { return move.type != EMoveType::Seam; }

} // namespace

// ---------------------------------------------------------------------------------------------
// SimulatedPrintSource

SimulatedPrintSource::SimulatedPrintSource(const GCodeProcessorResult& result, double speed)
    : m_speed(std::max(speed, 0.01)), m_start(std::chrono::steady_clock::now())
{
    m_steps.reserve(result.moves.size());
    float time = 0.f;
    Vec3f prev = result.moves.empty() ? Vec3f::Zero() : result.moves.front().position;
    for (const GCodeProcessorResult::MoveVertex& move : result.moves) {
        if (!is_motion(move))
            continue;
        const float distance = (move.position - prev).norm();
        if (move.feedrate > 0.f)
            time += distance / move.feedrate;
        m_steps.push_back({ move.position, time, static_cast<int>(move.layer_id) + 1 }); // numbered like the printer
        prev = move.position;
    }
}

std::string SimulatedPrintSource::name() const
{
    return (boost::format("simulated printer (x%1%)") % m_speed).str();
}

std::optional<PrintPositionSample> SimulatedPrintSource::poll()
{
    if (m_steps.empty())
        return std::nullopt;

    const float elapsed = static_cast<float>(
        std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count() * m_speed);
    while (m_cursor + 1 < m_steps.size() && m_steps[m_cursor].end_time < elapsed)
        ++m_cursor;

    const Step& step = m_steps[m_cursor];
    PrintPositionSample sample;
    sample.layer    = step.layer;
    sample.position = step.position;
    if (m_cursor > 0 && elapsed < step.end_time) {
        // Part way along the current move.
        const Step& prev     = m_steps[m_cursor - 1];
        const float duration = step.end_time - prev.end_time;
        if (duration > 0.f) {
            const float t   = std::clamp((elapsed - prev.end_time) / duration, 0.f, 1.f);
            sample.position = prev.position + t * (step.position - prev.position);
        }
    }
    return sample;
}

// ---------------------------------------------------------------------------------------------
// CrealityPrinterPositionSource

namespace {
constexpr auto POSITION_STATS_INTERVAL = std::chrono::seconds(5);

// The printer sends numbers either as JSON numbers or as strings.
std::optional<double> json_number(const nlohmann::json& status, const char* key)
{
    const auto it = status.find(key);
    if (it == status.end())
        return std::nullopt;
    if (it->is_number())
        return it->get<double>();
    if (it->is_string()) {
        const std::string text = it->get<std::string>();
        double value           = 0.;
        if (std::from_chars(text.data(), text.data() + text.size(), value).ec == std::errc())
            return value;
    }
    return std::nullopt;
}
} // namespace

CrealityPrinterPositionSource::CrealityPrinterPositionSource(std::string address)
    : m_address(std::move(address)), m_stats_start(std::chrono::steady_clock::now())
{
    m_socket = std::make_unique<CrealityPrinterSocket>(
        m_address, [this](const nlohmann::json& status) { on_status(status); },
        [this](bool connected, const std::string& error) { on_connection(connected, error); });
    m_socket->connect();
}

CrealityPrinterPositionSource::~CrealityPrinterPositionSource()
{
    // Close first so no callback can reach this object while it is being destroyed.
    m_socket.reset();
}

std::string CrealityPrinterPositionSource::name() const
{
    return "printer " + m_address + (m_connected ? "" : " (connecting)");
}

std::optional<PrintPositionSample> CrealityPrinterPositionSource::poll()
{
    std::optional<PrintPositionSample> sample = m_latest;
    m_latest.reset();
    return sample;
}

void CrealityPrinterPositionSource::on_connection(bool connected, const std::string& error)
{
    m_connected = connected;
    if (!connected)
        BOOST_LOG_TRIVIAL(warning) << "LivePrintTracker: lost the printer connection to " << m_address << ": " << error;
    m_stats_start       = std::chrono::steady_clock::now();
    m_messages          = 0;
    m_position_changes  = 0;
}

void CrealityPrinterPositionSource::on_status(const nlohmann::json& status)
{
    ++m_messages;

    // "curPosition": "X:110.00 Y:10.00 Z:50.00"
    const auto it = status.find("curPosition");
    if (it != status.end() && it->is_string()) {
        const std::string text = it->get<std::string>();
        // Not sscanf: %lf follows the locale, and a decimal-comma locale rejects "133.97".
        double axis[3] = {0., 0., 0.};
        int    found   = 0;
        for (const char* p = text.c_str(); *p != '\0' && found < 3; ++p) {
            if (*p == 'X' + found && p[1] == ':') {
                const char* first = p + 2;
                const char* last  = text.c_str() + text.size();
                const auto  res   = std::from_chars(first, last, axis[found]);
                if (res.ec != std::errc())
                    break;
                ++found;
                p = res.ptr - 1;
            }
        }
        const double x = axis[0], y = axis[1], z = axis[2];
        if (found == 3) {
            if (text != m_last_position_text) {
                m_last_position_text = text;
                ++m_position_changes;
                PrintPositionSample sample;
                sample.position = Vec3f(float(x), float(y), float(z));
                if (const std::optional<double> layer = json_number(status, "layer"))
                    sample.layer = static_cast<int>(*layer);
                m_latest = sample;
            }
        }
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - m_stats_start >= POSITION_STATS_INTERVAL) {
        const double seconds = std::chrono::duration<double>(now - m_stats_start).count();
        BOOST_LOG_TRIVIAL(warning) << boost::format("LivePrintTracker: %1% reports %2% status messages/s, %3% position changes/s (last %4%)") %
                                       m_address % (m_messages / seconds) % (m_position_changes / seconds) % m_last_position_text;
        if (m_position_changes == 0) {
            std::string keys;
            for (auto kv = status.begin(); kv != status.end(); ++kv)
                keys += kv.key() + " ";
            const auto cp = status.find("curPosition");
            BOOST_LOG_TRIVIAL(warning) << "LivePrintTracker: no position parsed. curPosition="
                                       << (cp == status.end() ? std::string("<absent>") : cp->dump())
                                       << " keys: " << keys;
        }
        m_stats_start      = now;
        m_messages         = 0;
        m_position_changes = 0;
    }
}

// ---------------------------------------------------------------------------------------------
// PrintPositionMatcher

namespace {
// The KE counts layers from 1 (layer 26 is printed at Z 5.2 with 0.2 mm layers), the slicer's
// layer_id from 0. Its number changes slightly before or after the position does, so a layer
// either side is accepted, but at a cost: neighbouring layers usually have the same XY path, so
// without it the matcher happily follows the layer below. -1 accepts everything at no cost.
int layer_mismatch(const GCodeProcessorResult::MoveVertex& move, int printer_layer)
{
    return printer_layer < 0 ? 0 : std::abs(static_cast<int>(move.layer_id) - (printer_layer - 1));
}
bool layer_plausible(const GCodeProcessorResult::MoveVertex& move, int printer_layer) { return layer_mismatch(move, printer_layer) <= 1; }
} // namespace

PrintPositionMatcher::PrintPositionMatcher(const GCodeProcessorResult& result) : m_result(result)
{
    m_length.reserve(result.moves.size());
    double length = 0.;
    Vec3f  prev   = result.moves.empty() ? Vec3f::Zero() : result.moves.front().position;
    for (const GCodeProcessorResult::MoveVertex& move : result.moves) {
        if (is_motion(move)) {
            length += double((move.position - prev).norm());
            prev = move.position;
        }
        m_length.push_back(length);
    }
}

size_t PrintPositionMatcher::move_at_position(double position) const
{
    if (m_length.empty())
        return 0;
    const auto it = std::lower_bound(m_length.begin(), m_length.end(), position);
    return it == m_length.end() ? m_length.size() - 1 : static_cast<size_t>(std::distance(m_length.begin(), it));
}

std::optional<PrintPositionMatcher::Match> PrintPositionMatcher::match(const PrintPositionSample& sample)
{
    const auto now = std::chrono::steady_clock::now();

    // An exact file position beats any geometric guess.
    if (sample.file_position >= 0) {
        if (std::optional<Match> found = match_file_position(sample.file_position)) {
            m_last          = found->move_id;
            m_last_position = found->position;
            m_last_wall     = now;
            return found;
        }
    }

    // Only moves around where the nozzle should be by now are candidates: in dense infill,
    // neighbouring lines are closer together than the position noise, so position alone picks
    // the wrong one.
    const bool   tracking  = m_last.has_value();
    const double elapsed   = tracking ? std::clamp(std::chrono::duration<double>(now - m_last_wall).count(), 0., 3.) : 0.;
    const double travel    = m_speed * elapsed;
    const double predicted = m_last_position + travel;
    bool         resynced  = false;

    std::optional<Candidate> best;
    if (tracking) {
        best = search_near(sample.position, sample.layer, predicted, travel);
        if (best && best->distance > NEAR_MAX_DISTANCE)
            best.reset();
    }
    if (best) {
        m_misses        = 0;
        m_last_distance = best->distance;
    } else {
        ++m_misses;
        if (tracking && m_misses < MISSES_BEFORE_RESYNC)
            return std::nullopt; // a bad report or two: wait for the next one
        // Lost track: search the whole layer.
        best = search_all(sample.position, sample.layer);
        if (!best && sample.layer >= 0)
            best = search_all(sample.position, -1); // the reported layer may be stale or numbered differently
        m_last_distance = best ? best->distance : -1.f;
        if (best && best->distance > SEARCH_ALL_MAX_DISTANCE)
            best.reset();
        if (!best)
            return std::nullopt;
        resynced = true;
        m_misses = 0;
    }

    // Not clamped to move forwards: if the last match was a little too far ahead, the next one may
    // correct it. The preview itself never moves back.
    const double position = position_in_move(*best);
    if (!resynced && elapsed >= 0.1)
        m_speed = std::clamp(0.6 * m_speed + 0.4 * std::max(0., position - m_last_position) / elapsed, 1., 1000.);
    m_last          = best->move_id;
    m_last_position = position;
    m_last_wall     = now;
    return Match{ best->move_id, position, resynced };
}

std::optional<PrintPositionMatcher::Match> PrintPositionMatcher::match_file_position(long long file_position) const
{
    // lines_ends[i] is the byte offset where line i + 1 ends.
    // TODO: check the line numbering against gcode_id with a real Moonraker feed.
    const std::vector<size_t>& lines_ends = m_result.lines_ends;
    if (lines_ends.empty())
        return std::nullopt;
    const auto         line_it = std::upper_bound(lines_ends.begin(), lines_ends.end(), static_cast<size_t>(file_position));
    const unsigned int line_id = static_cast<unsigned int>(std::distance(lines_ends.begin(), line_it)) + 1;

    const auto& moves   = m_result.moves;
    const auto  move_it = std::lower_bound(moves.begin(), moves.end(), line_id,
                                           [](const GCodeProcessorResult::MoveVertex& move, unsigned int id) { return move.gcode_id < id; });
    if (move_it == moves.end())
        return std::nullopt;
    const size_t move_id = static_cast<size_t>(std::distance(moves.begin(), move_it));
    return Match{ move_id, m_length[move_id], false };
}

float PrintPositionMatcher::distance_to_move(size_t move_id, const Vec3f& p, float* frac) const
{
    const auto& moves = m_result.moves;
    const Vec2f q(p.x(), p.y());
    const Vec2f b(moves[move_id].position.x(), moves[move_id].position.y());
    size_t      prev = move_id;
    while (prev > 0) {
        --prev;
        if (is_motion(moves[prev])) {
            const Vec2f a(moves[prev].position.x(), moves[prev].position.y());
            const Vec2f ab     = b - a;
            const float len_sq = ab.squaredNorm();
            const float t      = len_sq > 0.f ? std::clamp((q - a).dot(ab) / len_sq, 0.f, 1.f) : 1.f;
            if (frac)
                *frac = t;
            return (q - (a + t * ab)).norm();
        }
    }
    if (frac)
        *frac = 1.f;
    return (q - b).norm();
}

double PrintPositionMatcher::position_in_move(const Candidate& candidate) const
{
    // The move starts where the previous motion move ends.
    const auto& moves = m_result.moves;
    size_t      prev  = candidate.move_id;
    while (prev > 0) {
        --prev;
        if (is_motion(moves[prev]))
            break;
    }
    const double end   = m_length[candidate.move_id];
    const double start = candidate.move_id > 0 ? m_length[prev] : end;
    return start + double(candidate.frac) * (end - start);
}

std::optional<PrintPositionMatcher::Candidate> PrintPositionMatcher::search_near(const Vec3f& p, int layer, double predicted, double travel) const
{
    const auto&  moves = m_result.moves;
    const size_t begin = move_at_position(m_last_position - WINDOW_BEHIND);
    const size_t end   = std::min(moves.size(), move_at_position(predicted + WINDOW_AHEAD_MIN + 3. * travel) + 1);

    std::optional<Candidate> best;
    double                   best_score = std::numeric_limits<double>::max();
    for (size_t i = begin; i < end; ++i) {
        if (!is_motion(moves[i]) || !layer_plausible(moves[i], layer))
            continue;
        Candidate candidate{ i, 0.f, 0.f };
        candidate.distance = distance_to_move(i, p, &candidate.frac);
        const double score = double(candidate.distance) + PATH_WEIGHT * std::abs(position_in_move(candidate) - predicted) +
                             LAYER_WEIGHT * layer_mismatch(moves[i], layer);
        if (score < best_score) {
            best_score = score;
            best       = candidate;
        }
    }
    return best;
}

std::optional<PrintPositionMatcher::Candidate> PrintPositionMatcher::search_all(const Vec3f& p, int layer) const
{
    const auto&              moves = m_result.moves;
    std::optional<Candidate> best;
    double                   best_score = std::numeric_limits<double>::max();
    for (size_t i = 0; i < moves.size(); ++i) {
        if (!is_motion(moves[i]) || !layer_plausible(moves[i], layer) ||
            std::abs(moves[i].position.z() - p.z()) > SEARCH_ALL_Z_TOLERANCE)
            continue;
        Candidate candidate{ i, 0.f, 0.f };
        candidate.distance = distance_to_move(i, p, &candidate.frac);
        const double score = double(candidate.distance) + LAYER_WEIGHT * layer_mismatch(moves[i], layer);
        if (score < best_score) {
            best_score = score;
            best       = candidate;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------------------------
// LivePrintTracker

LivePrintTracker::LivePrintTracker(ResultGetter get_result, MoveCallback on_move)
    : m_get_result(std::move(get_result)), m_on_move(std::move(on_move))
{
    m_timer.SetOwner(this);
    Bind(wxEVT_TIMER, &LivePrintTracker::on_timer, this, m_timer.GetId());
}

LivePrintTracker::~LivePrintTracker() { m_timer.Stop(); }

void LivePrintTracker::start(SourceFactory make_source)
{
    m_make_source = std::move(make_source);
    m_result      = nullptr;
    m_source.reset();
    m_matcher.reset();
    reset_display();
    m_timer.Start(POLL_INTERVAL_MS);
}

void LivePrintTracker::stop()
{
    m_timer.Stop();
    m_source.reset();
    m_matcher.reset();
    m_result = nullptr;
}

std::string LivePrintTracker::source_name() const { return m_source ? m_source->name() : std::string(); }

void LivePrintTracker::reset_display()
{
    m_last_move.reset();
    m_reports.clear();
    m_shown_position.reset();
}

bool LivePrintTracker::sync_with_result()
{
    const GCodeProcessorResult* result = m_get_result ? m_get_result() : nullptr;
    if (result == nullptr || result->moves.empty()) {
        m_source.reset();
        m_matcher.reset();
        m_result = nullptr;
        return false;
    }
    if (result != m_result || result->moves.size() != m_result_moves || !m_source) {
        m_result       = result;
        m_result_moves = result->moves.size();
        m_source       = m_make_source ? m_make_source(*result) : nullptr;
        m_matcher      = std::make_unique<PrintPositionMatcher>(*result);
        reset_display();
        BOOST_LOG_TRIVIAL(info) << "LivePrintTracker: following " << source_name() << ", " << m_result_moves << " moves";
    }
    return m_source != nullptr;
}

void LivePrintTracker::on_timer(wxTimerEvent&)
{
    if (!sync_with_result())
        return;

    const auto now = std::chrono::steady_clock::now();

    if (const std::optional<PrintPositionSample> sample = m_source->poll()) {
        ++m_stat_samples;
        const std::optional<PrintPositionMatcher::Match> match = m_matcher->match(*sample);
        if (match) {
            ++m_stat_matched;
            if (match->resynced || !m_shown_position) {
                // First match, or tracking was lost: jump there.
                if (match->resynced)
                    ++m_stat_resyncs;
                m_reports.clear();
                m_shown_position = match->position;
            }
            auto stamp = now;
            if (!m_reports.empty())
                stamp = std::max(stamp, m_reports.back().time + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                                    std::chrono::duration<double>(MIN_REPORT_SPACING)));
            m_reports.push_back({ stamp, match->position });
        }
        m_stat_last = (boost::format("pos (%1$.2f, %2$.2f, %3$.2f) layer %4% -> %5%, distance %6$.2f mm, speed %7$.0f mm/s") %
                       sample->position.x() % sample->position.y() % sample->position.z() % sample->layer %
                       (match ? (boost::format("move %1% at %2$.1f mm") % match->move_id % match->position).str() : std::string("no match")) %
                       m_matcher->last_distance() % m_matcher->speed())
                          .str();
    }

    // Show where the nozzle was DISPLAY_DELAY ago, interpolated between the reports around then.
    while (m_reports.size() > 2 && std::chrono::duration<double>(now - m_reports[1].time).count() > REPORT_HISTORY)
        m_reports.pop_front();
    if (m_shown_position && !m_reports.empty()) {
        const auto render = now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(DISPLAY_DELAY));
        double     target = m_reports.back().position; // no newer report yet: wait at the newest
        if (render <= m_reports.front().time)
            target = m_reports.front().position;
        else
            for (size_t i = 0; i + 1 < m_reports.size(); ++i) {
                const Report& a = m_reports[i];
                const Report& b = m_reports[i + 1];
                if (render < b.time) {
                    const double span = std::chrono::duration<double>(b.time - a.time).count();
                    const double t    = span > 0. ? std::chrono::duration<double>(render - a.time).count() / span : 1.;
                    target            = a.position + std::clamp(t, 0., 1.) * (b.position - a.position);
                    break;
                }
            }
        m_shown_position = std::max(*m_shown_position, target);
        const size_t shown = m_matcher->move_at_position(*m_shown_position);
        if (!m_last_move || shown != *m_last_move) {
            m_last_move = shown;
            if (m_on_move)
                m_on_move(shown);
        }
    }

    if (now - m_stat_start >= std::chrono::seconds(5)) {
        BOOST_LOG_TRIVIAL(warning) << "LivePrintTracker: " << source_name() << ": " << m_stat_samples << " samples, " << m_stat_matched
                                   << " matched, " << m_stat_resyncs << " resyncs in 5 s. Last: " << (m_stat_last.empty() ? "none yet" : m_stat_last);
        m_stat_samples = 0;
        m_stat_matched = 0;
        m_stat_resyncs = 0;
        m_stat_start   = now;
    }
}

} // namespace GUI
} // namespace Slic3r
