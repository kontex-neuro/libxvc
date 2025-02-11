#include "ws_client.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <memory>


namespace http = beast::http;  // from <boost/beast/http.hpp>


namespace
{
auto constexpr RESOLVE = "resolve";
auto constexpr CONNECT = "connect";
auto constexpr HANDSHAKE = "handshake";
auto constexpr READ = "read";
auto constexpr CLOSE = "close";
auto constexpr ROUTE = "/ws";

// Report a failure
void fail(beast::error_code ec, char const *what) { spdlog::error("{} : {}", what, ec.message()); }

}  // namespace

namespace xvc
{

session::session(net::io_context &ioc, std::function<void(std::string)> handler)
    : _resolver(net::make_strand(ioc)),
      _ws(net::make_strand(ioc)),
      _event_handler(std::move(handler))
{
}

void session::run(char const *host, char const *port)
{
    _host = host;

    // Look up the domain name
    _resolver.async_resolve(
        host, port, beast::bind_front_handler(&session::on_resolve, shared_from_this())
    );
}

void session::on_resolve(beast::error_code ec, tcp::resolver::results_type results)
{
    if (ec) return fail(ec, RESOLVE);

    // Set the timeout for the operation
    beast::get_lowest_layer(_ws).expires_after(std::chrono::seconds(1));

    // Make the connection on the IP address we get from a lookup
    beast::get_lowest_layer(_ws).async_connect(
        results, beast::bind_front_handler(&session::on_connect, shared_from_this())
    );
}

void session::on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep)
{
    if (ec) {
        fail(ec, CONNECT);
        reconnect();
        return;
    };

    // Turn off the timeout on the tcp_stream, because
    // the websocket stream has its own timeout system.
    beast::get_lowest_layer(_ws).expires_never();

    // Set suggested timeout settings for the websocket
    _ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));

    // Set a decorator to change the User-Agent of the handshake
    _ws.set_option(websocket::stream_base::decorator([](websocket::request_type &req) {
        req.set(
            http::field::user_agent,
            std::string(BOOST_BEAST_VERSION_STRING) + " websocket-client-async"
        );
    }));

    // Update the host_ string. This will provide the value of the
    // Host HTTP header during the WebSocket handshake.
    // See https://tools.ietf.org/html/rfc7230#section-5.4
    _host += ':' + std::to_string(ep.port());

    // Perform the websocket handshake
    _ws.async_handshake(
        _host, ROUTE, beast::bind_front_handler(&session::on_handshake, shared_from_this())
    );
}

void session::on_handshake(beast::error_code ec)
{
    if (ec) return fail(ec, HANDSHAKE);

    read();
}

void session::read()
{
    _ws.async_read(_buffer, beast::bind_front_handler(&session::on_read, shared_from_this()));
}

void session::on_read(beast::error_code ec, std::size_t bytes_transferred)
{
    boost::ignore_unused(bytes_transferred);

    if (ec) {
        fail(ec, READ);
        reconnect();
        return;
    };

    // Process the received message
    auto const event = beast::buffers_to_string(_buffer.data());
    _event_handler(event);

    // Clear the buffer
    _buffer.clear();
    // _buffer.consume(_buffer.size());

    read();
}

void session::close()
{
    // Close the WebSocket connection
    _ws.async_close(
        websocket::close_code::normal,
        beast::bind_front_handler(&session::on_close, shared_from_this())
    );
}

void session::on_close(beast::error_code ec)
{
    if (ec) return fail(ec, CLOSE);

    // If we get here then the connection is closed gracefully

    spdlog::debug("WebSocket closed gracefully");
}

void session::reconnect(const std::chrono::milliseconds timeout)
{
    spdlog::debug("session has been disconnected, trying to reconnect...");

    if (_ws.is_open()) {
        close();
    }

    spdlog::debug("next trial will start after {}ms", timeout.count());
    std::this_thread::sleep_for(timeout);

    auto const host = "192.168.177.100";
    auto const port = "8000";

    run(host, port);
}

ws_client::ws_client(std::function<void(std::string)> handler) : _event_handler(std::move(handler))
{
    _ioc = std::make_unique<net::io_context>();

    _thread = std::jthread([this, host = "192.168.177.100", port = "8000"]() {
        try {
            // Launch the asynchronous operation
            _session = std::make_shared<session>(*_ioc, [this](const std::string &event) {
                _event_handler(event);
            });

            _session->run(host, port);

            // Run the I/O service. The call will return when
            // the socket is closed.
            _ioc->run();

            spdlog::debug("WebSocket closed");
        } catch (const std::exception &e) {
            spdlog::error("WebSocket thread error: {}", e.what());
        }
    });
}

ws_client::~ws_client()
{
    // _ioc->stop();
    // _session->close();
}

}  // namespace xvc