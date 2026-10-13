#include "LivePrintTracker.hpp"

#include "libslic3r/GCode/GCodeProcessor.hpp"

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r {
namespace GUI {

namespace {

// How often the tracker asks the source for a new position.
constexpr int   POLL_INTERVAL_MS = 100;
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
        if (best && best->distance > SEARCH_ALL_MAX_DISTANCE)
            best.reset();
    }

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
    if (!sample)
        return;

    const std::optional<size_t> move_id = m_matcher->match(*sample);
    if (!move_id || move_id == m_last_move)
        return;

    m_last_move = move_id;
    if (m_on_move)
        m_on_move(*move_id);
}

} // namespace GUI
} // namespace Slic3r
