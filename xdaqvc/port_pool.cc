#include "port_pool.h"

#include <spdlog/spdlog.h>


PortPool::PortPool(Port start, Port end)
{
    _start = start;
    _end = end;
    for (auto port = start; port < end; ++port) {
        _available_ports.insert(port);
    }
}

PortPool::Port PortPool::allocate_port()
{
    if (_available_ports.empty()) {
        throw std::runtime_error("No available ports");
    }
    auto port = *_available_ports.begin();
    _available_ports.erase(_available_ports.begin());
    return port;
}

void PortPool::release_port(Port port)
{
    if (_start <= port && port < _end) {
        _available_ports.insert(port);
    }
}

void PortPool::print_available_ports()
{
    spdlog::info("Available Ports: ");
    for (const auto port : _available_ports) {
        spdlog::info("{} ", port);
    }
}
