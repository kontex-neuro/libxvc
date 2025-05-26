#pragma once

#include <chrono>
#include <optional>
#include <string>

using namespace std::chrono_literals;

namespace xvc
{

enum class Status { OFF, ON };

class Version
{
public:
    Version(const std::string &version_str);

    bool operator==(const Version &other) const;
    bool operator<(const Version &other) const;
    bool operator>(const Version &other) const;

    std::string to_string() const;

private:
    int major;
    int minor;
    int patch;
};

class Server
{
public:
    Server(const std::string &host = "192.168.177.100", int port = 8000);

    Status status(const std::chrono::milliseconds timeout = 500ms) const;
    std::string logs(
        const std::string &filename = "", const std::chrono::milliseconds timeout = 500ms
    ) const;

    std::optional<std::string> get_api_version(const std::chrono::milliseconds timeout = 500ms)
        const;
    bool update(const Version &version, const std::chrono::milliseconds timeout = 500ms) const;

private:
    std::string _base_url;
};

}  // namespace xvc