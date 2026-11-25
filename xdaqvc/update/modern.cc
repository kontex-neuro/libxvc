#include "modern.h"
#include "artifact.h"
#include "device.h"
#include <algorithm>
#include <charconv>
#include <cpr/cpr.h>
#include <format>
#include <nlohmann/json.hpp>
#include <regex>
#include <sstream>
#include <thread>

namespace xvc::update {
namespace {
using Json = nlohmann::json;
std::string text(const Json &node, std::string_view name) {
    auto it=node.find(name); return it!=node.end() && it->is_string() ? it->get<std::string>() : "";
}
bool flag(const Json &node,std::string_view name) {auto it=node.find(name);return it!=node.end() && it->is_boolean() && it->get<bool>();}
int number(const Json &node,std::string_view name) {auto it=node.find(name);return it!=node.end() && it->is_number_integer()?it->get<int>():0;}
Error malformed(std::string message) {return {ErrorCode::MalformedResponse,std::move(message)};}
bool compatible_api(const DeviceDescriptor &device) {
    return device.api_version && device.api_version->major()==0 && device.api_version->minor()==1;
}
std::string base(const DeviceEndpoint &device,int port) {return std::format("http://{}:{}",device.host,port);}
Result<Json> get(const std::string &url,const std::string &token={}) {
    cpr::Header headers;
    if(!token.empty()) headers.emplace("Authorization","Bearer "+token);
    auto response=cpr::Get(cpr::Url{url},headers,cpr::Timeout{std::chrono::seconds{2}},cpr::Redirect{false});
    if(response.error.code!=cpr::ErrorCode::OK) return std::unexpected(Error{ErrorCode::NetworkUnreachable,"Device connection unavailable"});
    if(response.status_code!=200) return std::unexpected(Error{ErrorCode::HttpError,"Device endpoint unavailable",static_cast<int>(response.status_code)});
    auto value=Json::parse(response.text,nullptr,false);
    if(!value.is_object()) return std::unexpected(malformed("Device response is not an object"));
    return value;
}
const Artifact *role(const VersionEntry &entry,std::string_view wanted) {
    auto it=std::find_if(entry.files.begin(),entry.files.end(),[&](const auto &file){return file.role==wanted;});
    return it==entry.files.end()?nullptr:&*it;
}
bool framelab_ready(const Json &value) {
    if(!value.contains("protocol") || !value["protocol"].is_number_integer() || value["protocol"]!=1
       || !value.contains("sandbox") || !value["sandbox"].is_object()) return false;
    const auto &sandbox=value["sandbox"];
    return flag(sandbox,"bwrap") && flag(sandbox,"systemd_scope") && text(sandbox,"memory_enforcement")=="cgroup"
           && sandbox.contains("problems") && sandbox["problems"].is_array() && sandbox["problems"].empty();
}
Result<void> wait_operation(const DeviceEndpoint &endpoint,const VersionEntry &entry,const Artifact &artifact,
                            bool migration,LogCallback logs) {
    auto deadline=std::chrono::steady_clock::now()+std::chrono::minutes{10};
    auto next_diagnostic=std::chrono::steady_clock::now();
    std::string last_error="Waiting for a durable updater operation receipt";
    while(std::chrono::steady_clock::now()<deadline) {
        auto handshake=perform_handshake(endpoint,std::chrono::seconds{2});
        if(handshake) {
            auto receipt=get(base(endpoint,endpoint.update_port)+"/operations/"+artifact.sha256,handshake->token);
            if(receipt) {
                auto parsed=parse_operation_receipt(receipt->dump());
                if(!parsed) return std::unexpected(parsed.error());
                if(!receipt_matches(*parsed,entry,artifact)) return std::unexpected(malformed("Updater receipt identifies another package/device"));
                if(parsed->status=="failed" || parsed->status=="interrupted")
                    return std::unexpected(Error{ErrorCode::DeviceUpdateFailed,parsed->error.empty()?"Updater reported a failed or interrupted operation":parsed->error});
                if(parsed->status=="succeeded") {
                    last_error="Receipt succeeded, but updater recovery or readiness is not complete";
                    auto caps=get(base(endpoint,endpoint.update_port)+"/capabilities");
                    if(caps && !text(*caps,"recovery_error").empty())
                        return std::unexpected(Error{ErrorCode::DeviceUpdateFailed,"Updater recovery failed: "+text(*caps,"recovery_error")});
                    if(caps && updater_ready(caps->dump())) {
                        if(migration) return {};
                        auto identity=discover_device(endpoint);
                        if(identity) {
                            std::string runtime;
                            if(application_requires_framelab(*identity,entry)) {
                                auto framelab=get(base(endpoint,endpoint.server_port)+"/framelab/v1/capabilities");
                                if(framelab) runtime=framelab->dump();
                            }
                            if(application_health_ready(*identity,entry,runtime)) return {};
                        }
                        last_error="Receipt succeeded, but expected application identity or required capability health is not ready";
                    }
                }
            }
        }
        if(migration && std::chrono::steady_clock::now()>=next_diagnostic) {
            auto response=cpr::Get(cpr::Url{base(endpoint,endpoint.server_port)+"/logs/error.log.updater-migration-"+artifact.sha256},
                                   cpr::Timeout{std::chrono::seconds{2}},cpr::Redirect{false});
            if(response.status_code==200) {
                auto failure=migration_failure_log(response.text,artifact.sha256);
                if(failure) return std::unexpected(Error{ErrorCode::DeviceUpdateFailed,*failure});
            }
            next_diagnostic=std::chrono::steady_clock::now()+std::chrono::seconds{5};
        }
        std::this_thread::sleep_for(std::chrono::seconds{1});
    }
    if(migration && logs) {
        auto url=base(endpoint,endpoint.server_port)+"/logs/error.log.updater-migration-"+artifact.sha256;
        auto response=cpr::Get(cpr::Url{url},cpr::Timeout{std::chrono::seconds{2}},cpr::Redirect{false});
        if(response.status_code==200) logs({"","bootstrap","ERROR",response.text.substr(0,64000)});
    }
    return std::unexpected(Error{ErrorCode::DeviceUpdateFailed,last_error+"; the ten-minute deadline expired. "
        +(migration?"Inspect the migration diagnostic log before retrying.":"Check the installed application and updater diagnostics before retrying.")});
}
}

bool secure_cloud_url(std::string_view url) {
    static const std::regex https(R"(^https://[A-Za-z0-9](?:[A-Za-z0-9.-]*[A-Za-z0-9])?(?::[0-9]{1,5})?(?:/[A-Za-z0-9._~%/-]*)?$)");
    return std::regex_match(url.begin(),url.end(),https) && url.find("..") == std::string_view::npos;
}

bool updater_ready(std::string_view input) {
    auto value=Json::parse(input,nullptr,false);
    if(!value.is_object()) return false;
    auto version=Version::from_string(text(value,"updater_version"));
    auto minimum=Version::from_string("1.2.0");
    return version && *version>=*minimum && number(value,"protocol_version")==1
           && flag(value,"ready_for_updates")
           && value.contains("pending_application_transaction") && value["pending_application_transaction"].is_boolean()
           && !value["pending_application_transaction"].get<bool>()
           && value.contains("recovery_error") && value["recovery_error"].is_null()
           && value.contains("compatibility") && value["compatibility"].is_array()
           && std::find(value["compatibility"].begin(),value["compatibility"].end(),"device_package_v1")!=value["compatibility"].end()
           && std::find(value["compatibility"].begin(),value["compatibility"].end(),"pc_verified_package_v1")!=value["compatibility"].end();
}

bool application_requires_framelab(const DeviceDescriptor &device,const VersionEntry &target) {
    return target.product_id=="aio" || device.framelab_supported;
}

bool application_health_ready(const DeviceDescriptor &device,const VersionEntry &target,std::string_view capabilities) {
    if(!compatible_api(device) || !device.modern || (target.product_id!="aio" && target.product_id!="nvr")
       || device.product_id!=target.product_id || device.profile!=target.profile
       || !device.build_version || *device.build_version!=target.version) return false;
    if(!application_requires_framelab(device,target)) return true;
    if(!device.framelab_supported || !device.framelab_ready) return false;
    const auto runtime=Json::parse(capabilities,nullptr,false);
    return runtime.is_object() && framelab_ready(runtime);
}

Result<DeviceDescriptor> parse_device_descriptor(std::string_view descriptor,std::string_view build) {
    auto value=Json::parse(descriptor,nullptr,false), compiled=Json::parse(build,nullptr,false);
    if(!value.is_object() || !compiled.is_object()) return std::unexpected(malformed("Device identity/build response is malformed"));
    DeviceDescriptor result;
    result.api_version=Version::from_string(text(value,"version"));
    result.build_version=Version::from_string(text(compiled,"version"));
    result.product_id=text(value,"product_id"); result.profile=text(value,"release_profile");
    result.modern=!result.product_id.empty() || value.contains("device_descriptor_version");
    if(result.modern && ((result.product_id!="aio" && result.product_id!="nvr") || number(value,"device_descriptor_version")!=1))
        return std::unexpected(malformed("Unknown device identity/schema; update target cannot be guessed"));
    if(result.modern && value.contains("device_type") && text(value,"device_type")!=result.product_id)
        return std::unexpected(malformed("Device type/product identity conflicts"));
    if(value.contains("capabilities") && value["capabilities"].is_object()) {
        const auto &caps=value["capabilities"];
        if(caps.contains("framelab") && caps["framelab"].is_object()) {
            auto &feature=caps["framelab"];
            result.framelab_supported=feature.contains("supported") && feature["supported"]==true;
            result.framelab_ready=result.framelab_supported && flag(feature,"enabled") && flag(feature,"ready");
        }
        if(caps.contains("digital_io") && caps["digital_io"].is_object()) {
            auto &feature=caps["digital_io"];
            if(feature.contains("supported") && feature["supported"].is_boolean()) result.digital_io_supported=feature["supported"].get<bool>();
            result.digital_io_ready=result.digital_io_supported.value_or(false) && flag(feature,"enabled") && flag(feature,"ready");
        }
    }
    return result;
}

Result<DeviceDescriptor> discover_device(const DeviceEndpoint &endpoint) {
    auto descriptor=get(base(endpoint,endpoint.server_port)+"/version");
    if(!descriptor) return std::unexpected(descriptor.error());
    auto build=get(base(endpoint,endpoint.server_port)+"/build_version");
    if(!build) return std::unexpected(build.error());
    return parse_device_descriptor(descriptor->dump(),build->dump());
}

Result<DeviceUpdateIdentity> parse_update_identity(DeviceDescriptor device, std::string_view capabilities) {
    const auto value = Json::parse(capabilities, nullptr, false);
    if (!value.is_object()) return std::unexpected(malformed("Invalid updater capabilities"));
    auto version = text(value, "updater_version");
    if (!Version::from_string(version))
        return std::unexpected(malformed("Updater capabilities lack a valid software version"));
    const auto hardware = value.find("device");
    if (hardware == value.end() || !hardware->is_object())
        return std::unexpected(malformed("Updater capabilities lack a device identity"));
    auto product = text(*hardware, "product_id");
    auto profile = text(*hardware, "release_profile");
    static const std::regex profile_name("(aio|nvr)-[a-z0-9]+([.-][a-z0-9]+)*");
    if ((product != "aio" && product != "nvr") || !profile.starts_with(product + "-")
        || !std::regex_match(profile, profile_name) || profile == product + "-unqualified")
        return std::unexpected(malformed("Updater returned an unknown device product or release profile"));
    if (device.modern && (device.product_id != product || (!device.profile.empty() && device.profile != profile)))
        return std::unexpected(malformed("Videoserver and updater disagree about the device identity"));
    device.product_id = std::move(product);
    device.profile = std::move(profile);
    device.modern = true;
    return DeviceUpdateIdentity{std::move(device), std::move(version)};
}

Result<DeviceUpdateIdentity> discover_update_identity(const DeviceEndpoint &endpoint, DeviceDescriptor device) {
    const auto capabilities = get(base(endpoint, endpoint.update_port) + "/capabilities");
    if (!capabilities) {
        if (capabilities.error().code == ErrorCode::HttpError && capabilities.error().http_status == 404)
            return DeviceUpdateIdentity{std::move(device), {}};
        return std::unexpected(capabilities.error());
    }
    return parse_update_identity(std::move(device), capabilities->dump());
}

Result<std::string> discover_updater_version(const DeviceEndpoint &endpoint) {
    auto capabilities=get(base(endpoint,endpoint.update_port)+"/capabilities");
    if(!capabilities) {
        if(capabilities.error().code==ErrorCode::HttpError && capabilities.error().http_status==404) return std::string{};
        return std::unexpected(capabilities.error());
    }
    auto version=text(*capabilities,"updater_version");
    if(!Version::from_string(version)) return std::unexpected(malformed("Updater capabilities lack a valid software version"));
    return version;
}

Result<OperationReceipt> parse_operation_receipt(std::string_view input) {
    auto value=Json::parse(input,nullptr,false);
    if(!value.is_object()) return std::unexpected(malformed("Operation receipt is malformed"));
    OperationReceipt result{.status=text(value,"status"),.archive_sha256=text(value,"archive_sha256"),
        .version=text(value,"version"),.source_commit=text(value,"source_commit"),
        .product_id=text(value,"product_id"),.profile=text(value,"profile"),.error=text(value,"error"),
        .role=text(value,"role"),.kind=text(value,"kind"),.outcome=text(value,"outcome")};
    if(result.status.empty() || result.archive_sha256.empty() || result.source_commit.empty()
       || (result.role!="application" && result.role!="updater-migration"))
        return std::unexpected(malformed("Operation receipt lacks package identity"));
    if(result.status!="pending" && result.status!="succeeded" && result.status!="failed" && result.status!="interrupted")
        return std::unexpected(malformed("Unknown updater operation status"));
    if((result.role=="application" && result.kind!="install" && result.kind!="rollback")
       || (result.role=="updater-migration" && result.kind!="updater-migration"))
        return std::unexpected(malformed("Updater receipt operation kind conflicts with its artifact role"));
    return result;
}

bool receipt_matches(const OperationReceipt &receipt,const VersionEntry &entry,const Artifact &artifact) {
    return receipt.archive_sha256==artifact.sha256 && receipt.version==entry.version.to_string()
           && receipt.source_commit==entry.source_commit && receipt.product_id==entry.product_id && receipt.profile==entry.profile
           && receipt.role==artifact.role
           && receipt.kind==(artifact.role=="application"?"install":"updater-migration");
}

std::optional<int> legacy_setup_return_code(std::string_view message) {
    // These are the pinned legacy updater's setup outcome formats, not arbitrary
    // callback output or its ambiguous failed_rollbacked terminal status.
    static const std::regex http(R"(setup\.sh from package root failed\. Status: failed, RC: (-?[0-9]+)(?![0-9A-Za-z]))");
    static const std::regex stream(R"(^Script setup\.sh failed with return code (-?[0-9]+)$)");
    std::match_results<std::string_view::const_iterator> match;
    if(!std::regex_search(message.begin(),message.end(),match,http)
       && !std::regex_match(message.begin(),message.end(),match,stream)) return std::nullopt;
    int result=0;
    const auto digits=match[1].str();
    const auto parsed=std::from_chars(digits.data(),digits.data()+digits.size(),result);
    if(parsed.ec!=std::errc{} || parsed.ptr!=digits.data()+digits.size()) return std::nullopt;
    return result;
}

bool migration_upload_pending(const Error &error) {
    const auto code=legacy_setup_return_code(error.message);
    return (!code || *code==75)
           && (error.code==ErrorCode::NetworkUnreachable || (error.code==ErrorCode::HttpError && error.http_status==500));
}

Result<void> migration_upload_result(const Result<void> &upload,std::optional<int> setup_return_code) {
    const auto response_code=upload?std::nullopt:legacy_setup_return_code(upload.error().message);
    // A known failure must never be hidden by an ambiguous reset or by exit 75
    // observed elsewhere. Exit 75 is a handoff signal, never a success receipt.
    const auto failure=response_code && *response_code!=75?response_code
        :setup_return_code && *setup_return_code!=75?setup_return_code:std::nullopt;
    if(failure) return std::unexpected(Error{ErrorCode::DeviceUpdateFailed,
        std::format("Updater migration setup failed with exit code {}; inspect migration diagnostics before retrying.",*failure)
        +(upload?std::string{}:" "+upload.error().message)});
    if(upload || migration_upload_pending(upload.error())) return {};
    return std::unexpected(upload.error());
}

Result<void> transfer_migration(const DeviceEndpoint &endpoint,const Handshake &handshake,
                                const PreparedTransfer &prepared,const std::filesystem::path &file,
                                ProgressCallback progress,LogCallback on_log,
                                std::chrono::milliseconds upload_timeout,std::chrono::milliseconds drain_timeout) {
    // Only the SSE worker writes this value; drain() joins it before we read it.
    std::optional<int> setup_result;
    auto logs=stream_update_logs(endpoint,handshake,prepared,[&](const LogEntry &entry) {
        if(entry.source=="execute_script" && entry.level=="ERROR") {
            if(auto code=legacy_setup_return_code(entry.message);code && (!setup_result || *code!=75))
                setup_result=code;
        }
        if(on_log) on_log(entry);
    });
    auto transfer=transfer_file_and_apply_update(endpoint,handshake,prepared,file,progress,upload_timeout);
    logs.drain(drain_timeout);
    return migration_upload_result(transfer,setup_result);
}

std::optional<std::string> migration_failure_log(std::string_view log,std::string_view archive_sha256) {
    std::istringstream lines(std::string(log.substr(log.size()>64000?log.size()-64000:0)));
    std::string line;
    std::optional<std::string> failure;
    while(std::getline(lines,line)) {
        auto value=Json::parse(line,nullptr,false);
        if(value.is_object() && text(value,"id")==archive_sha256 && text(value,"status")=="failed")
            failure=text(value,"error").empty()?"Updater migration failed; inspect recovery diagnostics":text(value,"error");
    }
    return failure;
}

Result<DeviceUpdatePlan> plan_device_update(const VersionMatrix &matrix,const DeviceDescriptor &device,
                                           std::string_view updater_version,std::string_view environment,bool allow_legacy_aio_confirmation) {
    return plan_device_update(matrix, device, updater_version, environment,
                              allow_legacy_aio_confirmation ? LegacyDeviceProduct::Aio : LegacyDeviceProduct::Unspecified);
}

namespace {
Result<DeviceUpdatePlan> plan_device_target(const VersionMatrix &matrix, const DeviceDescriptor &device,
                                           std::string_view updater_version, std::string_view environment,
                                           LegacyDeviceProduct confirmed_product, std::optional<Version> requested) {
    if(environment!="release" && environment!="dev") return std::unexpected(malformed("Invalid update environment"));
    if(!compatible_api(device))
        return std::unexpected(Error{ErrorCode::VersionNotFound,"No compatible update can be selected for this device API; application build version cannot substitute for API compatibility"});
    const auto advertised_updater=Version::from_string(updater_version);
    if(advertised_updater && *advertised_updater<Version(1,2,0))
        return std::unexpected(Error{ErrorCode::VersionNotFound,"This modern updater lacks offline PC approval support; reviewed maintenance is required before updating"});
    const bool legacy=!device.modern;
    std::string product=device.product_id;
    std::string profile=device.profile;
    if (legacy) {
        switch (confirmed_product) {
        case LegacyDeviceProduct::Aio:
            product="aio";
            profile="aio-jetson-orin-nx-l4t36.4.3-aarch64-cp310";
            break;
        case LegacyDeviceProduct::Nvr:
            product="nvr";
            profile.clear();
            break;
        default:
            return std::unexpected(malformed("Choose AIO or NVR to check a factory device for updates"));
        }
    } else if (profile.empty() || profile==product+"-unqualified") {
        return std::unexpected(Error{ErrorCode::PlatformNotFound,"Device runtime profile is not established; no platform fallback is allowed"});
    }
    if (requested && product != "aio")
        return std::unexpected(Error{ErrorCode::PlatformNotFound, "Explicit version selection requires an AIO device"});
    const bool factory_nvr=legacy && confirmed_product==LegacyDeviceProduct::Nvr;
    const VersionEntry *best=nullptr;
    for(const auto &entry:matrix.device_versions) {
        if (requested && entry.version != *requested) continue;
        const auto application=role(entry,"application");
        const auto offline_minimum=application?Version::from_string(application->minimum_updater):std::nullopt;
        if(!offline_minimum || *offline_minimum<Version(1,2,0)) continue;
        if(entry.product_id!=product || entry.platform!="linux-arm64"
           || entry.environment!=environment || entry.status!="stable"
           || (legacy && !role(entry,"updater-migration"))) continue;
        if (factory_nvr) {
            if (!entry.nvr_factory_migration) continue;
            if (best && best->profile != entry.profile)
                return std::unexpected(Error{ErrorCode::PlatformNotFound,"Multiple NVR profiles are available. The factory updater cannot identify which one fits this device."});
        } else if (entry.profile!=profile) continue;
        if (requested && best)
            return std::unexpected(malformed("The catalog contains multiple matches for the requested AIO version"));
        if(!best || entry.version>best->version) best=&entry;
    }
    if(!best) {
        if (requested)
            return std::unexpected(Error{ErrorCode::VersionNotFound, std::format(
                "No compatible AIO {} package for profile {} on the {} channel", requested->to_string(), profile, environment)});
        if (legacy) return std::unexpected(Error{ErrorCode::VersionNotFound, std::format(
            "No compatible {} factory migration package is available on the {} channel.", factory_nvr ? "NVR" : "AIO", environment)});
        return std::unexpected(Error{ErrorCode::VersionNotFound,"No approved package for this device/profile/channel; legacy ARM64 feed will not be substituted"});
    }
    if(!requested && device.build_version && *device.build_version>best->version)
        return std::unexpected(Error{ErrorCode::DowngradeRejected,"Device application is newer than this catalog; downgrade is blocked"});
    auto updater=Version::from_string(updater_version), minimum=Version::from_string(role(*best,"application")->minimum_updater);
    if(!minimum) return std::unexpected(malformed("Application minimum updater is missing"));
    bool migration=!updater || *updater<*minimum;
    if(migration && !role(*best,"updater-migration"))
        return std::unexpected(Error{ErrorCode::VersionNotFound,"Matching updater migration is required before this application package"});
    const bool update_available = requested || !device.build_version || *device.build_version < best->version || migration;
    DeviceUpdatePlan plan{device,*best,update_available,migration,legacy && !factory_nvr && migration,std::string(environment)};
    return plan;
}
} // namespace

Result<DeviceUpdatePlan> plan_device_update(const VersionMatrix &matrix, const DeviceDescriptor &device,
                                           std::string_view updater_version, std::string_view environment,
                                           LegacyDeviceProduct confirmed_product) {
    return plan_device_target(matrix, device, updater_version, environment, confirmed_product, std::nullopt);
}

Result<DeviceUpdatePlan> plan_device_version(const VersionMatrix &matrix, const DeviceDescriptor &device,
                                            std::string_view updater_version, std::string_view environment,
                                            LegacyDeviceProduct confirmed_product, const Version &target_version) {
    return plan_device_target(matrix, device, updater_version, environment, confirmed_product, target_version);
}

Result<void> install_device_release(const std::string &cloud,const DeviceEndpoint &endpoint,const DeviceUpdatePlan &plan,
                                   ProgressCallback progress,LogCallback on_log,UpdatePhaseCallback phase) {
    auto notify=[&](std::string_view message){if(phase)phase(message);};
    bool migration=plan.migration_required;
    if(migration) {
        auto caps=get(base(endpoint,endpoint.update_port)+"/capabilities");
        if(caps) {
            const auto application=role(plan.target,"application");
            const auto current=Version::from_string(text(*caps,"updater_version"));
            const auto minimum=application?Version::from_string(application->minimum_updater):std::nullopt;
            if(!updater_ready(caps->dump()) || !current || !minimum || *current<*minimum)
                return std::unexpected(Error{ErrorCode::DeviceUpdateFailed,"Existing updater is not ready for offline application updates; inspect recovery diagnostics before retrying"});
            if(!caps->contains("device") || !(*caps)["device"].is_object()
               || text((*caps)["device"],"product_id")!=plan.target.product_id
               || text((*caps)["device"],"release_profile")!=plan.target.profile)
                return std::unexpected(malformed("Updater device identity differs from the selected package"));
            migration=false;
        } else if(caps.error().code!=ErrorCode::HttpError || caps.error().http_status!=404) {
            return std::unexpected(caps.error());
        } else if(plan.target.product_id=="nvr" && !plan.target.nvr_factory_migration) {
            return std::unexpected(Error{ErrorCode::VersionNotFound,"This NVR package does not support factory updater migration."});
        }
    }
    std::vector<std::string_view> roles;
    if(migration) roles.push_back("updater-migration");
    roles.push_back("application");
    for(auto wanted:roles) {
        auto artifact=role(plan.target,wanted);
        if(!artifact) return std::unexpected(malformed("Required artifact role missing"));
        if(wanted=="application") {
            auto caps=get(base(endpoint,endpoint.update_port)+"/capabilities");
            if(!caps) return std::unexpected(caps.error());
            auto current=Version::from_string(text(*caps,"updater_version"));
            auto minimum=Version::from_string(artifact->minimum_updater);
            if(!current || !minimum || *current<*minimum)
                return std::unexpected(malformed("A newer compatible updater is required before the application package"));
            if(!updater_ready(caps->dump()))
                return std::unexpected(Error{ErrorCode::DeviceUpdateFailed,
                    text(*caps,"recovery_error").empty()?"Updater is not ready for application updates; inspect pending recovery diagnostics before retrying"
                    :"Updater recovery failed: "+text(*caps,"recovery_error")});
            if(!caps->contains("device") || !(*caps)["device"].is_object()
               || text((*caps)["device"],"product_id")!=plan.target.product_id
               || text((*caps)["device"],"release_profile")!=plan.target.profile)
                return std::unexpected(malformed("Updater device identity differs from the selected package"));
        }
        auto selected=plan.target; selected.files={*artifact};
        if (wanted=="application") notify("Downloading application");
        else notify(plan.target.product_id=="nvr" ? "Downloading NVR updater migration" : "Downloading AIO updater migration");
        auto downloaded=download_artifact(cloud,selected,progress);
        if(!downloaded) return std::unexpected(downloaded.error());
        struct Cleanup {std::filesystem::path path; ~Cleanup(){std::error_code ignored;std::filesystem::remove(path,ignored);}} cleanup{downloaded->path};
        notify("Connecting to updater");
        auto handshake=perform_handshake(endpoint);
        if(!handshake) return std::unexpected(handshake.error());
        auto prepared=prepare_transfer(endpoint,*handshake,*downloaded,std::chrono::seconds{10},plan.environment);
        if(!prepared) return std::unexpected(prepared.error());
        Result<void> transfer;
        if(wanted=="updater-migration") {
            notify(plan.target.product_id=="nvr" ? "Verifying NVR and migrating updater" : "Verifying AIO and migrating updater");
            transfer=transfer_migration(endpoint,*handshake,*prepared,downloaded->path,progress,on_log);
        } else {
            auto logs=stream_update_logs(endpoint,*handshake,*prepared,on_log?on_log:[](const LogEntry&){});
            notify("Installing application");
            transfer=transfer_file_and_apply_update(endpoint,*handshake,*prepared,downloaded->path,progress,std::chrono::minutes{10});
            logs.stop();
            if(!transfer && migration_upload_pending(transfer.error())) transfer={};
        }
        if(!transfer) return transfer;
        notify(wanted=="application"?"Verifying application identity and required capabilities":"Waiting for durable updater migration result");
        auto verified=wait_operation(endpoint,plan.target,*artifact,wanted=="updater-migration",on_log);
        if(!verified) return verified;
    }
    return {};
}
}
