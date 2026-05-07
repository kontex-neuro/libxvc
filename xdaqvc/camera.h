#pragma once

#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <vector>

class Camera
{
public:
    struct Cap {
        std::string media_type;
        std::optional<std::string> format = std::nullopt;
        int width;
        int height;
        int fps_n;
        int fps_d;

        constexpr std::string to_string() const noexcept
        {
            if (format.has_value() && !format.value().empty()) {
                return std::format(
                    "{},format={},width={},height={},framerate={}/{}",
                    media_type,
                    format.value(),
                    width,
                    height,
                    fps_n,
                    fps_d
                );
            } else {
                return std::format(
                    "{},width={},height={},framerate={}/{}", media_type, width, height, fps_n, fps_d
                );
            }
        }
    };

    explicit Camera(int id, std::string device_id, std::string name);
    ~Camera();

    // TODO: Camera::parse is used by xvc::ws_client function pointer and Camera::cameras
    //       but it should be made private.
    [[nodiscard]] static std::unique_ptr<Camera> parse(std::string_view camera_json);
    [[nodiscard]] static std::vector<std::unique_ptr<Camera>> cameras(
        std::chrono::milliseconds timeout = std::chrono::milliseconds(1000)
    );
    [[nodiscard]] int id() const noexcept { return _id; }
    [[nodiscard]] const std::string &device_id() const noexcept { return _device_id; }
    [[nodiscard]] const std::string &name() const noexcept { return _name; }
    [[nodiscard]] unsigned short port() const noexcept { return _port; }
    [[nodiscard]] const std::vector<Cap> &caps() const noexcept { return _caps; }

    void set_name(std::string_view name) { _name = name; }
    void add_cap(const Cap &cap) { _caps.emplace_back(cap); }

    bool start(
        const Cap &cap, std::chrono::milliseconds duration = std::chrono::milliseconds(1000)
    );
    bool stop(std::chrono::milliseconds duration = std::chrono::milliseconds(1000));

private:
    int _id;
    std::string _device_id;
    unsigned short _port;
    std::string _name;
    std::vector<Cap> _caps;
};