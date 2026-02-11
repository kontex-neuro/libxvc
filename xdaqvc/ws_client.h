#pragma once

#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace beast = boost::beast;          // from <boost/beast.hpp>
namespace websocket = beast::websocket;  // from <boost/beast/websocket.hpp>
namespace net = boost::asio;             // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;        // from <boost/asio/ip/tcp.hpp>

// Sends a WebSocket message and prints the response
class session : public std::enable_shared_from_this<session>
{
    tcp::resolver _resolver;
    websocket::stream<beast::tcp_stream> _ws;
    beast::flat_buffer _buffer;
    std::string _host;
    std::string _port;
    std::function<void(std::string)> _handler;

public:
    // Resolver and socket require an io_context
    explicit session(
        std::string host, std::string port, net::io_context &ioc,
        std::function<void(std::string)> handler
    );

    // Start the asynchronous operation
    void run();

    void on_resolve(beast::error_code ec, tcp::resolver::results_type results);
    void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep);
    void on_handshake(beast::error_code ec);

    void read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void close();
    void on_close(beast::error_code ec);

    void reconnect(std::chrono::milliseconds timeout = std::chrono::milliseconds(500));
};

namespace xvc
{

class ws_client
{
public:
    ws_client(
        std::string host = "192.168.177.100", std::string port = "8000",
        std::function<void(std::string)> handler = nullptr
    );
    ~ws_client();

private:
    net::io_context _ioc;
    std::jthread _thread;
};

}  // namespace xvc