#include <fmt/core.h>
#include <spdlog/spdlog.h>
#include <xdaqvc/xvc.h>
#include <cpr/cpr.h>

#include <fstream>
#include <string>

std::string read_file_content(const std::string &filepath)
{
    std::ifstream file(filepath);
    if (!file.is_open()) {
        throw std::runtime_error("Could not open file: " + filepath);
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

int main()
{
    const std::string server_address = "192.168.177.100";
    const int server_port = 8000;
    
    try {
        // Perform handshake first
        auto handshake_result = xvc::perform_handshake(server_address, server_port);
        if (!handshake_result.success) {
            spdlog::error("Failed to establish connection with the device server: {}", 
                handshake_result.error_message);
            return 1;
        }
        
        spdlog::info("Successfully connected to device server");
        // Store the token for later use
        std::string session_token = handshake_result.token;

        // Original download code continues here...
        const std::string download_url = 
            "https://xvc001.sgp1.cdn.digitaloceanspaces.com/test_download.txt";
        const std::string checksum_url = "https://xvc001.sgp1.cdn.digitaloceanspaces.com/checksum.txt";
        const std::string output_path = "test_download.txt";

        // First download the checksum file
        auto checksum_result = xvc::download_and_verify(
            checksum_url,
            "",  // Empty hash since we don't verify the checksum file
            "checksum.txt"
        );

        if (!checksum_result.success) {
            spdlog::error("Failed to download checksum file: {}", checksum_result.error_message);
            return 1;
        }

        // Read the expected hash from the checksum file
        std::string expected_hash = read_file_content("checksum.txt");
        // Trim any whitespace or newlines
        expected_hash.erase(
            std::remove_if(expected_hash.begin(), expected_hash.end(), ::isspace),
            expected_hash.end()
        );

        spdlog::info("Expected hash: {}", expected_hash);

        // Prepare the file transfer before downloading
        if (!xvc::prepare_file_transfer(
            server_address,
            server_port,
            session_token,
            "test_download.txt",
            expected_hash,
            0  // We'll need to get the file size from the HEAD request
        )) {
            spdlog::error("Failed to prepare file transfer");
            return 1;
        }

        spdlog::info("File transfer preparation successful");

        // Now download and verify the actual file
        auto result = xvc::download_and_verify(download_url, expected_hash, output_path);

        if (!result.success) {
            spdlog::error("Download and verification failed: {}", result.error_message);
            return 1;
        }

        spdlog::info("Download and verification successful!");

        // Read and display the content of the downloaded file
        std::string content = read_file_content(output_path);
        spdlog::info("Downloaded file content: {}", content);

        return 0;
    } catch (const std::exception &e) {
        spdlog::error("Error: {}", e.what());
        return 1;
    }
}