#include "CrealityPrinterSocket.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/log/trivial.hpp>

#include <wx/app.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <thread>

namespace Slic3r {
namespace GUI {

namespace asio      = boost::asio;
namespace beast     = boost::beast;
namespace websocket = boost::beast::websocket;
using tcp           = boost::asio::ip::tcp;

namespace {
constexpr const char* PORT = "9999";
constexpr auto        CONNECT_TIMEOUT = std::chrono::seconds(5);
} // namespace

struct CrealityPrinterSocket::Impl : std::enable_shared_from_this<CrealityPrinterSocket::Impl>
{
    std::string        address;
    StatusCallback     on_status;
    ConnectionCallback on_connection;

    asio::io_context                                               io;
    std::unique_ptr<websocket::stream<beast::tcp_stream>>          ws;
    tcp::resolver                                                  resolver{ io };
    beast::flat_buffer                                             buffer;
    std::deque<std::string>                                        outbox;
    bool                                                           open{ false };
    std::thread                                                    thread;
    std::shared_ptr<std::atomic<bool>>                             alive = std::make_shared<std::atomic<bool>>(true);
    // The printer sends partial status updates, so keep the merged state.
    nlohmann::json                                                 status = nlohmann::json::object();

    // Runs fn on the UI thread unless the owner has gone away.
    template<class Fn> void to_ui(Fn&& fn)
    {
        if (wxTheApp == nullptr)
            return;
        auto alive_flag = alive;
        wxTheApp->CallAfter([alive_flag, fn = std::forward<Fn>(fn)]() {
            if (alive_flag->load())
                fn();
        });
    }

    void report_connection(bool connected, const std::string& error)
    {
        BOOST_LOG_TRIVIAL(info) << "CrealityPrinterSocket " << address << (connected ? " connected" : " disconnected: ") << error;
        auto cb = on_connection;
        to_ui([cb, connected, error]() { if (cb) cb(connected, error); });
    }

    void start()
    {
        ws = std::make_unique<websocket::stream<beast::tcp_stream>>(io);
        auto self = shared_from_this();
        resolver.async_resolve(address, PORT, [self](beast::error_code ec, tcp::resolver::results_type results) {
            if (ec)
                return self->fail(ec, "resolve");
            beast::get_lowest_layer(*self->ws).expires_after(CONNECT_TIMEOUT);
            beast::get_lowest_layer(*self->ws).async_connect(results, [self](beast::error_code ec, tcp::endpoint) {
                if (ec)
                    return self->fail(ec, "connect");
                beast::get_lowest_layer(*self->ws).expires_never();
                self->ws->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
                self->ws->async_handshake(self->address + ":" + PORT, "/", [self](beast::error_code ec) {
                    if (ec)
                        return self->fail(ec, "handshake");
                    self->open = true;
                    self->report_connection(true, {});
                    self->read();
                    self->write_next();
                });
            });
        });
        thread = std::thread([self]() { self->io.run(); });
    }

    void fail(beast::error_code ec, const char* what)
    {
        const bool was_open = open;
        open                = false;
        if (ec == websocket::error::closed || ec == asio::error::operation_aborted) {
            if (was_open)
                report_connection(false, "closed");
            return;
        }
        report_connection(false, std::string(what) + ": " + ec.message());
    }

    void read()
    {
        auto self = shared_from_this();
        ws->async_read(buffer, [self](beast::error_code ec, std::size_t) {
            if (ec)
                return self->fail(ec, "read");
            std::string text = beast::buffers_to_string(self->buffer.data());
            self->buffer.consume(self->buffer.size());
            self->handle_message(text);
            self->read();
        });
    }

    void handle_message(const std::string& text)
    {
        // "ok" acknowledges a command or heartbeat; it carries no status.
        if (text == "ok")
            return;
        nlohmann::json message = nlohmann::json::parse(text, nullptr, false);
        if (!message.is_object())
            return;
        if (message.value("ModeCode", std::string()) == "heart_beat") {
            queue("ok");
            return;
        }
        status.merge_patch(message);
        auto cb       = on_status;
        auto snapshot = status;
        to_ui([cb, snapshot]() { if (cb) cb(snapshot); });
    }

    void queue(std::string text)
    {
        auto self = shared_from_this();
        asio::post(io, [self, text = std::move(text)]() mutable {
            self->outbox.push_back(std::move(text));
            if (self->outbox.size() == 1 && self->open)
                self->write_next();
        });
    }

    void write_next()
    {
        if (outbox.empty() || !open)
            return;
        auto self = shared_from_this();
        ws->text(true);
        ws->async_write(asio::buffer(outbox.front()), [self](beast::error_code ec, std::size_t) {
            if (ec)
                return self->fail(ec, "write");
            BOOST_LOG_TRIVIAL(info) << "CrealityPrinterSocket " << self->address << " sent " << self->outbox.front();
            self->outbox.pop_front();
            self->write_next();
        });
    }

    void stop()
    {
        alive->store(false);
        auto self = shared_from_this();
        asio::post(io, [self]() {
            // Drop the TCP connection rather than waiting for the printer to answer a close frame.
            self->open = false;
            if (self->ws) {
                beast::error_code ec;
                beast::get_lowest_layer(*self->ws).socket().close(ec);
            }
            self->io.stop();
        });
        if (thread.joinable()) {
            if (thread.get_id() == std::this_thread::get_id())
                thread.detach();
            else
                thread.join();
        }
    }
};

CrealityPrinterSocket::CrealityPrinterSocket(std::string address, StatusCallback on_status, ConnectionCallback on_connection)
    : m_impl(std::make_shared<Impl>())
{
    m_impl->address       = std::move(address);
    m_impl->on_status     = std::move(on_status);
    m_impl->on_connection = std::move(on_connection);
}

CrealityPrinterSocket::~CrealityPrinterSocket() { close(); }

void CrealityPrinterSocket::connect() { m_impl->start(); }

void CrealityPrinterSocket::close()
{
    if (m_impl)
        m_impl->stop();
}

void CrealityPrinterSocket::send(const nlohmann::json& message) { m_impl->queue(message.dump()); }

} // namespace GUI
} // namespace Slic3r
