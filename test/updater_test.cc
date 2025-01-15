#include "updater_test_base.h"

using namespace std::chrono_literals;

TEST_F(XVCUpdaterTest, CalculateSHA256)
{
    // Create a test file with known content
    std::string test_content = "Hello, World!";
    std::ofstream test_file("test.txt");
    test_file << test_content;
    test_file.close();

    auto hash = xvc::calculate_sha256("test.txt");
    ASSERT_TRUE(hash.has_value());
    EXPECT_FALSE(hash->empty());

    // Known SHA256 hash for "Hello, World!"
    std::string expected_hash = "dffd6021bb2bd5b0af676290809ec3a53191dd81c7f70a4b28688a362182986f";
    EXPECT_EQ(*hash, expected_hash);

    std::filesystem::remove("test.txt");
}

TEST_F(XVCUpdaterTest, DownloadAndVerify)
{
    // Get version table first to get valid hash
    auto version_table = xvc::get_version_table(version_table_url);
    ASSERT_TRUE(version_table.has_value());
    ASSERT_FALSE(version_table->versions.empty());

    const auto &test_version = version_table->versions.back();
    std::string download_url =
        fmt::format("https://xvc001.sgp1.cdn.digitaloceanspaces.com/{}", test_file);

    auto result = xvc::download_and_verify(download_url, test_version.hash, test_file);
    ASSERT_TRUE(result.success) << "Download failed: " << result.error_message;

    // Verify file exists and has content
    ASSERT_TRUE(std::filesystem::exists(test_file));
    ASSERT_GT(std::filesystem::file_size(test_file), 0);
}

TEST_F(XVCUpdaterTest, DownloadAndVerifyInvalidHash)
{
    auto result = xvc::download_and_verify(
        "https://xvc001.sgp1.cdn.digitaloceanspaces.com/xvc-server-0.0.1.tar.xz",
        "invalid_hash",
        test_file
    );
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error_message.empty());
    EXPECT_FALSE(std::filesystem::exists(test_file));
}

TEST_F(XVCUpdaterTest, HandshakeTest)
{
    auto response = xvc::perform_handshake(server_address, update_server_port);
    ASSERT_TRUE(response.success) << "Handshake failed: " << response.error_message;

    EXPECT_FALSE(response.token.empty());
    EXPECT_GT(response.expires, std::chrono::system_clock::now());
}

TEST_F(XVCUpdaterTest, FileTransferWorkflow)
{
    // First download a test file
    auto version_table = xvc::get_version_table(version_table_url);
    ASSERT_TRUE(version_table.has_value());
    ASSERT_FALSE(version_table->versions.empty());

    const auto &test_version = version_table->versions.back();
    std::string download_url =
        fmt::format("https://xvc001.sgp1.cdn.digitaloceanspaces.com/{}", test_file);

    auto download_result = xvc::download_and_verify(download_url, test_version.hash, test_file);
    ASSERT_TRUE(download_result.success);

    // Perform handshake
    auto handshake = xvc::perform_handshake(server_address, update_server_port);
    ASSERT_TRUE(handshake.success);

    // Prepare transfer
    std::string transfer_id;
    auto file_size = std::filesystem::file_size(test_file);
    bool prepared = xvc::prepare_file_transfer(
        server_address,
        update_server_port,
        handshake.token,
        test_file,
        test_version.hash,
        file_size,
        transfer_id
    );
    ASSERT_TRUE(prepared);
    EXPECT_FALSE(transfer_id.empty());

    // Perform transfer
    bool transfer_success = xvc::transfer_file(
        server_address,
        update_server_port,
        handshake.token,
        test_file,
        transfer_id,
        [](const xvc::FileTransferProgress &progress) {
            EXPECT_GE(progress.progress_percentage, 0.0f);
            EXPECT_LE(progress.progress_percentage, 100.0f);
            EXPECT_GT(progress.total_bytes, 0ULL);
        }
    );
    ASSERT_TRUE(transfer_success);
}

TEST_F(XVCUpdaterTest, GetServerVersion)
{
    auto version = xvc::get_server_version(server_address, server_port);
    ASSERT_TRUE(version.has_value());
    EXPECT_GT(*version, xvc::Version({0, 0, 0}));
}

TEST_F(XVCUpdaterTest, GetVersionTable)
{
    auto table = xvc::get_version_table(version_table_url);
    ASSERT_TRUE(table.has_value());
    EXPECT_FALSE(table->versions.empty());
    EXPECT_EQ(table->latest_version, table->versions.front().version);
}

TEST_F(XVCUpdaterTest, CompleteUpdateWorkflow)
{
    auto result = xvc::update_server(
        server_address,
        server_port,
        update_server_port,
        version_table_url,
        update_dir,
        client_version
    );

    ASSERT_TRUE(result.success) << "Update failed: " << result.error_message;

    if (result.update_needed) {
        EXPECT_GT(result.available_version, result.current_version);

        // Verify update file was downloaded
        std::string expected_filename =
            fmt::format("xvc-server-{}.tar.xz", result.available_version.to_string());
        auto update_path = std::filesystem::path(update_dir) / expected_filename;

        EXPECT_TRUE(std::filesystem::exists(update_path));
        EXPECT_GT(std::filesystem::file_size(update_path), 0);
    }
}