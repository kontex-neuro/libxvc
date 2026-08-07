#pragma once

#include <boost/asio.hpp>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

class PortPool
{
public:
    using Port = unsigned short;

    // Range: [start, end)
    explicit PortPool(Port start, Port end);
    ~PortPool();

    PortPool(const PortPool &) = delete;
    PortPool(PortPool &&) = delete;
    PortPool &operator=(const PortPool &) = delete;
    PortPool &operator=(PortPool &&) = delete;

    [[nodiscard]] std::optional<Port> allocate();
    bool release(Port port);

    [[nodiscard]] const std::unordered_set<Port> &available_ports() const noexcept
    {
        return _available_ports;
    };

private:
    using tcp = boost::asio::ip::tcp;
    using Acceptor = tcp::acceptor;

    std::unordered_map<Port, std::unique_ptr<Acceptor>> _bound_ports;
    std::unordered_set<Port> _available_ports;
    std::vector<Port> _shuffled_ports;

    bool try_bind(Port port);

    Port _start;
    Port _end;
    boost::asio::io_context _io_context;
};
