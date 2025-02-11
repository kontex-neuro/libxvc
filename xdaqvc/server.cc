#include "server.h"

#include <cpr/api.h>
#include <spdlog/spdlog.h>


namespace
{
auto constexpr OpenAPI = "192.168.177.100:8000/openapi.json";
auto constexpr OK = 200;
}  // namespace

namespace xvc
{

Status server_status(const std::chrono::milliseconds timeout)
{
    cpr::Url url{OpenAPI};
    cpr::Timeout _timeout{timeout};

    auto response = cpr::Get(url, _timeout);

    // cpr::Session session;
    // session.SetUrl(url);
    // session.SetTimeout(_timeout);
    // auto response = session.Get();

    if (response.status_code == OK) {
        return Status::ON;
    }
    return Status::OFF;
}

}  // namespace xvc