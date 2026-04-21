#include "camera.h"

#include <cpr/api.h>
#include <spdlog/spdlog.h>

#include <string_view>

#include "port_pool.h"

namespace
{
constexpr auto Cameras = "http://192.168.177.100:8000/cameras";
constexpr auto MJPEG = "http://192.168.177.100:8000/jpeg";
constexpr auto Test = "http://192.168.177.100:8000/test";
constexpr auto H265 = "http://192.168.177.100:8000/h265";
constexpr auto H264 = "http://192.168.177.100:8000/h264";
constexpr auto Stop = "http://192.168.177.100:8000/stop";
constexpr auto OK = 200;

PortPool pool(9000, 9064);

std::optional<json> get_json(std::string_view url, std::chrono::milliseconds timeout)
{
    if (url.empty()) {
        spdlog::error("GET attempted with empty URL");
        return std::nullopt;
    }

    auto res = cpr::Get(cpr::Url{url}, cpr::Timeout{timeout});
    if (res.status_code != OK) {
        spdlog::warn(
            "GET {} failed (status={}, error='{}')", url, res.status_code, res.error.message
        );
        return std::nullopt;
    }

    try {
        return json::parse(res.text);
    } catch (const std::exception &e) {
        spdlog::error("JSON parse error from {}: {}", url, e.what());
        return std::nullopt;
    }
};

cpr::Response post_json(
    std::string_view url, const json &payload, const std::chrono::milliseconds timeout
)
{
    if (url.empty()) {
        spdlog::error("Attempted HTTP POST with empty URL");
        return {};
    }

    return cpr::Post(
        cpr::Url{url},
        cpr::Header{{"Content-Type", "application/json"}},
        cpr::Body{payload.dump(2)},
        cpr::Timeout{timeout}
    );
}

void log(const cpr::Response &r, std::string_view action)
{
    if (r.status_code == OK) {
        spdlog::info("{} succeeded", action);
    } else {
        spdlog::warn("{} failed. status={}, error='{}'", action, r.status_code, r.error.message);
    }
}

}  // namespace

Camera::Camera(const int id, std::string_view name) : _id(id), _name(name), _test(false)
{
    if (auto port = pool.allocate_port()) {
        _port = port.value();
    }
    spdlog::info("Creating camera id: {}, name: {}, port: {}", _id, _name, _port);
}

Camera::~Camera()
{
    pool.release_port(_port);
    spdlog::info("Deleting camera id: {}, name: {}, port: {}", _id, _name, _port);
}

std::vector<Camera *> Camera::cameras(const std::chrono::milliseconds duration)
{
    std::vector<Camera *> cameras;

    auto data = get_json(Cameras, duration);
    if (!data) {
        return cameras;
    }
    cameras.reserve(data->size());

    for (const auto &cam_json : *data) {
        if (auto cam = parse(cam_json)) {
            cameras.emplace_back(cam);
        }
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
    // auto camera = std::make_unique<Camera>(
    //     camera_json["id"].get<int>(), camera_json["name"].get<std::string>()
    // );

    for (const auto &cap_json : caps_json) {
        Camera::Cap cap{
            .media_type = cap_json.at("media_type").get<std::string>(),
            .format = cap_json.at("format").get<std::string>(),
            .width = cap_json.at("width").get<int>(),
            .height = cap_json.at("height").get<int>()
        };

        auto framerate_str = cap_json.at("framerate").get<std::string>();
        auto delimiter_pos = framerate_str.find('/');
        if (delimiter_pos != std::string::npos) {
            cap.fps_n = std::stoi(framerate_str.substr(0, delimiter_pos));
            cap.fps_d = std::stoi(framerate_str.substr(delimiter_pos + 1));
        }

        if (cap.media_type != "image/jpeg" && cap.media_type != "video/x-h265") {
            continue;
        }
        camera->add_cap(cap);
    }

    return camera;
}

void Camera::start(const Cap &cap, const std::chrono::milliseconds duration)
{
    const json payload{{"id", _id}, {"capability", cap.to_string()}, {"port", _port}};

    std::string_view url;

    if (_test) {
        url = Test;
    } else if (cap.media_type == "image/jpeg") {
        url = MJPEG;
    } else if (cap.media_type == "video/x-h265") {
        url = H265;
    } else if (cap.media_type == "video/x-h264") {
        url = H264;
    } else {
        spdlog::error("Unsupported codec for camera id: {}", _id);
        return;
    }

    auto res = post_json(url, payload, duration);

    log(res, fmt::format("Start camera id: {}", _id));
}

void Camera::stop(const std::chrono::milliseconds duration)
{
    const json payload{{"id", _id}};

    auto res = post_json(Stop, payload, duration);

    log(res, fmt::format("Stop camera id: {}", _id));
}