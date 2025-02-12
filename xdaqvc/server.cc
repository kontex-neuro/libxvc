#include "server.h"

#include <cpr/api.h>
#include <spdlog/spdlog.h>

#include <filesystem>


namespace fs = std::filesystem;


namespace
{
auto constexpr OpenAPI = "192.168.177.100:8000/openapi.json";
auto constexpr OK = 200;

auto constexpr Logs = "192.168.177.100:8000/logs";
}  // namespace


namespace xvc
{

Status server_status(const std::chrono::milliseconds duration)
{
    cpr::Url url(OpenAPI);
    cpr::Timeout timeout(duration);

    auto response = cpr::Get(url, timeout);

    // cpr::Session session;
    // session.SetUrl(url);
    // session.SetTimeout(_timeout);
    // auto response = session.Get();

    if (response.status_code == OK) {
        return Status::ON;
    }
    return Status::OFF;
}


std::string server_logs(const std::string &filename, const std::chrono::milliseconds duration)
{
    cpr::Url url;
    cpr::Timeout timeout(duration);

    if (!filename.empty()) {
        auto log = fs::path(Logs) / filename;
        spdlog::info("log = {}", log.generic_string());
        url = cpr::Url(log.generic_string());
    } else {
        url = cpr::Url(Logs);
    }

    auto response = cpr::Get(url, timeout);
    return response.text;
}

}  // namespace xvc