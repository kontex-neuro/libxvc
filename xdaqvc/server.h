#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include "common.h"

namespace xvc
{

class Server
{
public:
    explicit Server(std::string_view host = "192.168.177.100", int port = 8000) noexcept;

    bool root(std::chrono::milliseconds timeout = std::chrono::milliseconds(1000)) const;

    std::optional<std::string> logs(
        std::string_view filename = "",
        std::chrono::milliseconds timeout = std::chrono::milliseconds(1000)
    ) const;

    std::optional<Version> api_version(
        std::chrono::milliseconds timeout = std::chrono::milliseconds(1000)
    ) const;

private:
    std::string _base_url;
};

}  // namespace xvc