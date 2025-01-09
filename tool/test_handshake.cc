#include <spdlog/spdlog.h>
#include <fmt/chrono.h>
#include <xdaqvc/xvc.h>

int main() {
    const std::string server_address = "192.168.177.100";
    const int server_port = 8001;
    
    try {
        auto handshake_result = xvc::perform_handshake(server_address, server_port);
        if (!handshake_result.success) {
            spdlog::error("Handshake failed: {}", handshake_result.error_message);
            return 1;
        }
        
        spdlog::info("Handshake successful!");
        spdlog::info("Session token: {}", handshake_result.token);
        spdlog::info("Token expires: {}", fmt::format("{:%Y-%m-%d %H:%M:%S}", handshake_result.expires));
        return 0;
    } catch (const std::exception& e) {
        spdlog::error("Error: {}", e.what());
        return 1;
    }
} 