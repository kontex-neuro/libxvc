#include <spdlog/spdlog.h>
#include <xdaqvc/xvc.h>
#include <fstream>

std::string read_file_content(const std::string &filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        throw std::runtime_error("Could not open file: " + filepath);
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

int main() {
    try {
        const std::string download_url = 
            "https://xvc001.sgp1.cdn.digitaloceanspaces.com/test_download.txt";
        const std::string checksum_url = 
            "https://xvc001.sgp1.cdn.digitaloceanspaces.com/checksum.txt";
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

        // Read and verify the actual file
        std::string expected_hash = read_file_content("checksum.txt");
        expected_hash.erase(
            std::remove_if(expected_hash.begin(), expected_hash.end(), ::isspace),
            expected_hash.end()
        );

        auto result = xvc::download_and_verify(download_url, expected_hash, output_path);
        if (!result.success) {
            spdlog::error("Download and verification failed: {}", result.error_message);
            return 1;
        }

        spdlog::info("Download and verification successful!");
        std::string content = read_file_content(output_path);
        spdlog::info("Downloaded file content: {}", content);
        return 0;
    } catch (const std::exception& e) {
        spdlog::error("Error: {}", e.what());
        return 1;
    }
} 