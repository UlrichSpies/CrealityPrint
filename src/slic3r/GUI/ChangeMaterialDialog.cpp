#include "ChangeMaterialDialog.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/SpinInput.hpp"
#include "print_manage/Device/CrealityPrinterSocket.hpp"

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include <wx/sizer.h>
#include <wx/stattext.h>

#include <charconv>
#include <cmath>
#include <cstdio>

namespace Slic3r {
namespace GUI {

namespace {

// Printer "state" values, as the Device page reads them.
constexpr int STATE_PRINTING = 1;
constexpr int STATE_PAUSED   = 5;
constexpr int STATE_PAUSING  = 6;

// Where the head waits when no print is running (a paused print is parked by the printer's
// own PAUSE macro). Front left of the KE's 220 x 220 bed, raised clear of the bed.
constexpr double PARK_X = 10.0;
constexpr double PARK_Y = 10.0;
constexpr double PARK_Z = 50.0;

constexpr double PUSH_OUT_MM      = 35.0;
constexpr double PURGE_MM         = 50.0;
constexpr double EXTRUDE_MM_PER_S = 3.0;

constexpr int TEMP_MIN     = 180;
constexpr int TEMP_MAX     = 300;
constexpr int TEMP_DEFAULT = 245;
// Close enough to the target to count as heated / cooled back down.
constexpr double TEMP_TOLERANCE = 3.0;

constexpr auto PAUSE_TIMEOUT   = std::chrono::seconds(180);
constexpr auto PARK_TIMEOUT    = std::chrono::seconds(180);
constexpr auto HEAT_TIMEOUT    = std::chrono::seconds(420);
constexpr auto CONNECT_TIMEOUT = std::chrono::seconds(15);

// Status values arrive as numbers or as strings, depending on the field and firmware.
std::optional<double> to_number(const nlohmann::json& status, const char* key)
{
    auto it = status.find(key);
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

// The source is UTF-8 but the compiler reads narrow literals as ANSI, so spell the degree sign out.
wxString degrees_celsius() { return wxString::FromUTF8("\xC2\xB0" "C"); }

wxString format_temp(double value) { return wxString::Format("%.0f", value) + degrees_celsius(); }

} // namespace

ChangeMaterialDialog::ChangeMaterialDialog(wxWindow* parent, const std::string& address, const wxString& printer_name)
    : wxDialog(parent, wxID_ANY, _L("Change material"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    , m_address(address)
{
    SetBackgroundColour(*wxWHITE);
    auto* root = new wxBoxSizer(wxVERTICAL);

    m_title = new wxStaticText(this, wxID_ANY, wxString::Format(_L("Change material on %s"), printer_name));
    m_title->SetFont(::Label::Head_16);
    root->Add(m_title, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(20));

    m_message = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(420), FromDIP(70)));
    m_message->SetFont(::Label::Body_14);
    root->Add(m_message, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(20));

    auto* temp_row = new wxBoxSizer(wxHORIZONTAL);
    auto* temp_label = new wxStaticText(this, wxID_ANY, _L("Nozzle temperature for the change"));
    temp_label->SetFont(::Label::Body_14);
    temp_row->Add(temp_label, 0, wxALIGN_CENTER_VERTICAL);
    temp_row->AddStretchSpacer();
    m_temp_input = new SpinInput(this, wxString::Format("%d", TEMP_DEFAULT), degrees_celsius(), wxDefaultPosition, wxSize(FromDIP(100), -1), 0,
                                 TEMP_MIN, TEMP_MAX, TEMP_DEFAULT);
    temp_row->Add(m_temp_input, 0, wxALIGN_CENTER_VERTICAL);
    root->Add(temp_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(20));

    m_temperature = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_temperature->SetFont(::Label::Body_13);
    m_temperature->SetForegroundColour(wxColour(110, 110, 115));
    root->Add(m_temperature, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(8));

    StateColor green(std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Disabled),
                     std::pair<wxColour, int>(wxColour(23, 204, 95), StateColor::Pressed),
                     std::pair<wxColour, int>(wxColour(68, 205, 122), StateColor::Hovered),
                     std::pair<wxColour, int>(wxColour(23, 204, 95), StateColor::Normal));
    StateColor grey(std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Hovered),
                    std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Normal));
    const wxSize button_size(FromDIP(130), FromDIP(32));
    auto make_button = [&](const wxString& label, const StateColor& bg, const wxColour& fg) {
        auto* button = new Button(this, label);
        button->SetBackgroundColor(bg);
        button->SetBorderColor(bg);
        button->SetTextColor(fg);
        button->SetMinSize(button_size);
        button->SetCornerRadius(FromDIP(4));
        return button;
    };
    m_primary   = make_button(_L("Start"), green, *wxWHITE);
    m_secondary = make_button(_L("Same material"), green, *wxWHITE);
    m_cancel    = make_button(_L("Cancel"), grey, wxColour(51, 51, 51));

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer();
    buttons->Add(m_primary, 0, wxRIGHT, FromDIP(10));
    buttons->Add(m_secondary, 0, wxRIGHT, FromDIP(10));
    buttons->Add(m_cancel, 0);
    root->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(20));

    SetSizerAndFit(root);
    CentreOnParent();
    wxGetApp().UpdateDlgDarkUI(this);

    m_primary->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        switch (m_step) {
        case Step::Ready: on_start(); break;
        case Step::WaitingForMaterial: on_new_material(); break;
        case Step::Done: on_finish(); break;
        default: break;
        }
    });
    m_secondary->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_step == Step::WaitingForMaterial)
            on_same_material();
    });
    m_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_cancel(); });
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { on_cancel(); });

    m_timer.SetOwner(this);
    Bind(wxEVT_TIMER, &ChangeMaterialDialog::on_tick, this, m_timer.GetId());

    m_printer = std::make_unique<CrealityPrinterSocket>(
        address, [this](const nlohmann::json& status) { on_status(status); },
        [this](bool connected, const std::string& error) { on_connection(connected, error); });
    go_to(Step::Connecting);
    m_printer->connect();
    m_timer.Start(250);
}

ChangeMaterialDialog::~ChangeMaterialDialog()
{
    m_timer.Stop();
    if (m_printer)
        m_printer->close();
}

void ChangeMaterialDialog::on_connection(bool connected, const std::string& error)
{
    if (connected) {
        if (m_step == Step::Connecting)
            go_to(Step::Ready);
        return;
    }
    if (m_step == Step::Done || m_step == Step::Failed)
        return;
    fail(wxString::Format(_L("Lost the connection to the printer (%s)."), wxString::FromUTF8(error)));
}

void ChangeMaterialDialog::on_status(const nlohmann::json& status)
{
    if (auto state = to_number(status, "state"))
        m_printer_state = static_cast<int>(*state);
    m_nozzle_temp   = to_number(status, "nozzleTemp");
    m_nozzle_target = to_number(status, "targetNozzleTemp");

    // "curPosition": "X:110.00 Y:10.00 Z:50.00"
    auto it = status.find("curPosition");
    if (it != status.end() && it->is_string()) {
        // Not sscanf: %lf follows the locale, and a decimal-comma locale rejects "133.97".
        const std::string text = it->get<std::string>();
        double axis[3] = {0., 0., 0.};
        int    found   = 0;
        for (const char* p = text.c_str(); *p != '\0' && found < 3; ++p) {
            if (*p == 'X' + found && p[1] == ':') {
                const auto res = std::from_chars(p + 2, text.c_str() + text.size(), axis[found]);
                if (res.ec != std::errc())
                    break;
                ++found;
                p = res.ptr - 1;
            }
        }
        if (found == 3) {
            m_x = axis[0];
            m_y = axis[1];
            m_z = axis[2];
        }
    }
    update_ui();
}

void ChangeMaterialDialog::on_tick(wxTimerEvent&)
{
    const auto now = std::chrono::steady_clock::now();
    switch (m_step) {
    case Step::Connecting:
        if (step_timed_out(CONNECT_TIMEOUT))
            fail(wxString::Format(_L("Could not connect to the printer at %s."), wxString::FromUTF8(m_address)));
        break;
    case Step::Pausing:
        if (m_printer_state == STATE_PAUSED)
            start_heating();
        else if (step_timed_out(PAUSE_TIMEOUT))
            fail(_L("The printer did not pause."));
        break;
    case Step::Parking:
        if (m_x && m_y && m_z && std::abs(*m_x - PARK_X) < 1.0 && std::abs(*m_y - PARK_Y) < 1.0 && std::abs(*m_z - PARK_Z) < 1.0)
            start_heating();
        else if (step_timed_out(PARK_TIMEOUT))
            fail(_L("The print head did not reach the park position."));
        break;
    case Step::Heating:
        if (m_nozzle_temp && *m_nozzle_temp >= m_change_temp - TEMP_TOLERANCE)
            extrude(PUSH_OUT_MM, Step::PushingOut);
        else if (step_timed_out(HEAT_TIMEOUT))
            fail(_L("The nozzle did not reach the temperature."));
        break;
    case Step::PushingOut:
        if (now >= m_extrude_done)
            go_to(Step::WaitingForMaterial);
        break;
    case Step::Purging:
        if (now >= m_extrude_done)
            go_to(Step::Done);
        break;
    case Step::RestoringTemperature:
        if (m_nozzle_temp && std::abs(*m_nozzle_temp - m_print_temp) <= TEMP_TOLERANCE) {
            m_printer->set_paused(false);
            EndModal(wxID_OK);
        } else if (step_timed_out(HEAT_TIMEOUT)) {
            fail(_L("The nozzle did not return to the print temperature. The print is still paused."));
        }
        break;
    default: break;
    }
}

void ChangeMaterialDialog::on_start()
{
    m_change_temp = m_temp_input->GetValue();

    if (m_printer_state == STATE_PRINTING || m_printer_state == STATE_PAUSING || m_printer_state == STATE_PAUSED) {
        // The printer's PAUSE macro parks the head; RESUME brings it back.
        m_resume_print = true;
        m_print_temp   = m_nozzle_target.value_or(0.0);
        if (m_printer_state == STATE_PRINTING)
            m_printer->set_paused(true);
        if (m_printer_state == STATE_PAUSED)
            start_heating();
        else
            go_to(Step::Pausing);
        return;
    }

    m_resume_print = false;
    m_printer->send_gcode((boost::format("G28\nG90\nG1 Z%1% F600\nG1 X%2% Y%3% F6000") % PARK_Z % PARK_X % PARK_Y).str());
    go_to(Step::Parking);
}

void ChangeMaterialDialog::start_heating()
{
    m_printer->send_gcode((boost::format("M104 S%1%") % m_change_temp).str());
    go_to(Step::Heating);
}

void ChangeMaterialDialog::extrude(double length_mm, Step step)
{
    m_printer->send_gcode((boost::format("M83\nG1 E%1% F%2%") % length_mm % (EXTRUDE_MM_PER_S * 60.0)).str());
    // A little extra for the command to reach the printer and start.
    m_extrude_done = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(static_cast<int>(length_mm / EXTRUDE_MM_PER_S * 1000.0) + 3000);
    go_to(step);
}

void ChangeMaterialDialog::on_new_material() { extrude(PURGE_MM, Step::Purging); }

void ChangeMaterialDialog::on_same_material() { go_to(Step::Done); }

void ChangeMaterialDialog::on_finish()
{
    if (!m_resume_print) {
        m_printer->send_gcode("M104 S0");
        EndModal(wxID_OK);
        return;
    }
    // Resuming at the change temperature would print the rest too hot or too cold.
    if (m_print_temp > 0.0 && std::abs(m_print_temp - m_change_temp) > TEMP_TOLERANCE) {
        m_printer->send_gcode((boost::format("M104 S%1%") % m_print_temp).str());
        go_to(Step::RestoringTemperature);
        return;
    }
    m_printer->set_paused(false);
    EndModal(wxID_OK);
}

void ChangeMaterialDialog::on_cancel()
{
    // Leave a paused print paused (it can be resumed from the Device page) at its own
    // temperature, and don't leave the heater on for nothing when no print is running.
    const bool heated = m_step != Step::Connecting && m_step != Step::Ready && m_step != Step::Pausing && m_step != Step::Parking;
    if (heated && m_printer) {
        if (!m_resume_print)
            m_printer->send_gcode("M104 S0");
        else if (m_print_temp > 0.0)
            m_printer->send_gcode((boost::format("M104 S%1%") % m_print_temp).str());
    }
    m_timer.Stop();
    EndModal(wxID_CANCEL);
}

void ChangeMaterialDialog::fail(const wxString& message)
{
    m_error = message;
    BOOST_LOG_TRIVIAL(warning) << "ChangeMaterialDialog: " << message.ToUTF8().data();
    go_to(Step::Failed);
}

bool ChangeMaterialDialog::step_timed_out(std::chrono::seconds limit) const
{
    return std::chrono::steady_clock::now() - m_step_started > limit;
}

void ChangeMaterialDialog::go_to(Step step)
{
    m_step         = step;
    m_step_started = std::chrono::steady_clock::now();
    update_ui();
}

void ChangeMaterialDialog::update_ui()
{
    wxString message;
    wxString primary;
    wxString secondary;
    wxString cancel = _L("Cancel");
    switch (m_step) {
    case Step::Connecting: message = _L("Connecting to the printer..."); break;
    case Step::Ready:
        message = m_printer_state == STATE_PRINTING ? _L("A print is running. Start pauses it, parks the head and heats the nozzle.") :
                  m_printer_state == STATE_PAUSED   ? _L("The print is paused. Start heats the nozzle.") :
                                                      _L("Start homes the printer, parks the head and heats the nozzle.");
        primary = _L("Start");
        break;
    case Step::Pausing: message = _L("Pausing the print..."); break;
    case Step::Parking: message = _L("Homing and parking the print head..."); break;
    case Step::Heating: message = wxString::Format(_L("Heating the nozzle to %s..."), format_temp(m_change_temp)); break;
    case Step::PushingOut:
        message = wxString::Format(_L("Pushing the old material out (%.0f mm)..."), PUSH_OUT_MM);
        break;
    case Step::WaitingForMaterial:
        message   = _L("Insert the new material from the top until it reaches the nozzle.\n"
                       "New material purges another 50 mm to clear the old type out.");
        primary   = _L("New material");
        secondary = _L("Same material");
        break;
    case Step::Purging: message = wxString::Format(_L("Purging the old material (%.0f mm)..."), PURGE_MM); break;
    case Step::Done:
        message = m_resume_print ? _L("Done. Remove any purged material from the nozzle, then resume the print.") :
                                   _L("Done. Remove any purged material from the nozzle. Finish turns the heater off.");
        primary = m_resume_print ? _L("Resume print") : _L("Finish");
        break;
    case Step::RestoringTemperature:
        message = wxString::Format(_L("Returning the nozzle to the print temperature (%s), then resuming..."), format_temp(m_print_temp));
        break;
    case Step::Failed:
        message = m_error + (m_resume_print ? "\n" + _L("The print stays paused; resume it from the Device page.") : wxString());
        cancel  = _L("Close");
        break;
    }

    m_message->SetLabel(message);
    m_message->Wrap(FromDIP(420));

    m_primary->Show(!primary.IsEmpty());
    if (!primary.IsEmpty())
        m_primary->SetLabel(primary);
    m_secondary->Show(!secondary.IsEmpty());
    if (!secondary.IsEmpty())
        m_secondary->SetLabel(secondary);
    m_cancel->SetLabel(cancel);
    m_temp_input->Enable(m_step == Step::Ready);

    if (m_nozzle_temp)
        m_temperature->SetLabel(wxString::Format(_L("Nozzle: %s / %s"), format_temp(*m_nozzle_temp),
                                                 format_temp(m_nozzle_target.value_or(0.0))));
    else
        m_temperature->SetLabel(m_step == Step::Connecting ? wxString() : _L("Nozzle: waiting for the printer..."));

    Layout();
}

} // namespace GUI
} // namespace Slic3r
