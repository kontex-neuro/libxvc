#pragma once
#include "types.h"
#include "device.h"
#include "plan.h"

namespace xvc::update {

enum class LegacyDeviceProduct { Unspecified, Aio, Nvr };

struct DeviceUpdateIdentity {
    DeviceDescriptor device;
    std::string updater_version;
};

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
// A factory updater returns 404. A modern updater can identify the hardware
// even if migration was interrupted before the new videoserver was installed.
Result<DeviceUpdateIdentity> discover_update_identity(const DeviceEndpoint &endpoint, DeviceDescriptor device);
Result<DeviceUpdateIdentity> parse_update_identity(DeviceDescriptor device, std::string_view capabilities);
Result<std::string> discover_updater_version(const DeviceEndpoint &device);
Result<OperationReceipt> parse_operation_receipt(std::string_view json);
bool receipt_matches(const OperationReceipt &receipt, const VersionEntry &entry, const Artifact &artifact);
bool updater_ready(std::string_view capabilities);
bool application_requires_framelab(const DeviceDescriptor &device, const VersionEntry &target);
bool application_health_ready(const DeviceDescriptor &device, const VersionEntry &target,
                              std::string_view framelab_capabilities = {});
std::optional<int> legacy_setup_return_code(std::string_view message);
bool migration_upload_pending(const Error &error);
// Success here only permits durable-receipt polling; it never proves migration success.
Result<void> migration_upload_result(const Result<void> &upload, std::optional<int> setup_return_code = std::nullopt);
// Upload + drain + classify only. A successful return still requires a durable
// matching migration receipt; no application package is transferred here.
Result<void> transfer_migration(const DeviceEndpoint &endpoint, const Handshake &handshake,
                               const PreparedTransfer &prepared, const std::filesystem::path &file,
                               ProgressCallback progress = nullptr, LogCallback logs = nullptr,
                               std::chrono::milliseconds upload_timeout = std::chrono::minutes{10},
                               std::chrono::milliseconds drain_timeout = std::chrono::seconds{5});
std::optional<std::string> migration_failure_log(std::string_view log, std::string_view archive_sha256);
bool secure_cloud_url(std::string_view url);
Result<DeviceUpdatePlan> plan_device_update(const VersionMatrix &matrix, const DeviceDescriptor &device,
                                            std::string_view updater_version, std::string_view environment,
                                            LegacyDeviceProduct confirmed_product);
Result<DeviceUpdatePlan> plan_device_update(const VersionMatrix &matrix, const DeviceDescriptor &device,
                                          std::string_view updater_version, std::string_view environment = "release",
                                          bool allow_legacy_aio_confirmation = false);
using UpdatePhaseCallback = std::function<void(std::string_view)>;
Result<void> install_device_release(const std::string &cloud, const DeviceEndpoint &endpoint,
                                   const DeviceUpdatePlan &plan, ProgressCallback progress = nullptr,
                                   LogCallback logs = nullptr, UpdatePhaseCallback phase = nullptr);
}
