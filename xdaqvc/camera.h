#pragma once

#include <fmt/format.h>

#include <chrono>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using nlohmann::json;

class Camera
{
public:
    // video/x-raw,format=YUY2,width=640,height=480,framerate=30/1
    // image/jpeg,width=640,height=480,framerate=30/1
    struct Cap {
        std::string media_type;
        std::optional<std::string> format = std::nullopt;
        int width;
        int height;
        int fps_n;
        int fps_d;

        std::string to_string() const
        {
            if (format.has_value() && !format.value().empty()) {
                return fmt::format(
                    "{},format={},width={},height={},framerate={}/{}",
                    media_type,
                    format.value(),
                    width,
                    height,
                    fps_n,
                    fps_d
                );
            } else {
                return fmt::format(
                    "{},width={},height={},framerate={}/{}", media_type, width, height, fps_n, fps_d
                );
            }
        }
    };
    enum class Codec { MJPEG, H265, H264 };

    Camera(const int id = -1, const std::string &name = "");
    ~Camera();

    // [[nodiscard]] static std::unique_ptr<Camera> parse(const json &event);
    [[nodiscard]] static Camera *parse(const json &event);

    // [[nodiscard]] static std::vector<std::unique_ptr<Camera>> cameras(
    //     const std::chrono::milliseconds duration = 500ms
    // );
    [[nodiscard]] static std::vector<Camera *> cameras(
        const std::chrono::milliseconds duration = 500ms
    );
    [[nodiscard]] int id() const { return _id; }
    [[nodiscard]] unsigned short port() const { return _port; }

    [[nodiscard]] std::string name() const { return _name; }
    void set_name(const std::string &name) { _name = name; }

    [[nodiscard]] std::vector<Cap> caps() const { return _caps; }
    void add_cap(const Cap &cap) { _caps.emplace_back(cap); }

    [[nodiscard]] std::vector<Codec> codecs() const { return _codecs; }
    void add_codec(const Codec &codec)
    {
        if (std::find(_codecs.begin(), _codecs.end(), codec) == _codecs.end()) {
            _codecs.emplace_back(codec);
        }
    }

    [[nodiscard]] Codec stream_codec() const { return _stream_codec; }
    void set_stream_codec(const Codec &codec) { _stream_codec = codec; }

    void start(const Cap &cap, std::chrono::milliseconds duration = 500ms);
    void stop(const std::chrono::milliseconds duration = 500ms);

    [[nodiscard]] bool test_mode() const { return _test; }
    void set_test(const bool test) { _test = test; }

private:
    int _id;
    unsigned short _port;
    std::string _name;

    std::vector<Cap> _caps;
    std::vector<Codec> _codecs;
    Codec _stream_codec;

    bool _test;
};