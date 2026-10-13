#ifndef slic3r_LivePrintTracker_hpp_
#define slic3r_LivePrintTracker_hpp_

// Live print preview: follows a running print in the G-code preview.
//
// A PrintPositionSource reports where the printer is (a real printer, or a simulation of
// the sliced G-code). The PrintPositionMatcher turns each report into an index into
// GCodeProcessorResult::moves, and the LivePrintTracker polls the source on a timer and
// hands that move index to the preview canvas, which moves its sliders there.

#include "libslic3r/Point.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <wx/event.h>
#include <wx/timer.h>

namespace Slic3r {

struct GCodeProcessorResult;

namespace GUI {

// One report of where the printer is, in the same coordinates as GCodeProcessorResult::moves.
struct PrintPositionSample
{
    Vec3f     position{ Vec3f::Zero() };
    // Zero-based layer the printer says it is on, -1 if unknown.
    int       layer{ -1 };
    // Byte offset of the G-code line being executed (Klipper/Moonraker virtual_sdcard.file_position), -1 if unknown.
    long long file_position{ -1 };
};

class PrintPositionSource
{
public:
    virtual ~PrintPositionSource() = default;
    virtual std::string name() const = 0;
    // Latest report, or nothing if the printer has not reported a position yet.
    virtual std::optional<PrintPositionSample> poll() = 0;
};

// Pretends to be a printer running the sliced G-code, so the live preview can be developed
// and tested without a printer. Reports the nozzle position (as Creality's websocket
// "curPosition" does), not the file position.
class SimulatedPrintSource : public PrintPositionSource
{
public:
    // speed: how many times faster than the estimated print time to run.
    SimulatedPrintSource(const GCodeProcessorResult& result, double speed);

    std::string name() const override;
    std::optional<PrintPositionSample> poll() override;

private:
    struct Step
    {
        Vec3f position;
        float end_time; // s since print start when the nozzle reaches position
        int   layer;
    };

    std::vector<Step>                     m_steps;
    double                                m_speed;
    std::chrono::steady_clock::time_point m_start;
    size_t                                m_cursor{ 0 };
};

// Maps printer reports to the move being executed.
class PrintPositionMatcher
{
public:
    explicit PrintPositionMatcher(const GCodeProcessorResult& result);

    // Index into GCodeProcessorResult::moves, or nothing if no move is a plausible match.
    std::optional<size_t> match(const PrintPositionSample& sample);

private:
    struct Candidate
    {
        size_t move_id;
        float  distance;
    };

    std::optional<size_t>    match_file_position(long long file_position) const;
    // Distance from p to the path the nozzle follows while executing move_id.
    float                    distance_to_move(size_t move_id, const Vec3f& p) const;
    std::optional<Candidate> search_near_last(const Vec3f& p) const;
    std::optional<Candidate> search_all(const Vec3f& p) const;

    const GCodeProcessorResult& m_result;
    std::optional<size_t>       m_last;
};

class LivePrintTracker : public wxEvtHandler
{
public:
    using ResultGetter  = std::function<const GCodeProcessorResult*()>;
    using SourceFactory = std::function<std::unique_ptr<PrintPositionSource>(const GCodeProcessorResult&)>;
    using MoveCallback  = std::function<void(size_t move_id)>;

    LivePrintTracker(ResultGetter get_result, MoveCallback on_move);
    ~LivePrintTracker() override;

    // The source is (re)created by make_source whenever the sliced G-code changes.
    void start(SourceFactory make_source);
    void stop();
    bool is_running() const { return m_timer.IsRunning(); }
    std::string source_name() const;

private:
    void on_timer(wxTimerEvent& evt);
    // Recreates the source and matcher if the preview now shows different G-code. False if there is none.
    bool sync_with_result();

    ResultGetter                          m_get_result;
    MoveCallback                          m_on_move;
    SourceFactory                         m_make_source;
    wxTimer                               m_timer;
    const GCodeProcessorResult*           m_result{ nullptr };
    size_t                                m_result_moves{ 0 };
    std::unique_ptr<PrintPositionSource>  m_source;
    std::unique_ptr<PrintPositionMatcher> m_matcher;
    std::optional<size_t>                 m_last_move;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_LivePrintTracker_hpp_
