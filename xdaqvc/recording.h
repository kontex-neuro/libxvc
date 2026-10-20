#pragma once
#include <chrono>
#include <expected>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace xvc {
// Shared endpoint configuration, including development/testing on router LANs.
std::string device_host();
int device_port();
std::string device_url();

struct RecordingProfile {
    std::string codec;
    int width=0, height=0, fps_n=0, fps_d=1;
    nlohmann::json json() const;
    static RecordingProfile parse(const nlohmann::json&);
};
struct RecordingReply {
    nlohmann::json state;
    std::string error;
    int status=0;
    explicit operator bool() const { return status==200 && error.empty(); }
};
class RecordingClient {
public:
    explicit RecordingClient(std::string base=device_url()):base_(std::move(base)) {}
    RecordingReply state() const;
    RecordingReply apply(const nlohmann::json& configuration) const;
    RecordingReply storage(std::string_view operation, const nlohmann::json& token) const;
private:
    std::string base_;
    RecordingReply request(std::string_view method,std::string_view path,
                           const nlohmann::json& body={},int timeout_ms=1500) const;
};
}
