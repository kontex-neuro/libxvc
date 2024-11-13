#include "port_pool.h"

#include <iostream>


PortPool::PortPool(unsigned short start, unsigned short end)
{
    _start = start;
    _end = end;
    for (auto port = start; port < end; ++port) {
        available_ports.insert(port);
    }
}

unsigned short PortPool::allocate_port()
{
    if (available_ports.empty()) {
        throw std::runtime_error("No available ports");
    }
    auto port = *available_ports.begin();
    available_ports.erase(available_ports.begin());
    return port;
}

void PortPool::release_port(unsigned short port)
{
    if (_start <= port && port < _end) {
        available_ports.insert(port);
    }
}

void PortPool::print_available_ports()
{
    std::cout << "Available Ports: ";
    for (const auto &port : available_ports) {
        std::cout << port << " ";
    }
    std::cout << std::endl;
}
