#include "camera.h"

#include <cpr/api.h>

#include <nlohmann/json.hpp>

#include "port_pool.h"
#include "spdlog/spdlog.h"


using nlohmann::json;
using namespace std::chrono_literals;

namespace
{
auto constexpr Cameras = "192.168.177.100:8000/cameras";
auto constexpr jpeg = "192.168.177.100:8000/jpeg";
auto constexpr test = "192.168.177.100:8000/test";
auto constexpr H265 = "192.168.177.100:8000/h265";
auto constexpr Stop = "192.168.177.100:8000/stop";
auto constexpr OK = 200;
auto constexpr Timeout = 2s;

auto constexpr VIDEO_MJPEG = "image/jpeg";
auto constexpr VIDEO_RAW = "video/x-raw";

PortPool pool(9000, 9010);

}  // namespace

Camera::Camera(const int id, const std::string &name)
    : _id(id), _port(pool.allocate_port()), _name(name)
{
}

Camera::~Camera() { pool.release_port(_port); }

const std::string Camera::cameras()
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
    payload["capability"] = _current_cap;
    payload["port"] = _port;
    cpr::Url url;
    if (_id <= -1 && _id >= -10) {
        url = cpr::Url{test};
    } else if (_current_cap.find(VIDEO_MJPEG) != std::string::npos ||
               _current_cap.find(VIDEO_RAW) != std::string::npos) {
        url = cpr::Url{jpeg};
    } else {
        // TODO: disable h265 for now
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
