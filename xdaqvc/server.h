#pragma once

#include <chrono>


using namespace std::chrono_literals;


namespace xvc
{

enum class Status { OFF, ON };

Status server_status(const std::chrono::milliseconds duration = 500s);

std::string server_logs(
    const std::string &filename = "", const std::chrono::milliseconds duration = 500s
);

}  // namespace xvc