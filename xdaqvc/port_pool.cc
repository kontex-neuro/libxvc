#include "port_pool.h"

#include <spdlog/spdlog.h>

#include <random>

PortPool::PortPool(Port start, Port end) : _start(start), _end(end)
{
    if (start >= end) {
        throw std::invalid_argument("Invalid port range");
    }

    _available_ports.reserve(_end - _start);
    for (auto port = _start; port < _end; ++port) {
        _available_ports.insert(port);
        _shuffled_ports.push_back(port);
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::shuffle(_shuffled_ports.begin(), _shuffled_ports.end(), gen);
}

PortPool::~PortPool()
{
    for (auto &[port, acceptor] : _bound_ports) {
        boost::system::error_code ec;
        auto _ = acceptor->close(ec);
        if (ec) {
            spdlog::error("Failed to close acceptor on port {}: {}", port, ec.message());
        }
    }
    _bound_ports.clear();
}

std::optional<PortPool::Port> PortPool::allocate()
{
    if (_available_ports.empty()) {
        spdlog::warn("No available ports");
        return std::nullopt;
    }

    for (auto port : _shuffled_ports) {
        if (!_available_ports.contains(port)) {
            continue;
        }
        if (try_bind(port)) {
            _available_ports.erase(port);
            spdlog::debug("Allocated port {}", port);
            return port;
        }
    }
    spdlog::error("No ports could be bound");
    return std::nullopt;
}

bool PortPool::release(Port port)
{
    if (port < _start || port >= _end) {
        spdlog::warn("Failed to release port {}: outside of range", port);
        return false;
    }

    auto it = _bound_ports.find(port);
    if (it == _bound_ports.end()) {
        spdlog::warn("Failed to release port {}: not allocated", port);
        return false;
    }

    boost::system::error_code ec;
    auto _ = it->second->close(ec);
    if (ec) {
        spdlog::error("Failed to close acceptor on port {}: {}", port, ec.message());
    }
    _bound_ports.erase(it);
    _available_ports.insert(port);

    spdlog::debug(
        "Released port {} ({}/{} ports in use)", port, _bound_ports.size(), _end - _start
    );
    return true;
}

bool PortPool::try_bind(Port port)
{
    boost::system::error_code ec;
    auto acceptor = std::make_unique<Acceptor>(_io_context);

    auto _ = acceptor->open(tcp::v4(), ec);
    if (ec) {
        spdlog::error("Failed to open acceptor on port {}: {}", port, ec.message());
        return false;
    }

    _ = acceptor->set_option(Acceptor::reuse_address(true), ec);
    if (ec) {
        spdlog::error("Failed to set reuse_address option on port {}: {}", port, ec.message());
        return false;
    }

    _ = acceptor->bind({tcp::v4(), port}, ec);
    if (ec == boost::asio::error::address_in_use) {
        spdlog::error("Failed to bind port {}: {}", port, ec.message());
        return false;
    }

    _bound_ports.emplace(port, std::move(acceptor));
    return true;
}