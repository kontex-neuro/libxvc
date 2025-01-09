#include <spdlog/spdlog.h>
#include <fmt/chrono.h>
#include <xdaqvc/xvc.h>

int main() {
    const std::string server_address = "192.168.177.100";
    const int server_port = 8001;
    
    try {
        // First get a valid token
        auto handshake_result = xvc::perform_handshake(server_address, server_port);
        if (!handshake_result.success) {
            spdlog::error("Handshake failed: {}", handshake_result.error_message);
            return 1;
        }

        spdlog::info("Handshake successful!");
        spdlog::info("Session token: {}", handshake_result.token);

        // Test prepare_file_transfer with proper request format
        const std::string test_filename = "test.txt";
        const std::string test_hash = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
        const size_t test_size = 1024;
        std::string transfer_id;

        // Try prepare_file_transfer, if token expired, refresh and try again
        bool prepare_result = xvc::prepare_file_transfer(
            server_address,
            server_port,
            handshake_result.token,
            test_filename,
            test_hash,
            test_size,
            transfer_id
        );

        if (!prepare_result) {
            // Try to get a new token and retry
            spdlog::info("First attempt failed, refreshing token and retrying...");
            handshake_result = xvc::perform_handshake(server_address, server_port);
            if (!handshake_result.success) {
                spdlog::error("Token refresh failed: {}", handshake_result.error_message);
                return 1;
            }

            prepare_result = xvc::prepare_file_transfer(
                server_address,
                server_port,
                handshake_result.token,
                test_filename,
                test_hash,
                test_size,
                transfer_id
            );

            if (!prepare_result) {
                spdlog::error("Prepare file transfer failed after token refresh");
                return 1;
            }
        }

        spdlog::info("File transfer preparation successful!");
        spdlog::info("Transfer ID: {}", transfer_id);
        return 0;
    } catch (const std::exception& e) {
        spdlog::error("Error: {}", e.what());
        return 1;
    }
} 