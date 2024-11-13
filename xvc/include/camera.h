#pragma once

#include <string>
#include <vector>

namespace xvc
{

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

    [[nodiscard]] const std::string list_cameras();
    [[nodiscard]] const std::string &get_name() const { return _name; };
    [[nodiscard]] const std::vector<Cap> &get_caps() const { return caps; };
    [[nodiscard]] const unsigned short get_port() const { return _port; };
    [[nodiscard]] const int get_id() const { return _id; }
    [[nodiscard]] const std::string &get_current_cap() const { return _cap; };

    void set_current_cap(const std::string &cap) { _cap = cap; }
    void add_cap(const Cap &cap) { caps.emplace_back(cap); }

    void start();
    void stop();

private:
    int _id;
    unsigned short _port;
    std::string _name;
    std::vector<Cap> caps;
    std::string _cap;
};

}  // namespace xvc