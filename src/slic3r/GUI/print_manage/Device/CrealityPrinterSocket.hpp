#ifndef slic3r_CrealityPrinterSocket_hpp_
#define slic3r_CrealityPrinterSocket_hpp_

// A direct connection to a Creality Klipper printer (K1 / Ender 3 V3 KE family) on its
// LAN websocket, ws://<address>:9999 — the same protocol the Device page uses:
//   {"method":"set","params":{"gcodeCmd":"G28"}}   run G-code
//   {"method":"set","params":{"pause":1}}          pause (0 = resume)
// The printer pushes JSON status objects (nozzleTemp, targetNozzleTemp, state, ...) on its own.

#include "nlohmann/json.hpp"

#include <functional>
#include <memory>
#include <string>

namespace Slic3r {
namespace GUI {

class CrealityPrinterSocket
{
public:
    // Called on the UI thread.
    using StatusCallback     = std::function<void(const nlohmann::json& status)>;
    using ConnectionCallback = std::function<void(bool connected, const std::string& error)>;

    CrealityPrinterSocket(std::string address, StatusCallback on_status, ConnectionCallback on_connection);
    ~CrealityPrinterSocket();

    CrealityPrinterSocket(const CrealityPrinterSocket&)            = delete;
    CrealityPrinterSocket& operator=(const CrealityPrinterSocket&) = delete;

    void connect();
    void close();

    void send(const nlohmann::json& message);
    void send_gcode(const std::string& gcode) { send({{"method", "set"}, {"params", {{"gcodeCmd", gcode}}}}); }
    void set_paused(bool paused) { send({{"method", "set"}, {"params", {{"pause", paused ? 1 : 0}}}}); }

private:
    struct Impl;
    std::shared_ptr<Impl> m_impl;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_CrealityPrinterSocket_hpp_
