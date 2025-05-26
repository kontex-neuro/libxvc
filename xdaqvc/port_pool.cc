#include "port_pool.h"

#include <spdlog/spdlog.h>

#include <optional>
#include <random>

PortPool::PortPool(Port start, Port end) : _start(start), _end(end)
{
    if (start >= end) {
        throw std::invalid_argument("Invalid port range");
    }

    for (auto port = start; port < end; ++port) {
        _available_ports.insert(port);
    }
}

PortPool::~PortPool()
{
    for (auto &[port, acceptor] : _bound_ports) {
        boost::system::error_code ec;
        acceptor->close(ec);
        if (ec) {
            spdlog::warn("Failed to close acceptor on port {}: {}", port, ec.message());
        }
    }
}

std::optional<PortPool::Port> PortPool::allocate_port()
{
    if (_available_ports.empty()) {
        spdlog::warn("No available ports");
        return std::nullopt;
    }

    std::vector<Port> ports(_available_ports.begin(), _available_ports.end());

    std::random_device rd;
    std::mt19937 gen(rd());
    std::shuffle(ports.begin(), ports.end(), gen);

    for (auto port : ports) {
        boost::system::error_code ec;

        auto acceptor = std::make_shared<boost::asio::ip::tcp::acceptor>(_io_context);
        acceptor->open(boost::asio::ip::tcp::v4(), ec);
        if (ec) {
            spdlog::debug("Failed to open acceptor on port {}: {}", port, ec.message());
            continue;
        }

        acceptor->set_option(boost::asio::ip::tcp::acceptor::reuse_address(true), ec);
        if (ec) {
            spdlog::debug("Failed to set reuse_address on port {}: {}", port, ec.message());
            continue;
        }

        auto _ = acceptor->bind({boost::asio::ip::tcp::v4(), port}, ec);
        if (ec == boost::asio::error::address_in_use) {
            spdlog::debug("Failed to bind port {}: {}", port, ec.message());
            continue;
        }

        _bound_ports[port] = acceptor;
        _available_ports.erase(port);

        spdlog::info("Allocated port {}", port);
        return port;
    }

    spdlog::warn("No ports could be bound successfully");
    return std::nullopt;
}

void PortPool::release_port(Port port)
{
    // if (_start <= port && port < _end) {
    //     _available_ports.insert(port);
    //     _bound_ports.erase(port);
    //     fmt::println("Released and unbound port {}", port);
    // }
    if (port < _start || port >= _end) {
        spdlog::warn("Attempted to release port {} outside of range", port);
        return;
    }

    auto it = _bound_ports.find(port);
    if (it != _bound_ports.end()) {
        boost::system::error_code ec;
        it->second->close(ec);
        if (ec) {
            spdlog::warn("Failed to close acceptor on port {}: {}", port, ec.message());
        }

        _bound_ports.erase(it);
    }

    _available_ports.insert(port);
    spdlog::info("Released port {}", port);
    // fmt::println("Port {} is not in the valid range", port);
}

void PortPool::print_available_ports() const
{
    std::string ports;
    for (const auto port : _available_ports) {
        ports += std::to_string(port) + " ";
    }
    spdlog::info("Available Ports: {}", ports);
}