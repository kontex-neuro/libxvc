#pragma once

#include <boost/asio.hpp>
#include <optional>
#include <unordered_map>
#include <unordered_set>

class PortPool
{
public:
    using Port = unsigned short;

    explicit PortPool(Port start, Port end);
    ~PortPool();

    [[nodiscard]] std::optional<Port> allocate_port();
    void release_port(Port port);
    void print_available_ports() const;

private:
    std::unordered_map<Port, std::shared_ptr<boost::asio::ip::tcp::acceptor>> _bound_ports;

    std::unordered_set<Port> _available_ports;
    Port _start, _end;
    boost::asio::io_context _io_context;
};
