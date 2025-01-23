#pragma once

#include <chrono>


using namespace std::chrono_literals;


namespace xvc
{

enum class Status { OFF, ON };

Status server_status(const std::chrono::milliseconds timeout = 1s);

}  // namespace xvc