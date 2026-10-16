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
// Further than this between two samples, jump instead of gliding.
constexpr double GLIDE_MAX_MOVES = 4000.;
// A report further than this from every move nearby the last match triggers a full search.
constexpr float NEAR_LAST_MAX_DISTANCE = 0.5f; // mm
// A full search only accepts moves this close to the report.
constexpr float SEARCH_ALL_MAX_DISTANCE = 1.0f; // mm
// Full search only looks at moves within this Z of the report (covers z-hops).
constexpr float SEARCH_ALL_Z_TOLERANCE = 1.5f; // mm
// Window around the last match that is searched first. Prints run forwards, so look further ahead.
constexpr size_t NEAR_LAST_BEHIND = 50;
constexpr size_t NEAR_LAST_AHEAD  = 3000;

// Seams are markers, not motion, so they never describe where the nozzle is.
bool is_motion(const GCodeProcessorResult::MoveVertex& move) { return move.type != EMoveType::Seam; }

float point_segment_distance(const Vec3f& p, const Vec3f& a, const Vec3f& b)
{
    const Vec3f ab     = b - a;
    const float len_sq = ab.squaredNorm();
    if (len_sq <= 0.f)
        return (p - a).norm();
    const float t = std::clamp((p - a).dot(ab) / len_sq, 0.f, 1.f);
    return (p - (a + t * ab)).norm();
}

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

PrintPositionMatcher::PrintPositionMatcher(const GCodeProcessorResult& result) : m_result(result) {}

std::optional<size_t> PrintPositionMatcher::match(const PrintPositionSample& sample)
{
    // An exact file position beats any geometric guess.
    if (sample.file_position >= 0) {
        if (std::optional<size_t> move_id = match_file_position(sample.file_position)) {
            m_last = move_id;
            return move_id;
        }
    }

    std::optional<Candidate> best = search_near_last(sample.position);
    if (!best || best->distance > NEAR_LAST_MAX_DISTANCE) {
        std::optional<Candidate> global = search_all(sample.position);
        if (global && (!best || global->distance < best->distance))
            best = global;
        m_last_distance = best ? best->distance : -1.f;
        if (best && best->distance > SEARCH_ALL_MAX_DISTANCE)
            best.reset();
    } else
        m_last_distance = best->distance;

    if (!best)
        return std::nullopt;
    m_last = best->move_id;
    return best->move_id;
}

std::optional<size_t> PrintPositionMatcher::match_file_position(long long file_position) const
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
    return static_cast<size_t>(std::distance(moves.begin(), move_it));
}

float PrintPositionMatcher::distance_to_move(size_t move_id, const Vec3f& p) const
{
    const auto& moves = m_result.moves;
    size_t      prev  = move_id;
    while (prev > 0) {
        --prev;
        if (is_motion(moves[prev]))
            return point_segment_distance(p, moves[prev].position, moves[move_id].position);
    }
    return (p - moves[move_id].position).norm();
}

std::optional<PrintPositionMatcher::Candidate> PrintPositionMatcher::search_near_last(const Vec3f& p) const
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
        if (!is_motion(moves[i]))
            continue;
        const float distance = distance_to_move(i, p);
        // Paths cross each other within a layer, so prefer moves just after the last match,
        // and going backwards only for a clearly better fit.
        const float penalty = i >= last ? 0.0005f * float(i - last) : 0.01f * float(last - i);
        if (distance + penalty < best_score) {
            best_score = distance + penalty;
            best       = Candidate{ i, distance };
        }
    }
    return best;
}

std::optional<PrintPositionMatcher::Candidate> PrintPositionMatcher::search_all(const Vec3f& p) const
{
    const auto&              moves = m_result.moves;
    std::optional<Candidate> best;
    for (size_t i = 0; i < moves.size(); ++i) {
        if (!is_motion(moves[i]) || std::abs(moves[i].position.z() - p.z()) > SEARCH_ALL_Z_TOLERANCE)
            continue;
        const float distance = distance_to_move(i, p);
        if (!best || distance < best->distance)
            best = Candidate{ i, distance };
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
        const std::optional<size_t> move_id = m_matcher->match(*sample);
        if (move_id) {
            ++m_stat_matched;
            const auto   t     = std::chrono::steady_clock::now();
            const double dt    = std::clamp(std::chrono::duration<double>(t - m_last_target_time).count(), 0.1, 1.0);
            const double delta = double(*move_id) - m_shown;
            if (!m_last_move || delta < 0. || delta > GLIDE_MAX_MOVES) {
                // First match, backwards or a long way: jump.
                m_shown = double(*move_id);
                m_rate  = 0.;
            } else
                m_rate = delta / dt; // arrive about when the next sample is due
            m_target           = *move_id;
            m_last_target_time = t;
            if (!m_last_move) {
                m_last_move = move_id;
                if (m_on_move)
                    m_on_move(*move_id);
            }
        }
        m_stat_last = (boost::format("pos (%1$.2f, %2$.2f, %3$.2f) layer %4% -> %5%, distance %6$.2f mm") % sample->position.x() %
                       sample->position.y() % sample->position.z() % sample->layer %
                       (move_id ? "move " + std::to_string(*move_id) : std::string("no match")) % m_matcher->last_distance())
                          .str();
    }

    const auto now = std::chrono::steady_clock::now();

    // Glide the shown move towards the target.
    if (m_last_move) {
        const double elapsed = std::chrono::duration<double>(now - m_last_tick).count();
        if (m_shown < double(m_target))
            m_shown = std::min(double(m_target), m_shown + m_rate * elapsed);
        const size_t shown = static_cast<size_t>(m_shown);
        if (shown != *m_last_move) {
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
