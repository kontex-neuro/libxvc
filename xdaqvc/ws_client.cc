#include "ws_client.h"

#include <spdlog/spdlog.h>

namespace http = beast::http;  // from <boost/beast/http.hpp>

// Report a failure
void fail(beast::error_code ec, std::string_view what)
{
    spdlog::debug("{} : {}", what, ec.message());
}

session::session(
    std::string_view host, std::string_view port, net::io_context &ioc,
    std::function<void(std::string_view)> event_handler
)
    : _resolver(net::make_strand(ioc)),
      _ws(net::make_strand(ioc)),
      _host(host),
      _port(port),
      _handler(std::move(event_handler))
{
}

void session::run()
{
    // Look up the domain name
    _resolver.async_resolve(
        _host, _port, beast::bind_front_handler(&session::on_resolve, shared_from_this())
    );
}

void session::on_resolve(beast::error_code ec, tcp::resolver::results_type results)
{
    if (ec) return fail(ec, "resolve");

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
        fail(ec, "connect");
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

    // Update the _host string. This will provide the value of the
    // Host HTTP header during the WebSocket handshake.
    // See https://tools.ietf.org/html/rfc7230#section-5.4
    _host = fmt::format("{}:{}", _host, ep.port());

    // Perform the websocket handshake
    _ws.async_handshake(
        _host, "/ws", beast::bind_front_handler(&session::on_handshake, shared_from_this())
    );
}

void session::on_handshake(beast::error_code ec)
{
    if (ec) return fail(ec, "handshake");

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
        fail(ec, "read");
        reconnect();
        return;
    };

    // Process the received message
    _handler(beast::buffers_to_string(_buffer.data()));

    // Clear the buffer
    _buffer.consume(_buffer.size());

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
    if (ec) return fail(ec, "close");

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

    run();
}

namespace xvc
{

ws_client::ws_client(
    std::string_view host, std::string_view port,
    std::function<void(std::string_view)> event_handler
)
{
    // Launch the asynchronous operation
    auto _session = std::make_shared<session>(
        host, port, _ioc, [handler = std::move(event_handler)](std::string_view event) {
            handler(event);
        }
    );
    _session->run();

    _thread = std::jthread([&]() {
        try {
            // Run the I/O service. The call will return when
            // the socket is closed.
            _ioc.run();
        } catch (const std::exception &e) {
            spdlog::error("WebSocket thread error: {}", e.what());
        }
    });
}

ws_client::~ws_client() { _ioc.stop(); }

}  // namespace xvc