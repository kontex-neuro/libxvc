#include "server.h"

#include <cpr/api.h>
#include <spdlog/spdlog.h>

#include <format>
#include <nlohmann/json.hpp>

namespace
{
constexpr int OK = 200;
}  // namespace

namespace xvc
{

Server::Server(std::string_view host, int port) noexcept
    : _base_url(std::format("http://{}:{}", host, port))
{
}

bool Server::root(std::chrono::milliseconds timeout) const
{
    auto response = cpr::Get(cpr::Url{_base_url}, cpr::Timeout{timeout});
    if (response.status_code != OK) {
        spdlog::debug("Failed to fetch {} Status: {}", _base_url, response.status_code);
        return false;
    }
    return true;
}

std::optional<std::string> Server::logs(
    std::string_view filename, std::chrono::milliseconds timeout
) const
{
    const auto logs = filename.empty();
    const auto &url = logs ? std::format("{}/{}", _base_url, "logs")
                           : std::format("{}/{}/{}", _base_url, "logs", filename);

    auto response = cpr::Get(cpr::Url{url}, cpr::Timeout{timeout});
    if (response.status_code != OK) {
        spdlog::debug("Failed to fetch {} Status: {}", url, response.status_code);
        return std::nullopt;
    }
    return logs ? nlohmann::json::parse(response.text).dump(2) : response.text;
}

std::optional<Version> Server::api_version(std::chrono::milliseconds timeout) const
{
    const auto &url = std::format("{}/{}", _base_url, "api_version");

    auto response = cpr::Get(cpr::Url{url}, cpr::Timeout{timeout});
    if (response.status_code != OK) {
        spdlog::debug("Failed to fetch {} Status: {}", url, response.status_code);
        return std::nullopt;
    }

    auto text = response.text;
    try {
        auto json = nlohmann::json::parse(text);
        auto version_str = json.at("version").get<std::string>();
        auto version = Version::from_string(version_str);
        if (!version) {
            spdlog::error("Invalid API version format: {}", text);
            return std::nullopt;
        }
        return version.value();
    } catch (const nlohmann::json::parse_error &e) {
        spdlog::error("Failed to parse {} response as JSON: {}", url, text);
        return std::nullopt;
    }
}

}  // namespace xvc