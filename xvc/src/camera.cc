#include "camera.h"

#include <cpr/api.h>

#include <nlohmann/json.hpp>

#include "port_pool.h"
#include "spdlog/spdlog.h"


using nlohmann::json;
using namespace std::chrono_literals;

namespace xvc
{
namespace
{
constexpr auto Cameras = "192.168.177.100:8000/cameras";
constexpr auto JPEG = "192.168.177.100:8000/jpeg";
constexpr auto Mock = "192.168.177.100:8000/mock";
constexpr auto H265 = "192.168.177.100:8000/h265";
constexpr auto Stop = "192.168.177.100:8000/stop";
constexpr auto OK = 200;
constexpr auto Timeout = 2s;

PortPool pool(9000, 9010);

}  // namespace

Camera::Camera(const int id, const std::string &name)
    : _id(id), _port(pool.allocate_port()), _name(name)
{
}

Camera::~Camera() { pool.release_port(_port); }

const std::string Camera::list_cameras()
{
    std::string cameras = "";
    auto response = cpr::Get(cpr::Url{Cameras}, cpr::Timeout{Timeout});
    if (response.status_code == OK) {
        cameras = json::parse(response.text).dump(2);
    }
    return cameras;
}

void Camera::start()
{
    json payload;
    payload["id"] = _id;
    payload["capability"] = _cap;
    payload["port"] = _port;
    cpr::Url url;
    if (_cap.find("image/jpeg") != std::string::npos) {
        url = cpr::Url{JPEG};
    } else if (_id == -1) {
        url = cpr::Url{Mock};
    } else {
        url = cpr::Url{H265};
    }
    auto response = cpr::Post(
        url,
        cpr::Header{{"Content-Type", "application/json"}},
        cpr::Body{payload.dump(2)},
        cpr::Timeout{Timeout}
    );
    if (response.status_code == OK) {
        spdlog::info("Successfully start camera");
    }
}

void Camera::stop()
{
    json payload;
    payload["id"] = _id;
    auto response = cpr::Post(
        cpr::Url{Stop},
        cpr::Header{{"Content-Type", "application/json"}},
        cpr::Body{payload.dump(2)},
        cpr::Timeout{Timeout}
    );
    if (response.status_code == OK) {
        spdlog::info("Successfully stop camera");
    }
}

}  // namespace xvc
