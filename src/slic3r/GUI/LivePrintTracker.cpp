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

// How often the tracker asks the source for a new position.
constexpr int   POLL_INTERVAL_MS = 33;
// The display clock (estimated print time) is steered towards the matched time with this gain;
// it never runs backwards and is capped to this multiple of real speed when catching up.
constexpr double CLOCK_GAIN      = 2.5;  // 1/s
constexpr double CLOCK_MAX_RATE  = 8.;
// Between reports the target is extrapolated at real speed, for at most this long.
constexpr double MAX_EXTRAPOLATE = 0.5; // s
// Further than this from the clock, jump instead of steering.
constexpr double RESYNC_BEHIND   = 2.;  // s
constexpr double RESYNC_AHEAD    = 10.; // s
// A report further than this from every move nearby the last match triggers a full search.
constexpr float NEAR_LAST_MAX_DISTANCE = 0.5f; // mm
// A full search only accepts moves this close to the report.
constexpr float SEARCH_ALL_MAX_DISTANCE = 1.0f; // mm
// Full search only looks at moves within this Z of the report (covers z-hops).
constexpr float SEARCH_ALL_Z_TOLERANCE = 1.5f; // mm
// Window around the last match that is searched first. Prints run forwards, so look further ahead.
constexpr size_t NEAR_LAST_BEHIND = 8;
constexpr size_t NEAR_LAST_AHEAD  = 3000;

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
        m_steps.push_back({ move.position, time, static_cast<int>(move.layer_id) });
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

PrintPositionMatcher::PrintPositionMatcher(const GCodeProcessorResult& result) : m_result(result)
{
    // Estimated print time at the end of every move, the same estimate the simulation uses.
    m_time.reserve(result.moves.size());
    double time = 0.;
    Vec3f  prev = result.moves.empty() ? Vec3f::Zero() : result.moves.front().position;
    for (const GCodeProcessorResult::MoveVertex& move : result.moves) {
        if (is_motion(move)) {
            if (move.feedrate > 0.f)
                time += double((move.position - prev).norm() / move.feedrate);
            prev = move.position;
        }
        m_time.push_back(time);
    }
}

size_t PrintPositionMatcher::move_at_time(double time) const
{
    if (m_time.empty())
        return 0;
    const auto it = std::lower_bound(m_time.begin(), m_time.end(), time);
    return it == m_time.end() ? m_time.size() - 1 : static_cast<size_t>(std::distance(m_time.begin(), it));
}

std::optional<PrintPositionMatcher::Match> PrintPositionMatcher::match(const PrintPositionSample& sample)
{
    // An exact file position beats any geometric guess.
    if (sample.file_position >= 0) {
        if (std::optional<Match> found = match_file_position(sample.file_position)) {
            m_last      = found->move_id;
            m_last_time = found->time;
            return found;
        }
    }

    bool                     resynced = false;
    std::optional<Candidate> best     = search_near_last(sample.position, sample.layer);
    if (!best || best->distance > NEAR_LAST_MAX_DISTANCE) {
        std::optional<Candidate> global = search_all(sample.position, sample.layer);
        if (global && (!best || global->distance < best->distance)) {
            best     = global;
            resynced = true;
        }
        m_last_distance = best ? best->distance : -1.f;
        if (best && best->distance > SEARCH_ALL_MAX_DISTANCE)
            best.reset();
    } else
        m_last_distance = best->distance;

    if (!best && sample.layer >= 0) {
        // The reported layer may be stale or numbered differently: try again without it.
        best = search_all(sample.position, -1);
        if (best && best->distance > SEARCH_ALL_MAX_DISTANCE)
            best.reset();
        resynced = best.has_value();
        m_last_distance = best ? best->distance : m_last_distance;
    }
    if (!best)
        return std::nullopt;
    double time = time_in_move(*best);
    // Prints only run forwards: jitter in the reports must not move the preview back.
    if (!resynced)
        time = std::max(time, m_last_time);
    m_last      = best->move_id;
    m_last_time = time;
    return Match{ best->move_id, time };
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
    return Match{ move_id, m_time[move_id] };
}

float PrintPositionMatcher::distance_to_move(size_t move_id, const Vec3f& p, float* frac) const
{
    const auto& moves = m_result.moves;
    size_t      prev  = move_id;
    while (prev > 0) {
        --prev;
        if (is_motion(moves[prev])) {
            const Vec3f& a = moves[prev].position;
            const Vec3f& b = moves[move_id].position;
            const Vec3f  ab     = b - a;
            const float  len_sq = ab.squaredNorm();
            const float  t      = len_sq > 0.f ? std::clamp((p - a).dot(ab) / len_sq, 0.f, 1.f) : 1.f;
            if (frac)
                *frac = t;
            return (p - (a + t * ab)).norm();
        }
    }
    if (frac)
        *frac = 1.f;
    return (p - moves[move_id].position).norm();
}

double PrintPositionMatcher::time_in_move(const Candidate& candidate) const
{
    // The move starts when the previous motion move ends.
    const auto& moves = m_result.moves;
    size_t      prev  = candidate.move_id;
    while (prev > 0) {
        --prev;
        if (is_motion(moves[prev]))
            break;
    }
    const double end   = m_time[candidate.move_id];
    const double start = candidate.move_id > 0 ? m_time[prev] : end;
    return start + double(candidate.frac) * (end - start);
}

namespace {
// The printer's layer number may be zero- or one-based, and it changes slightly before or after
// the position does, so accept a layer either side of it. -1 accepts everything.
bool layer_plausible(const GCodeProcessorResult::MoveVertex& move, int layer)
{
    return layer < 0 || std::abs(static_cast<int>(move.layer_id) - layer) <= 1;
}
} // namespace

std::optional<PrintPositionMatcher::Candidate> PrintPositionMatcher::search_near_last(const Vec3f& p, int layer) const
{
    if (!m_last)
        return std::nullopt;

    const auto&  moves = m_result.moves;
    const size_t last  = std::min(*m_last, moves.size() - 1);
    const size_t begin = last > NEAR_LAST_BEHIND ? last - NEAR_LAST_BEHIND : 0;
    const size_t end   = std::min(moves.size(), last + NEAR_LAST_AHEAD);

    std::optional<Candidate> best;
    float                    best_score = std::numeric_limits<float>::max();
    for (size_t i = begin; i < end; ++i) {
        if (!is_motion(moves[i]) || !layer_plausible(moves[i], layer))
            continue;
        float        frac     = 0.f;
        const float distance = distance_to_move(i, p, &frac);
        // Paths cross each other within a layer, so prefer moves just after the last match,
        // and going backwards only for a clearly better fit.
        const float penalty = i >= last ? 0.0005f * float(i - last) : 0.01f * float(last - i);
        if (distance + penalty < best_score) {
            best_score = distance + penalty;
            best       = Candidate{ i, distance, frac };
        }
    }
    return best;
}

std::optional<PrintPositionMatcher::Candidate> PrintPositionMatcher::search_all(const Vec3f& p, int layer) const
{
    const auto&              moves = m_result.moves;
    std::optional<Candidate> best;
    for (size_t i = 0; i < moves.size(); ++i) {
        if (!is_motion(moves[i]) || !layer_plausible(moves[i], layer) ||
            std::abs(moves[i].position.z() - p.z()) > SEARCH_ALL_Z_TOLERANCE)
            continue;
        float        frac     = 0.f;
        const float distance = distance_to_move(i, p, &frac);
        if (!best || distance < best->distance)
            best = Candidate{ i, distance, frac };
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
    m_last_move.reset();
    m_have_target = false;
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
        m_last_move.reset();
        m_have_target = false;
        BOOST_LOG_TRIVIAL(info) << "LivePrintTracker: following " << source_name() << ", " << m_result_moves << " moves";
    }
    return m_source != nullptr;
}

void LivePrintTracker::on_timer(wxTimerEvent&)
{
    if (!sync_with_result())
        return;

    const std::optional<PrintPositionSample> sample = m_source->poll();
    if (sample) {
        ++m_stat_samples;
        const std::optional<PrintPositionMatcher::Match> match = m_matcher->match(*sample);
        if (match) {
            ++m_stat_matched;
            if (!m_have_target || match->time < m_shown_time - RESYNC_BEHIND || match->time > m_shown_time + RESYNC_AHEAD)
                m_shown_time = match->time; // first match or lost track: jump
            m_target_time  = match->time;
            m_target_stamp = std::chrono::steady_clock::now();
            m_have_target  = true;
        }
        m_stat_last = (boost::format("pos (%1$.2f, %2$.2f, %3$.2f) layer %4% -> %5%, distance %6$.2f mm") % sample->position.x() %
                       sample->position.y() % sample->position.z() % sample->layer %
                       (match ? "move " + std::to_string(match->move_id) + " t=" + std::to_string(match->time) :
                                std::string("no match")) %
                       m_matcher->last_distance())
                          .str();
    }

    const auto now = std::chrono::steady_clock::now();

    // Run the display clock at real speed, steered towards the newest matched time (extrapolated
    // at real speed since it arrived). It never runs backwards.
    if (m_have_target) {
        const double elapsed = std::min(std::chrono::duration<double>(now - m_last_tick).count(), 0.2);
        const double since   = std::clamp(std::chrono::duration<double>(now - m_target_stamp).count(), 0., MAX_EXTRAPOLATE);
        const double error   = m_target_time + since - m_shown_time;
        const double rate    = std::clamp(1. + CLOCK_GAIN * error, 0., CLOCK_MAX_RATE);
        m_shown_time += rate * elapsed;
        const size_t shown = m_matcher->move_at_time(m_shown_time);
        if (!m_last_move || shown != *m_last_move) {
            m_last_move = shown;
            if (m_on_move)
                m_on_move(shown);
        }
    }
    m_last_tick = now;

    if (now - m_stat_start >= std::chrono::seconds(5)) {
        BOOST_LOG_TRIVIAL(warning) << "LivePrintTracker: " << source_name() << ": " << m_stat_samples << " samples, " << m_stat_matched
                                   << " matched in 5 s. Last: " << (m_stat_last.empty() ? "none yet" : m_stat_last);
        m_stat_samples = 0;
        m_stat_matched = 0;
        m_stat_start   = now;
    }
}

} // namespace GUI
} // namespace Slic3r
