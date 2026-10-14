#ifndef slic3r_ChangeMaterialDialog_hpp_
#define slic3r_ChangeMaterialDialog_hpp_

// Guided filament change on a Creality Klipper printer:
//   pause (or home and park when idle) -> heat -> push the old material out ->
//   ask for the new material -> purge if it is a different material -> resume.

#include "nlohmann/json.hpp"

#include <wx/dialog.h>
#include <wx/timer.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>

class wxStaticText;
class SpinInput;
class Button;

namespace Slic3r {
namespace GUI {

class CrealityPrinterSocket;

class ChangeMaterialDialog : public wxDialog
{
public:
    // address: the printer's LAN address, as the Device page knows it.
    ChangeMaterialDialog(wxWindow* parent, const std::string& address, const wxString& printer_name);
    ~ChangeMaterialDialog() override;

private:
    enum class Step
    {
        Connecting,
        Ready,
        Pausing,
        Parking,
        Heating,
        PushingOut,
        WaitingForMaterial,
        Purging,
        Done,
        RestoringTemperature,
        Failed,
    };

    void on_status(const nlohmann::json& status);
    void on_connection(bool connected, const std::string& error);
    void on_tick(wxTimerEvent& evt);

    void on_start();
    void on_new_material();
    void on_same_material();
    void on_finish();
    void on_cancel();

    void go_to(Step step);
    void start_heating();
    void extrude(double length_mm, Step step);
    void fail(const wxString& message);
    void update_ui();
    bool step_timed_out(std::chrono::seconds limit) const;

    std::unique_ptr<CrealityPrinterSocket> m_printer;
    wxTimer                                m_timer;
    std::string                            m_address;

    Step                                   m_step{ Step::Connecting };
    std::chrono::steady_clock::time_point  m_step_started;
    // When the current extrusion is expected to be finished.
    std::chrono::steady_clock::time_point  m_extrude_done;

    // Latest printer status.
    int                   m_printer_state{ -1 };
    std::optional<double> m_nozzle_temp;
    std::optional<double> m_nozzle_target;
    std::optional<double> m_x, m_y, m_z;

    // What to put back afterwards.
    bool   m_resume_print{ false };
    double m_print_temp{ 0.0 };
    int    m_change_temp{ 245 };
    wxString m_error;

    wxStaticText* m_title{ nullptr };
    wxStaticText* m_message{ nullptr };
    wxStaticText* m_temperature{ nullptr };
    SpinInput*    m_temp_input{ nullptr };
    Button*       m_primary{ nullptr };
    Button*       m_secondary{ nullptr };
    Button*       m_cancel{ nullptr };
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_ChangeMaterialDialog_hpp_
