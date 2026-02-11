#include "camera.h"

#include <cpr/api.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>
#include <string_view>

#include "port_pool.h"
#include "validator.h"

using nlohmann::json;

namespace
{
constexpr std::string_view URL = "http://192.168.177.100:8000";
constexpr std::string_view CAMERAS = "/cameras";
constexpr std::string_view MJPEG = "/jpeg";
constexpr std::string_view H265 = "/h265";
constexpr std::string_view H264 = "/h264";
constexpr std::string_view STOP = "/stop";
constexpr auto OK = 200;

PortPool pool(9000, 9064);

std::optional<nlohmann::json> get_json(std::string_view url, std::chrono::milliseconds timeout)
{
    if (url.empty()) {
        spdlog::error("GET attempted with empty URL");
        return std::nullopt;
    }

    auto res = cpr::Get(cpr::Url{url}, cpr::Timeout{timeout});
    if (res.status_code != OK) {
        spdlog::warn(
            "Failed to GET {} (status={}, error='{}')", url, res.status_code, res.error.message
        );
        return std::nullopt;
    }

    try {
        return nlohmann::json::parse(res.text);
    } catch (const std::exception &e) {
        spdlog::error("JSON parse error from {}: {}", url, e.what());
        return std::nullopt;
    }
};

cpr::Response post_json(
    std::string_view url, const nlohmann::json &payload, std::chrono::milliseconds timeout
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

constexpr std::string url(std::string_view endpoint) { return std::format("{}{}", URL, endpoint); }

}  // namespace

Camera::Camera(int id, std::string device_id, std::string name)
    : _id(id), _device_id(std::move(device_id)), _name(std::move(name))
{
    auto port = pool.allocate();
    if (!port) {
        spdlog::error("Failed to allocate port for camera id: {}", _id);
        throw std::runtime_error("Failed to allocate port for camera");
    }

    _port = port.value();
    spdlog::debug(
        "Creating camera id: {}, device_id: {}, name: {}, port: {}", _id, _device_id, _name, _port
    );
}

Camera::~Camera()
{
    if (pool.release(_port)) {
        spdlog::debug(
            "Deleting camera id: {}, device_id: {}, name: {}, port: {}",
            _id,
            _device_id,
            _name,
            _port
        );
    }
}

std::unique_ptr<Camera> Camera::parse(std::string_view camera_json)
{
    try {
        auto json = json::parse(camera_json);
        if (json.empty() || !json.is_object()) {
            spdlog::error("Invalid camera JSON format");
            return nullptr;
        }
        if (auto validated = validate_camera(json); !validated) {
            spdlog::error("Camera JSON validation failed: {}", validated.error());
            return nullptr;
        }

        auto camera = std::make_unique<Camera>(
            json.at("id").get<int>(),
            json.at("device_id").get<std::string>(),
            json.at("name").get<std::string>()
        );

        for (const auto &cap_json : json.at("caps")) {
            Camera::Cap cap{
                .media_type = cap_json.at("media_type").get<std::string>(),
                .format = cap_json.value("format", ""),
                .width = cap_json.at("width").get<int>(),
                .height = cap_json.at("height").get<int>()
            };

            const auto &framerate = cap_json.at("framerate").get<std::string>();
            auto slash = framerate.find('/');
            if (slash != std::string::npos) {
                cap.fps_n = std::stoi(framerate.substr(0, slash));
                cap.fps_d = std::stoi(framerate.substr(slash + 1));
            }

            if (cap.media_type == "image/jpeg" || cap.media_type == "video/x-h265") {
                camera->add_cap(cap);
            }
        }
        return camera;
    } catch (const std::exception &e) {
        spdlog::error("Exception parsing camera JSON: {}", e.what());
        return nullptr;
    }
}

std::vector<std::unique_ptr<Camera>> Camera::cameras(std::chrono::milliseconds duration)
{
    std::vector<std::unique_ptr<Camera>> cameras;

    auto camera_json = get_json(url(CAMERAS), duration);
    if (!camera_json || !camera_json->is_array()) {
        spdlog::error("Invalid cameras format");
        return cameras;
    }
    cameras.reserve(camera_json->size());

    for (const auto &json : *camera_json) {
        if (auto validated = validate_camera(json); !validated) {
            spdlog::error("Camera JSON validation failed: {}", validated.error());
            continue;
        }
        if (auto cam = parse(json.dump())) {
            cameras.emplace_back(std::move(cam));
        }
    }
    return cameras;
}

bool Camera::start(const Cap &cap, std::chrono::milliseconds duration)
{
    const nlohmann::json payload{{"id", _id}, {"capability", cap.to_string()}, {"port", _port}};

    std::string _url;
    if (cap.media_type == "image/jpeg") {
        _url = url(MJPEG);
    } else if (cap.media_type == "video/x-h265") {
        _url = url(H265);
    } else {
        spdlog::error("Unsupported codec for camera id: {}", _id);
        return false;
    }

    auto res = post_json(_url, payload, duration);
    if (res.status_code != OK) {
        log(res, std::format("Start camera id: {}", _id));
        return false;
    }

    log(res, std::format("Start camera id: {}", _id));
    return true;
}

bool Camera::stop(std::chrono::milliseconds duration)
{
    const nlohmann::json payload{{"id", _id}};

    auto res = post_json(url(STOP), payload, duration);
    if (res.status_code != OK) {
        log(res, std::format("Stop camera id: {}", _id));
        return false;
    }

    log(res, std::format("Stop camera id: {}", _id));
    return true;
}