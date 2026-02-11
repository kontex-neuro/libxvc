#pragma once

#include <expected>
#include <nlohmann/json.hpp>

std::expected<void, std::string> validate_camera(const nlohmann::json &json);
