#pragma once
#include "types.h"
#include "plan.h"

namespace xvc::update {

struct DeviceUpdatePlan {
    DeviceDescriptor device;
    VersionEntry target;
    bool update_available = false;
    bool migration_required = false;
    bool legacy_aio_confirmation = false;
    std::string environment = "release";
};

struct OperationReceipt {
    std::string status;
    std::string archive_sha256;
    std::string version;
    std::string source_commit;
    std::string product_id;
    std::string profile;
    std::string error;
    std::string role;
    std::string kind;
    std::string outcome;
};

Result<DeviceDescriptor> parse_device_descriptor(std::string_view descriptor, std::string_view build);
Result<DeviceDescriptor> discover_device(const DeviceEndpoint &device);
Result<std::string> discover_updater_version(const DeviceEndpoint &device);
Result<OperationReceipt> parse_operation_receipt(std::string_view json);
bool receipt_matches(const OperationReceipt &receipt, const VersionEntry &entry, const Artifact &artifact);
bool updater_ready(std::string_view capabilities);
bool migration_upload_pending(const Error &error);
std::optional<std::string> migration_failure_log(std::string_view log, std::string_view archive_sha256);
bool secure_cloud_url(std::string_view url);
Result<DeviceUpdatePlan> plan_device_update(const VersionMatrix &matrix, const DeviceDescriptor &device,
                                          std::string_view updater_version, std::string_view environment = "release",
                                          bool allow_legacy_aio_confirmation = false);
using UpdatePhaseCallback = std::function<void(std::string_view)>;
Result<void> install_device_release(const std::string &cloud, const DeviceEndpoint &endpoint,
                                   const DeviceUpdatePlan &plan, ProgressCallback progress = nullptr,
                                   LogCallback logs = nullptr, UpdatePhaseCallback phase = nullptr);
}
