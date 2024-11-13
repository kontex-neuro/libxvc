#pragma once

#include <unordered_set>


class PortPool
{
public:
    explicit PortPool(unsigned short start, unsigned short end);
    [[nodiscard]] unsigned short allocate_port();
    void release_port(unsigned short port);
    void print_available_ports();

private:
    std::unordered_set<unsigned short> available_ports;
    unsigned short _start;
    unsigned short _end;
};
