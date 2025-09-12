#include "camera.h"

#include <cpr/api.h>
#include <spdlog/spdlog.h>

// #include <nlohmann/json.hpp>

#include "port_pool.h"

// using nlohmann::json;

namespace
{
auto constexpr Cameras = "192.168.177.100:8000/cameras";
auto constexpr jpeg = "192.168.177.100:8000/jpeg";
auto constexpr test = "192.168.177.100:8000/test";
[[maybe_unused]] auto constexpr loopback = "127.0.0.1:8000/test";
auto constexpr H265 = "192.168.177.100:8000/h265";
auto constexpr Stop = "192.168.177.100:8000/stop";
auto constexpr OK = 200;

PortPool pool(9000, 9064);

}  // namespace

Camera::Camera(const int id, const std::string &name) : _id(id), _name(name), _test(false)
{
    auto port = pool.allocate_port();
    if (port) {
        _port = port.value();
    }
    spdlog::info("Creating camera id: {}, name: {}, port: {}", _id, _name, _port);
}

Camera::~Camera()
{
    pool.release_port(_port);
    spdlog::info("Deleting camera id: {}, name: {}, port: {}", _id, _name, _port);
}

// std::vector<std::unique_ptr<Camera>> Camera::cameras(const std::chrono::milliseconds duration)
std::vector<Camera *> Camera::cameras(const std::chrono::milliseconds duration)
{
    // std::vector<std::unique_ptr<Camera>> cameras;
    std::vector<Camera *> cameras;

    auto response = cpr::Get(cpr::Url(Cameras), cpr::Timeout(duration));
    if (response.status_code != OK) {
        spdlog::warn("Failed to fetch cameras.");
        return cameras;
    }

    try {
        auto cameras_json = json::parse(response.text);
        for (const auto &cam : cameras_json) {
            cameras.emplace_back(Camera::parse(cam));
        }
    } catch (const std::exception &e) {
        spdlog::error("JSON parse error: {}", e.what());
    }

    return cameras;
}

// std::unique_ptr<Camera> Camera::parse(const json &camera_json)
Camera *Camera::parse(const json &camera_json)
{
    auto const id = camera_json["id"].get<int>();
    auto const name = camera_json["name"].get<std::string>();
    auto const caps_json = camera_json["caps"];

    auto camera = new Camera(id, name);
    // auto camera = std::make_unique<Camera>(id, name);

    for (const auto &cap_json : caps_json) {
        Camera::Cap cap;
        cap.media_type = cap_json.at("media_type").get<std::string>();
        cap.format = cap_json.at("format").get<std::string>();
        cap.width = cap_json.at("width").get<int>();
        cap.height = cap_json.at("height").get<int>();

        auto framerate_str = cap_json.at("framerate").get<std::string>();
        auto delimiter_pos = framerate_str.find('/');
        if (delimiter_pos != std::string::npos) {
            cap.fps_n = std::stoi(framerate_str.substr(0, delimiter_pos));
            cap.fps_d = std::stoi(framerate_str.substr(delimiter_pos + 1));
        }
        camera->add_cap(cap);

        // TODO: hack
        if (cap.media_type == "image/jpeg") {
            camera->add_codec(Codec::MJPEG);
        } else if (cap.media_type == "video/x-raw") {
            camera->add_codec(Codec::MJPEG);
            camera->add_codec(Codec::H265);
        }
    }

    return camera;
}

void Camera::start(const Cap &cap, const std::chrono::milliseconds duration)
{
    json payload;
    payload["id"] = _id;
    payload["capability"] = cap.to_string();
    payload["port"] = _port;

    cpr::Url url;
    if (_test) {
        url = cpr::Url(test);
    } else if (_stream_codec == Codec::MJPEG) {
        url = cpr::Url(jpeg);
    } else if (_stream_codec == Codec::H265) {
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
            "Successfully send HTTP request to start camera, id: {}, port: {}, cap: {}",
            _id,
            _port,
            cap.to_string()
        );
    } else {
        spdlog::warn("Failed to start camera, status code: {}", response.status_code);
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
        spdlog::info("Successfully send HTTP request to stop camera, id: {}", _id);
    } else {
        spdlog::warn("Failed to stop camera, status code: {}", response.status_code);
    }
}