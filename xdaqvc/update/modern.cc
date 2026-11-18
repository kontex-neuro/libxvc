#include "modern.h"
#include "artifact.h"
#include "device.h"
#include <algorithm>
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
    if(number(value,"protocol")!=1 || !value.contains("sandbox") || !value["sandbox"].is_object()) return false;
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
                    return std::unexpected(Error{ErrorCode::DeviceUpdateFailed,parsed->error.empty()?"Device restored its previous release":parsed->error});
                if(parsed->status=="succeeded") {
                    last_error="Receipt succeeded, but updater recovery or readiness is not complete";
                    auto caps=get(base(endpoint,endpoint.update_port)+"/capabilities");
                    if(caps && !text(*caps,"recovery_error").empty())
                        return std::unexpected(Error{ErrorCode::DeviceUpdateFailed,"Updater recovery failed: "+text(*caps,"recovery_error")});
                    if(caps && updater_ready(caps->dump())) {
                        if(migration) return {};
                        auto identity=discover_device(endpoint);
                        auto framelab=get(base(endpoint,endpoint.server_port)+"/framelab/v1/capabilities");
                        if(identity && identity->product_id==entry.product_id && identity->profile==entry.profile
                           && identity->build_version && *identity->build_version==entry.version
                           && framelab && framelab_ready(*framelab)) return {};
                        last_error="Receipt succeeded, but expected application build/device/FrameLab health is not ready";
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
    return std::unexpected(Error{ErrorCode::DeviceUpdateFailed,last_error+"; the ten-minute deadline expired. Inspect the migration diagnostic log before retrying."});
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
    auto minimum=Version::from_string("1.1.0");
    return version && *version>=*minimum && number(value,"protocol_version")==1
           && flag(value,"ready_for_updates")
           && value.contains("pending_application_transaction") && value["pending_application_transaction"].is_boolean()
           && !value["pending_application_transaction"].get<bool>()
           && value.contains("recovery_error") && value["recovery_error"].is_null()
           && value.contains("compatibility") && value["compatibility"].is_array()
           && std::find(value["compatibility"].begin(),value["compatibility"].end(),"device_package_v1")!=value["compatibility"].end();
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
    if(value.contains("device_type") && text(value,"device_type")!=result.product_id)
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

bool migration_upload_pending(const Error &error) {
    return error.code==ErrorCode::NetworkUnreachable || error.code==ErrorCode::HttpError && error.http_status==500;
}

std::optional<std::string> migration_failure_log(std::string_view log,std::string_view archive_sha256) {
    std::istringstream lines(std::string(log.substr(log.size()>64000?log.size()-64000:0)));
    std::string line;
    std::optional<std::string> failure;
    while(std::getline(lines,line)) {
        auto value=Json::parse(line,nullptr,false);
        if(value.is_object() && text(value,"id")==archive_sha256 && text(value,"status")=="failed")
            failure=text(value,"error").empty()?"Updater migration restored the known legacy updater":text(value,"error");
    }
    return failure;
}

Result<DeviceUpdatePlan> plan_device_update(const VersionMatrix &matrix,const DeviceDescriptor &device,
                                           std::string_view updater_version,std::string_view environment,bool allow_legacy_aio_confirmation) {
    if(environment!="release" && environment!="dev") return std::unexpected(malformed("Invalid update environment"));
    if(!device.api_version || device.api_version->major()!=0 || device.api_version->minor()!=1)
        return std::unexpected(malformed("Device API compatibility is not established; application build version cannot substitute for API version"));
    const bool legacy=!device.modern;
    if(legacy && !allow_legacy_aio_confirmation) return std::unexpected(malformed("Unidentified legacy device requires explicit approved AIO migration confirmation"));
    const std::string product=legacy?"aio":device.product_id;
    const std::string profile=legacy?"aio-jetson-orin-nx-l4t36.4.3-aarch64-cp310":device.profile;
    if(profile.empty()) return std::unexpected(Error{ErrorCode::PlatformNotFound,"Device runtime profile is not established; no platform fallback is allowed"});
    const VersionEntry *best=nullptr;
    for(const auto &entry:matrix.device_versions) {
        if(entry.product_id!=product || entry.profile!=profile || entry.platform!="linux-arm64"
           || entry.environment!=environment || entry.status!="stable" || !role(entry,"application")
           || (legacy && !role(entry,"updater-migration"))) continue;
        if(!best || entry.version>best->version) best=&entry;
    }
    if(!best) return std::unexpected(Error{ErrorCode::VersionNotFound,"No approved package for this device/profile/channel; legacy ARM64 feed will not be substituted"});
    if(device.build_version && *device.build_version>best->version)
        return std::unexpected(Error{ErrorCode::DowngradeRejected,"Device application is newer than this catalog; downgrade is blocked"});
    auto updater=Version::from_string(updater_version), minimum=Version::from_string(role(*best,"application")->minimum_updater);
    if(!minimum) return std::unexpected(malformed("Application minimum updater is missing"));
    bool migration=legacy || !updater || *updater<*minimum;
    if(migration && !role(*best,"updater-migration"))
        return std::unexpected(Error{ErrorCode::VersionNotFound,"Matching updater migration is required before this application package"});
    DeviceUpdatePlan plan{device,*best,!device.build_version || *device.build_version<best->version || migration,migration,legacy,std::string(environment)};
    return plan;
}

Result<void> install_device_release(const std::string &cloud,const DeviceEndpoint &endpoint,const DeviceUpdatePlan &plan,
                                   ProgressCallback progress,LogCallback on_log,UpdatePhaseCallback phase) {
    auto notify=[&](std::string_view message){if(phase)phase(message);};
    std::vector<std::string_view> roles;
    if(plan.migration_required) roles.push_back("updater-migration");
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
        notify(wanted=="application"?"Downloading application":"Downloading AIO updater migration");
        auto downloaded=download_artifact(cloud,selected,progress);
        if(!downloaded) return std::unexpected(downloaded.error());
        struct Cleanup {std::filesystem::path path; ~Cleanup(){std::error_code ignored;std::filesystem::remove(path,ignored);}} cleanup{downloaded->path};
        notify("Connecting to updater");
        auto handshake=perform_handshake(endpoint);
        if(!handshake) return std::unexpected(handshake.error());
        auto prepared=prepare_transfer(endpoint,*handshake,*downloaded,std::chrono::seconds{10},plan.environment);
        if(!prepared) return std::unexpected(prepared.error());
        auto logs=stream_update_logs(endpoint,*handshake,*prepared,on_log?on_log:[](const LogEntry&){});
        notify(wanted=="application"?"Installing application":"Verifying AIO and migrating updater");
        auto transfer=transfer_file_and_apply_update(endpoint,*handshake,*prepared,downloaded->path,progress,std::chrono::minutes{10});
        logs.stop();
        if(!transfer && !migration_upload_pending(transfer.error()))
            return std::unexpected(transfer.error());
        notify(wanted=="application"?"Verifying application build and FrameLab":"Waiting for durable updater migration result");
        auto verified=wait_operation(endpoint,plan.target,*artifact,wanted=="updater-migration",on_log);
        if(!verified) return verified;
    }
    return {};
}
}
