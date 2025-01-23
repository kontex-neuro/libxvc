#pragma once

#include <unordered_set>


class PortPool
{
public:
    using Port = unsigned short;

    explicit PortPool(Port start, Port end);
    ~PortPool() = default;

    [[nodiscard]] Port allocate_port();
    void release_port(Port port);
    void print_available_ports();

private:
    std::unordered_set<Port> _available_ports;
    Port _start;
    Port _end;
};
