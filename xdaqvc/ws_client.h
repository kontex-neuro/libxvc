#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#ifndef _WIN32_WINDOWS
#define _WIN32_WINDOWS 0x0601
#endif

#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <memory>
#include <thread>


namespace beast = boost::beast;          // from <boost/beast.hpp>
namespace websocket = beast::websocket;  // from <boost/beast/websocket.hpp>
namespace net = boost::asio;             // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;        // from <boost/asio/ip/tcp.hpp>


namespace xvc
{

// Sends a WebSocket message and prints the response
class session : public std::enable_shared_from_this<session>
{
    tcp::resolver _resolver;
    websocket::stream<beast::tcp_stream> _ws;
    beast::flat_buffer _buffer;
    std::string _host;
    std::function<void(std::string)> _event_handler;

public:
    // Resolver and socket require an io_context
    explicit session(net::io_context &ioc, std::function<void(std::string)> handler);

    // Start the asynchronous operation
    void run(char const *host, char const *port);

    void on_resolve(beast::error_code ec, tcp::resolver::results_type results);
    void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep);
    void on_handshake(beast::error_code ec);

    void read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void close();
    void on_close(beast::error_code ec);
};

class ws_client
{
public:
    ws_client(
        const std::string &host, const std::string &port, std::function<void(std::string)> handler
    );
    ~ws_client();
    
private:
    std::shared_ptr<session> _session;
    std::unique_ptr<net::io_context> _ioc;
    std::jthread _runner;
    std::function<void(std::string)> _event_handler;
};

}  // namespace xvc