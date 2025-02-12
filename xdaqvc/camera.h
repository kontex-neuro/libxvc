#pragma once

#include <chrono>
#include <string>
#include <vector>


using namespace std::chrono_literals;


class Camera
{
public:
    struct Cap {
        std::string media_type;
        std::string format;
        int width;
        int height;
        int fps_n;
        int fps_d;
    };

    Camera(const int id, const std::string &name);
    ~Camera();

    [[nodiscard]] static std::string cameras(const std::chrono::milliseconds duration = 500ms);
    [[nodiscard]] std::string name() const { return _name; };
    [[nodiscard]] std::vector<Cap> caps() const { return _caps; };
    [[nodiscard]] unsigned short port() const { return _port; };
    [[nodiscard]] int id() const { return _id; }
    [[nodiscard]] std::string current_cap() const { return _current_cap; };

    void set_current_cap(const std::string &cap) { _current_cap = cap; }
    void add_cap(const Cap &cap) { _caps.emplace_back(cap); }

    void start(const std::chrono::milliseconds duration = 500ms);
    void stop(const std::chrono::milliseconds duration = 500ms);

private:
    int _id;
    unsigned short _port;
    std::string _name;
    std::vector<Cap> _caps;
    std::string _current_cap;
};