#include <xdaqvc/xvc.h>
#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <iostream>
#include <filesystem>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <server_address> <file_path>\n";
        std::cerr << "Example: " << argv[0] << " localhost:8000 /path/to/file.mkv\n";
        return 1;
    }

    try {
        // Parse server address and port
        std::string server_address = argv[1];
        auto colon_pos = server_address.find(':');
        if (colon_pos == std::string::npos) {
            std::cerr << "Invalid server address format. Use host:port\n";
            return 1;
        }
        
        std::string host = server_address.substr(0, colon_pos);
        int port = std::stoi(server_address.substr(colon_pos + 1));
        std::filesystem::path file_path = argv[2];

        // Verify file exists
        if (!std::filesystem::exists(file_path)) {
            std::cerr << "File does not exist: " << file_path << "\n";
            return 1;
        }

        // 1. Perform handshake
        spdlog::info("Performing handshake with server...");
        auto handshake_response = xvc::perform_handshake(host, port);
        if (!handshake_response.success) {
            spdlog::error("Handshake failed: {}", handshake_response.error_message);
            return 1;
        }
        spdlog::info("Handshake successful!");

        // 2. Calculate file hash
        spdlog::info("Calculating file hash...");
        auto file_hash = xvc::calculate_sha256(file_path);
        if (!file_hash) {
            spdlog::error("Failed to calculate file hash");
            return 1;
        }

        // 3. Prepare transfer and get transfer ID
        spdlog::info("Preparing file transfer...");
        auto file_size = std::filesystem::file_size(file_path);
        std::string transfer_id;
        bool prepared = xvc::prepare_file_transfer(
            host,
            port,
            handshake_response.token,
            file_path.filename().string(),
            *file_hash,
            file_size,
            transfer_id
        );

        if (!prepared) {
            spdlog::error("Failed to prepare file transfer");
            return 1;
        }
        spdlog::info("Transfer prepared. ID: {}", transfer_id);

        // 4. Start the actual file transfer
        spdlog::info("Starting file transfer...");
        auto last_update = std::chrono::steady_clock::now();
        bool transfer_result = xvc::transfer_file(
            host,
            port,
            handshake_response.token,
            file_path,
            transfer_id,
            [&last_update](const xvc::FileTransferProgress& progress) {
                auto now = std::chrono::steady_clock::now();
                // Update progress at most once per second
                if (now - last_update >= 1s) {
                    spdlog::info(
                        "Transfer progress: {:.1f}% ({}/{} bytes)",
                        progress.progress_percentage,
                        progress.bytes_transferred,
                        progress.total_bytes
                    );
                    last_update = now;
                }
            }
        );

        if (transfer_result) {
            spdlog::info("File transfer completed successfully!");
            return 0;
        } else {
            spdlog::error("File transfer failed");
            return 1;
        }

    } catch (const std::exception& e) {
        spdlog::error("Error: {}", e.what());
        return 1;
    }
} 