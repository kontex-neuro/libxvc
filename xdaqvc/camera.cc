#include "camera.h"

#include <cpr/api.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "port_pool.h"

using nlohmann::json;

namespace
{
auto constexpr Cameras = "192.168.177.100:8000/cameras";
auto constexpr jpeg = "192.168.177.100:8000/jpeg";
auto constexpr test = "192.168.177.100:8000/test";
[[maybe_unused]] auto constexpr loopback = "127.0.0.1:8000/test";
auto constexpr H265 = "192.168.177.100:8000/h265";
auto constexpr Stop = "192.168.177.100:8000/stop";
auto constexpr OK = 200;

auto constexpr VIDEO_MJPEG = "image/jpeg";
auto constexpr VIDEO_RAW = "video/x-raw";

PortPool pool(9000, 9064);

}  // namespace

Camera::Camera(const int id, const std::string &name)
    : _id(id), _name(name), _current_cap(""), _test(false)
{
    auto port = pool.allocate_port();
    if (port) {
        _port = port.value();
    }
    spdlog::info("Creating camera id: {}, name: {}, port: {}", id, name, _port);
}

Camera::~Camera()
{
    pool.release_port(_port);
    spdlog::info("Deleting camera id: {}, name: {}, port: {}", _id, _name, _port);
}

std::string Camera::cameras(const std::chrono::milliseconds duration)
{
    auto cameras = std::string("");

    auto response = cpr::Get(cpr::Url(Cameras), cpr::Timeout(duration));
    if (response.status_code == OK) {
        cameras = json::parse(response.text).dump(2);
    }
    return cameras;
}

void Camera::start(const std::chrono::milliseconds duration)
{
    json payload;
    payload["id"] = _id;
    payload["capability"] = _current_cap;
    payload["port"] = _port;
    cpr::Url url;

    if (_test) {
        url = cpr::Url(test);
    } else if (_current_cap.find(VIDEO_MJPEG) != std::string::npos ||
               _current_cap.find(VIDEO_RAW) != std::string::npos) {
        url = cpr::Url(jpeg);
    } else {
        // TODO: disable h265 for now
        url = cpr::Url(H265);
    }

    auto response = cpr::Post(
        url,
        cpr::Header{{"Content-Type", "application/json"}},
        cpr::Body(payload.dump(2)),
        cpr::Timeout(duration)
    );
    if (response.status_code == OK) {
        spdlog::info(
            "Successfully send http request to start camera: {} with id: {}, port: {}, cap: {}",
            jpeg,
            _id,
            _port,
            _current_cap
        );
    } else {
        spdlog::info("Failed to start camera");
    }
}

void Camera::stop(const std::chrono::milliseconds duration)
{
    json payload;
    payload["id"] = _id;

    auto response = cpr::Post(
        cpr::Url(Stop),
        cpr::Header{{"Content-Type", "application/json"}},
        cpr::Body(payload.dump(2)),
        cpr::Timeout(duration)
    );
    if (response.status_code == OK) {
        spdlog::info("Successfully send http request to stop camera: {} with id: {}", Stop, _id);
    } else {
        spdlog::info("Failed to stop camera");
    }
}
