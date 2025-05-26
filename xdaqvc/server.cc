#include "server.h"

#include <cpr/api.h>
#include <fmt/core.h>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace
{
auto constexpr OK = 200;
}  // namespace

namespace xvc
{

Version::Version(const std::string &version_str) : major(0), minor(0), patch(0)
{
    std::istringstream ss(version_str);
    std::string token;
    std::vector<int> parts;

    while (std::getline(ss, token, '.')) {
        try {
            parts.push_back(std::stoi(token));
        } catch (const std::invalid_argument &) {
            throw std::invalid_argument("Invalid version format: " + version_str);
        }
    }

    if (parts.size() != 3) {
        throw std::invalid_argument(
            "Version must have exactly three components (major.minor.patch): " + version_str
        );
    }

    major = parts[0];
    minor = parts[1];
    patch = parts[2];
}

bool Version::operator==(const Version &other) const
{
    return major == other.major && minor == other.minor && patch == other.patch;
}

bool Version::operator<(const Version &other) const
{
    if (major != other.major) return major < other.major;
    if (minor != other.minor) return minor < other.minor;
    return patch < other.patch;
}

bool Version::operator>(const Version &other) const { return other < *this; }

std::string Version::to_string() const { return fmt::format("{}.{}.{}", major, minor, patch); }

Server::Server(const std::string &host, int port)
    : _base_url(fmt::format("http://{}:{}", host, port))
{
}

Status Server::status(const std::chrono::milliseconds timeout) const
{
    auto response = cpr::Get(cpr::Url{_base_url}, cpr::Timeout{timeout});

    // cpr::Session session;
    // session.SetUrl(url);
    // session.SetTimeout(_timeout);
    // auto response = session.Get();

    return (response.status_code == OK) ? Status::ON : Status::OFF;
}

std::string Server::logs(const std::string &filename, const std::chrono::milliseconds timeout) const
{
    auto url = fmt::format("{}/{}", _base_url, "logs");
    if (!filename.empty()) {
        url += "/" + filename;
    }
    spdlog::debug("Fetching logs from = {}", url);

    auto response = cpr::Get(cpr::Url{url}, cpr::Timeout{timeout});
    if (response.status_code != OK) {
        spdlog::error("Failed to fetch logs. Status: {}", response.status_code);
        return "";
    }

    return response.text;
}

std::optional<std::string> Server::get_api_version(const std::chrono::milliseconds timeout) const
{
    auto url = fmt::format("{}/{}", _base_url, "version");
    auto response = cpr::Get(cpr::Url{url}, cpr::Timeout{timeout});

    if (response.status_code != OK) {
        spdlog::warn("Failed to fetch API version. Status: {}", response.status_code);
        return std::nullopt;
    }
    return response.text;
}

bool Server::update(const Version &update_version, const std::chrono::milliseconds timeout) const
{
    auto api_version = get_api_version();
    if (!api_version) {
        spdlog::error("Failed to get server API version.");
        return false;
    }

    Version current_version{json::parse(api_version.value())["version"].get<std::string>()};

    if (current_version == update_version) {
        spdlog::info(
            "Skip update: current version {} = update version {}",
            current_version.to_string(),
            update_version.to_string()
        );
        return true;
    }

    // TODO
    // json payload = {{"version", update_version.to_string()}};

    // auto url = fmt::format("{}/{}", _base_url, "update");

    // auto response = cpr::Post(
    //     cpr::Url{url},
    //     cpr::Header{{"Content-Type", "application/json"}},
    //     cpr::Body{payload.dump()},
    //     cpr::Timeout{timeout}
    // );

    // if (response.status_code != OK) {
    //     spdlog::error("Update request failed: {} - {}", response.status_code, response.text);
    //     return false;
    // }
    //

    spdlog::info("API version updated successfully.");
    return true;
}

}  // namespace xvc