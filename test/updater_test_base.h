#pragma once

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "updater.h"


namespace fs = std::filesystem;


class XVCUpdaterTest : public testing::Test
{
protected:
    void SetUp() override
    {
        server_address = "192.168.177.100";
        server_port = 8000;
        update_server_port = 8001;
        version_table_url = "https://xvc001.sgp1.digitaloceanspaces.com/versions.json";
        update_dir = "test_updates";
        test_file = "xvc-server-0.0.1.tar.xz";
        client_version = xvc::Version{0, 0, 1};
    }

    void TearDown() override
    {
        fs::remove_all(update_dir);
        if (fs::exists(test_file)) {
            fs::remove(test_file);
        }
    }

    std::string read_file_content(const std::string &filepath)
    {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            ADD_FAILURE() << "Could not open file: " << filepath;
            return std::string("");
        }
        return std::string(
            (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()
        );
    }

public:
    std::string server_address;
    int server_port;
    int update_server_port;
    std::string version_table_url;
    std::string update_dir;
    std::string test_file;
    xvc::Version client_version;
};