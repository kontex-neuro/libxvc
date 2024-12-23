#pragma once

#include <string>
#include <vector>


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

    [[nodiscard]] static const std::string cameras();
    [[nodiscard]] const std::string name() const { return _name; };
    [[nodiscard]] const std::vector<Cap> caps() const { return _caps; };
    [[nodiscard]] unsigned short port() const { return _port; };
    [[nodiscard]] int id() const { return _id; }
    [[nodiscard]] const std::string current_cap() const { return _current_cap; };

    void set_current_cap(const std::string &cap) { _current_cap = cap; }
    void add_cap(const Cap &cap) { _caps.emplace_back(cap); }

    void start();
    void stop();

private:
    int _id;
    unsigned short _port;
    std::string _name;
    std::vector<Cap> _caps;
    std::string _current_cap;
};