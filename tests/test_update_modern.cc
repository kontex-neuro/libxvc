#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include "update/update.h"
using namespace xvc::update;
using Json=nlohmann::json;

namespace {
Json device_entry(std::string product="aio",std::string version="1.4.2") {
    const auto profile=product=="aio"?"aio-jetson-orin-nx-l4t36.4.3-aarch64-cp310":"nvr-cm5-cp310";
    Json entry={{"version",version},{"platform","linux-arm64"},{"product_id",product},{"device_type",product},
                {"profile",profile},{"source_commit",std::string(40,'a')},{"environment","release"},{"status","stable"},
                {"download_base","/dist/v"+version+"/"+product+"/"+profile+"/linux-arm64/"}};
    entry["files"]=Json::array({{{"name","application.tar.xz"},{"size",1},{"sha256",std::string(64,'b')},
                               {"role","application"},{"minimum_updater","1.2.0"},{"package_metadata_sha256",std::string(64,'c')}},
                              {{"name","migration.tar.xz"},{"size",1},{"sha256",std::string(64,'d')},
                               {"role","updater-migration"},{"minimum_updater","0.0.1"},{"package_metadata_sha256",std::string(64,'e')}}});
    return entry;
}
Json descriptor(std::string product="aio") {
    return {{"version","0.1.2"},{"product_id",product},{"device_descriptor_version",1},
            {"release_profile",product=="aio"?"aio-jetson-orin-nx-l4t36.4.3-aarch64-cp310":"nvr-cm5-cp310"},
            {"capabilities",{{"framelab",{{"supported",true},{"enabled",true},{"ready",true}}},
                             {"digital_io",{{"supported",nullptr},{"enabled",false},{"ready",false},{"state","deferred"}}}}}};
}
VersionMatrix matrix(std::initializer_list<Json> entries) {
    Json data={{"versions",Json::array()},{"device_versions",entries}};
    auto parsed=parse_versions_json(data.dump()); REQUIRE(parsed); return *parsed;
}
}

TEST_CASE("Modern catalog retains legacy metadata and filters by product profile instead of ARM64", "[update][modern]") {
    auto catalog=matrix({device_entry("aio"),device_entry("nvr")});
    auto identity=parse_device_descriptor(descriptor().dump(),R"({"version":"1.4.1"})"); REQUIRE(identity);
    REQUIRE(identity->api_version==Version(0,1,2)); REQUIRE(identity->build_version==Version(1,4,1));
    auto plan=plan_device_update(catalog,*identity,"1.2.0"); REQUIRE(plan);
    REQUIRE(plan->target.product_id=="aio"); REQUIRE(plan->target.version==Version(1,4,2));
    identity->profile="aio-unmatched";
    REQUIRE_FALSE(plan_device_update(catalog,*identity,"1.2.0"));
}

TEST_CASE("An explicit AIO version can upgrade, reinstall or downgrade within its channel", "[update][modern][explicit]") {
    const Version selected{0,1,9};
    auto dev = device_entry("aio", "0.1.9");
    dev["environment"] = "dev";
    dev["download_base"] = "/dev/v0.1.9/aio/aio-jetson-orin-nx-l4t36.4.3-aarch64-cp310/linux-arm64/";
    auto release = device_entry("aio", "0.1.9");
    release["files"][0]["sha256"] = std::string(64, 'f');
    const auto catalog = matrix({device_entry("aio","0.1.10"), release, dev, device_entry("nvr","0.1.9")});
    auto device = parse_device_descriptor(descriptor().dump(), R"({"version":"0.1.8"})").value();
    for (const Version current : {Version(0,1,8), selected, Version(0,1,10)}) {
        device.build_version = current;
        auto plan = plan_device_version(catalog, device, "1.2.1", "release", LegacyDeviceProduct::Unspecified, selected);
        REQUIRE(plan);
        CHECK(plan->update_available);
        CHECK(plan->exact_version_requested);
        CHECK_FALSE(plan->migration_required);
        CHECK(plan->target.version == selected);
        CHECK(plan->target.product_id == "aio");
        CHECK(plan->environment == "release");
        CHECK(plan->target.files.front().sha256 == std::string(64, 'f'));
    }
    auto plan = plan_device_version(catalog, device, "1.2.1", "dev", LegacyDeviceProduct::Unspecified, selected);
    REQUIRE(plan);
    CHECK(plan->environment == "dev");
    CHECK(plan->target.files.front().sha256 == std::string(64, 'b'));
    auto latest = plan_device_update(catalog, device, "1.2.1", "release");
    REQUIRE(latest);
    CHECK(latest->target.version == Version(0,1,10));
    CHECK_FALSE(latest->update_available);
    CHECK_FALSE(latest->exact_version_requested);
    const auto latest_dev = plan_device_update(catalog, device, "1.2.1", "dev");
    REQUIRE_FALSE(latest_dev);
    CHECK(latest_dev.error().code == ErrorCode::DowngradeRejected);
}

TEST_CASE("An explicit version never falls back to another package", "[update][modern][explicit]") {
    const Version selected{0,1,9};
    const auto aio = device_entry("aio","0.1.9");
    auto catalog = matrix({aio, device_entry("aio","0.1.10"), device_entry("nvr","0.1.9")});
    auto device = parse_device_descriptor(descriptor().dump(), R"({"version":"0.1.7"})").value();
    auto plan = [&] { return plan_device_version(catalog, device, "1.2.1", "release", LegacyDeviceProduct::Aio, selected); };
    SECTION("missing device entry, even when present in the legacy catalog") {
        catalog.versions.push_back(catalog.device_versions.front());
        catalog.device_versions.erase(catalog.device_versions.begin());
    }
    SECTION("unapproved status") { catalog.device_versions.front().status = "deprecated"; }
    SECTION("wrong runtime profile") { catalog.device_versions.front().profile = "aio-other"; }
    SECTION("wrong platform") { catalog.device_versions.front().platform = "linux-x86_64"; }
    SECTION("another channel") { catalog.device_versions.front().environment = "dev"; }
    SECTION("ambiguous exact entry") { catalog = matrix({aio, aio}); }
    SECTION("device reports NVR despite AIO confirmation") {
        device = parse_device_descriptor(descriptor("nvr").dump(), R"({"version":"0.1.7"})").value();
    }
    SECTION("incompatible device API") { device.api_version = Version(0,2,0); }
    REQUIRE_FALSE(plan());
}

TEST_CASE("The version list contains exactly the reviewable AIO releases in descending order", "[update][modern][version-list]") {
    auto catalog = matrix({device_entry("aio","0.1.2"), device_entry("aio","0.1.10"), device_entry("aio","0.1.9")});
    const auto device = parse_device_descriptor(descriptor().dump(), R"({"version":"0.1.9"})").value();
    auto add = [&](int patch) -> VersionEntry & {
        auto &entry = catalog.device_versions.emplace_back(catalog.device_versions.front());
        entry.version = Version(0, 1, patch);
        return entry;
    };
    add(2).environment = "dev";             // Same version, distinct channel.
    add(11).environment = "dev";
    add(12).status = "deprecated";
    add(13).profile = "aio-other";
    add(14).platform = "linux-x86_64";
    add(15).product_id = "nvr";
    add(16).files.clear();
    add(17).files.front().minimum_updater = "1.1.0";
    add(18).files.pop_back();
    catalog.device_versions.back().files.front().minimum_updater = "1.3.0";
    add(19).files.front().minimum_updater = "1.3.0"; // Factory bootstrap cannot replace a detected managed updater.
    add(20);
    add(20);                                 // Two approved entries in one channel.
    auto generic = catalog.device_versions.front();
    generic.version = Version(0, 1, 21);
    catalog.versions.push_back(generic);

    const auto product = LegacyDeviceProduct::Unspecified;
    for (const std::string_view channel : {"release", "dev"}) {
        const auto list = list_device_versions(catalog, device, "1.2.1", channel, product);
        REQUIRE(list);
        const std::vector<Version> expected = channel == "release"
            ? std::vector<Version>{{0,1,10}, {0,1,9}, {0,1,2}}
            : std::vector<Version>{{0,1,11}, {0,1,2}};
        CHECK(*list == expected);
        for (int patch = 0; patch <= 21; ++patch) {
            const Version version{0, 1, patch};
            const auto review = plan_device_version(catalog, device, "1.2.1", channel, product, version);
            CHECK(review.has_value() == (std::find(list->begin(), list->end(), version) != list->end()));
        }
    }
    catalog.device_versions.clear();
    const auto empty = list_device_versions(catalog, device, "1.2.1", "release", product);
    REQUIRE(empty);
    CHECK(empty->empty());
}

TEST_CASE("Version listing requires a compatible device or confirmed factory AIO", "[update][modern][version-list]") {
    auto catalog = matrix({device_entry("aio","0.1.9"), device_entry("aio","0.1.10")});
    catalog.device_versions.front().files.front().minimum_updater = "1.2.1";
    catalog.device_versions.back().files.pop_back();
    const auto factory = parse_device_descriptor(R"({"version":"0.1.3"})", R"({"version":"0.0.32"})").value();
    const auto product = LegacyDeviceProduct::Aio;
    CHECK_FALSE(list_device_versions(catalog, factory, "", "release", LegacyDeviceProduct::Unspecified));
    CHECK_FALSE(list_device_versions(catalog, factory, "", "release", LegacyDeviceProduct::Nvr));
    CHECK_FALSE(list_device_versions(catalog, factory, "1.1.0", "release", product));
    const auto releases = list_device_versions(catalog, factory, "", "release", product);
    REQUIRE(releases);
    const std::vector<Version> expected{{0,1,9}};
    CHECK(*releases == expected);
    const auto managed = parse_device_descriptor(descriptor().dump(), R"({"version":"0.1.8"})").value();
    const auto limited = list_device_versions(catalog, managed, "1.2.0", "release", product);
    REQUIRE(limited);
    CHECK(*limited == std::vector<Version>{Version(0,1,10)});
    CHECK_FALSE(plan_device_version(catalog, managed, "1.2.0", "release", product, Version(0,1,9)));
    auto device = parse_device_descriptor(descriptor("nvr").dump(), R"({"version":"0.1.9"})").value();
    CHECK_FALSE(list_device_versions(catalog, device, "1.2.1", "release", product));
    device = parse_device_descriptor(descriptor().dump(), R"({"version":"0.1.9"})").value();
    CHECK_FALSE(list_device_versions(catalog, device, "1.2.1", "other", product));
    device.api_version = Version(0,2,0);
    CHECK_FALSE(list_device_versions(catalog, device, "1.2.1", "release", product));
}

TEST_CASE("An exact application install requires acknowledgement before polling a reusable archive receipt", "[update][modern][explicit]") {
    const auto catalog = matrix({device_entry("aio","0.1.9")});
    auto device = parse_device_descriptor(descriptor().dump(), R"({"version":"0.1.9"})").value();
    const auto exact = plan_device_version(catalog, device, "1.2.1", "release",
                                           LegacyDeviceProduct::Unspecified, Version(0,1,9)).value();
    device.build_version = Version(0,1,8);
    const auto latest = plan_device_update(catalog, device, "1.2.1").value();
    REQUIRE(application_upload_result(exact, {}));
    const Error reset{ErrorCode::NetworkUnreachable, "connection reset"};
    const Error server_error{ErrorCode::HttpError, "upload interrupted", 500};
    for (const auto &error : {reset, server_error}) {
        const auto result = application_upload_result(exact, std::unexpected(error));
        REQUIRE_FALSE(result);
        CHECK(result.error().code == error.code);
        CHECK(result.error().http_status == error.http_status);
        REQUIRE(application_upload_result(latest, std::unexpected(error)));
        REQUIRE(migration_upload_result(std::unexpected(error)));
    }
    const Error refused{ErrorCode::HttpError, "package refused", 412};
    CHECK_FALSE(application_upload_result(exact, std::unexpected(refused)));
    CHECK_FALSE(application_upload_result(latest, std::unexpected(refused)));
}

TEST_CASE("Explicit factory AIO requests retain product confirmation and migration checks", "[update][modern][explicit]") {
    const auto factory = parse_device_descriptor(R"({"version":"0.1.3"})", R"({"version":"0.0.32"})").value();
    const Version selected{0,1,9};
    auto catalog = matrix({device_entry("aio","0.1.9"), device_entry("aio","0.1.10")});
    auto plan = [&](LegacyDeviceProduct product, std::string_view updater) {
        return plan_device_version(catalog, factory, updater, "release", product, selected);
    };
    REQUIRE_FALSE(plan(LegacyDeviceProduct::Unspecified, ""));
    REQUIRE_FALSE(plan(LegacyDeviceProduct::Nvr, ""));
    REQUIRE_FALSE(plan(LegacyDeviceProduct::Aio, "1.1.0"));
    auto migration = plan(LegacyDeviceProduct::Aio, "");
    REQUIRE(migration);
    CHECK(migration->migration_required);
    CHECK(migration->legacy_aio_confirmation);
    CHECK(migration->target.version == selected);
    catalog.device_versions.front().files.pop_back();
    REQUIRE_FALSE(plan(LegacyDeviceProduct::Aio, ""));
}

TEST_CASE("FrameLab does not imply AIO digital IO and malformed flags never enable controls", "[update][modern]") {
    auto input=descriptor();
    auto identity=parse_device_descriptor(input.dump(),R"({"version":"1.4.1"})"); REQUIRE(identity);
    REQUIRE(identity->framelab_ready); REQUIRE_FALSE(identity->digital_io_supported.has_value()); REQUIRE_FALSE(identity->digital_io_ready);
    input["capabilities"]["framelab"]["ready"]="true";
    identity=parse_device_descriptor(input.dump(),R"({"version":"1.4.1"})"); REQUIRE(identity); REQUIRE_FALSE(identity->framelab_ready);
    input["device_type"]="nvr"; REQUIRE_FALSE(parse_device_descriptor(input.dump(),R"({"version":"1.4.1"})"));
}

TEST_CASE("Unknown legacy builds require explicit approved AIO migration and never generic feed", "[update][modern]") {
    auto old=parse_device_descriptor(R"({"version":"0.1.2"})",R"({"version":"x.x.x"})"); REQUIRE(old);
    REQUIRE_FALSE(old->modern);REQUIRE(old->product_id.empty());REQUIRE(old->profile.empty());
    auto catalog=matrix({device_entry("aio"),device_entry("nvr")});
    REQUIRE_FALSE(plan_device_update(catalog,*old,""));
    REQUIRE(old->product_id.empty());REQUIRE(old->profile.empty());
    auto plan=plan_device_update(catalog,*old,"","release",true); REQUIRE(plan);
    REQUIRE(plan->migration_required); REQUIRE(plan->legacy_aio_confirmation); REQUIRE(plan->target.product_id=="aio");
    catalog.device_versions[0].files.pop_back();
    REQUIRE_FALSE(plan_device_update(catalog,*old,"","release",true));
}

TEST_CASE("Factory product choice selects only a matching hardware-bound migration", "[update][modern]") {
    const auto device=parse_device_descriptor(R"({"version":"0.1.3"})",R"({"version":"0.1.3"})").value();
    auto aio=device_entry("aio","0.1.10");
    auto nvr=device_entry("nvr","0.1.11");
    nvr["qualification"]={{"state","accepted"}};
    const Json target={{"architecture","aarch64"},{"model_contains","Raspberry Pi Compute Module 5"},
        {"os_id","debian"},{"os_version","12"},{"python_abi","cp311"},{"kernel","example-kernel"}};
    for(auto& file:nvr["files"]) file["target"]=target;
    nvr["files"][1]["components"]={{"updater","1.2.1"}};
    auto plan_nvr=[&](const Json& entry) {
        return plan_device_update(matrix({aio,entry}),device,"","release",LegacyDeviceProduct::Nvr);
    };
    const auto both=matrix({aio,nvr});
    auto plan=plan_device_update(both,device,"","release",LegacyDeviceProduct::Aio);
    REQUIRE(plan);
    REQUIRE(plan->target.product_id=="aio");
    REQUIRE(plan->target.version==Version(0,1,10));
    REQUIRE(plan->migration_required);
    plan=plan_nvr(nvr);
    REQUIRE(plan);
    REQUIRE(plan->target.product_id=="nvr");
    REQUIRE(plan->target.version==Version(0,1,11));
    REQUIRE(plan->migration_required);
    REQUIRE_FALSE(plan->device.modern);
    REQUIRE_FALSE(plan->legacy_aio_confirmation);
    REQUIRE_FALSE(plan_device_update(both,device,"","release",LegacyDeviceProduct::Unspecified));
    REQUIRE_FALSE(plan_device_update(matrix({aio}),device,"","release",LegacyDeviceProduct::Nvr));

    auto invalid=nvr;
    invalid["files"].erase(1);
    REQUIRE_FALSE(plan_nvr(invalid));
    invalid=nvr; invalid["files"][1]["target"]["model_contains"]="Jetson Orin NX";
    REQUIRE_FALSE(plan_nvr(invalid));
    invalid=nvr;
    for(auto& file:invalid["files"]) file["target"]["model_contains"]="Jetson Orin NX";
    REQUIRE_FALSE(plan_nvr(invalid));
    invalid=nvr; invalid["files"][1]["target"].erase("kernel");
    REQUIRE_FALSE(plan_nvr(invalid));
    invalid=nvr; invalid["qualification"]["state"]="candidate";
    REQUIRE_FALSE(plan_nvr(invalid));
    invalid=nvr; invalid["files"][1]["components"]["updater"]="1.1.0";
    REQUIRE_FALSE(plan_nvr(invalid));
    auto second=nvr;
    auto set_profile=[](Json& value,std::string profile) {
        value["profile"]=profile;
        value["download_base"]="/dist/v"+value.at("version").get<std::string>()+"/nvr/"+profile+"/linux-arm64/";
    };
    set_profile(second,"nvr-unqualified");
    REQUIRE_FALSE(plan_nvr(second));
    auto unqualified=descriptor("nvr");
    unqualified["release_profile"]="nvr-unqualified";
    auto identified=parse_device_descriptor(unqualified.dump(),R"({"version":"0.1.3"})").value();
    REQUIRE_FALSE(plan_device_update(matrix({second}),identified,"","release",LegacyDeviceProduct::Nvr));
    set_profile(second,"nvr-cm5-alternative");
    REQUIRE_FALSE(plan_device_update(matrix({nvr,second}),device,"","release",LegacyDeviceProduct::Nvr));
}

TEST_CASE("A modern updater identifies an interrupted factory migration before Desktop asks for a product", "[update][modern]") {
    const auto legacy=parse_device_descriptor(R"({"version":"0.1.3"})",R"({"version":"0.1.3"})").value();
    const std::string profile="aio-jetson-orin-nx-l4t36.4.3-aarch64-cp310";
    Json caps={{"updater_version","1.2.1"},{"device",{{"product_id","aio"},{"release_profile",profile}}}};
    auto identity=parse_update_identity(legacy,caps.dump());
    REQUIRE(identity);
    REQUIRE(identity->device.modern);
    REQUIRE(identity->device.build_version==Version(0,1,3));
    REQUIRE(identity->updater_version=="1.2.1");
    auto plan=plan_device_update(matrix({device_entry("aio"),device_entry("nvr")}),identity->device,
                                 identity->updater_version,"release",LegacyDeviceProduct::Nvr);
    REQUIRE(plan);
    REQUIRE(plan->target.product_id=="aio");
    REQUIRE_FALSE(plan->migration_required);
    const auto modern_nvr=parse_device_descriptor(descriptor("nvr").dump(),R"({"version":"0.1.9"})").value();
    REQUIRE_FALSE(parse_update_identity(modern_nvr,caps.dump()));
    auto nvr_caps=caps;
    nvr_caps["device"]={{"product_id","nvr"},{"release_profile",modern_nvr.profile}};
    identity=parse_update_identity(legacy,nvr_caps.dump());
    REQUIRE(identity);
    plan=plan_device_update(matrix({device_entry("aio"),device_entry("nvr")}),identity->device,
                            identity->updater_version,"release",LegacyDeviceProduct::Aio);
    REQUIRE(plan);
    REQUIRE(plan->target.product_id=="nvr");
    REQUIRE_FALSE(plan->migration_required);
    auto invalid=caps; invalid["device"]["release_profile"]="nvr-unqualified";
    REQUIRE_FALSE(parse_update_identity(legacy,invalid.dump()));
    invalid=caps; invalid["device"]["release_profile"]=nullptr;
    REQUIRE_FALSE(parse_update_identity(legacy,invalid.dump()));
    invalid=caps; invalid.erase("device");
    REQUIRE_FALSE(parse_update_identity(legacy,invalid.dump()));
    invalid=caps; invalid["updater_version"]="unknown";
    REQUIRE_FALSE(parse_update_identity(legacy,invalid.dump()));
}

TEST_CASE("DEV selection is explicit and API compatibility is distinct from application version", "[update][modern]") {
    auto dev=device_entry(); dev["environment"]="dev"; dev["download_base"]="/dev/v1.4.2/aio/aio-jetson-orin-nx-l4t36.4.3-aarch64-cp310/linux-arm64/";
    auto catalog=matrix({dev});
    auto identity=parse_device_descriptor(descriptor().dump(),R"({"version":"1.4.1"})"); REQUIRE(identity);
    REQUIRE_FALSE(plan_device_update(catalog,*identity,"1.2.0"));
    REQUIRE(plan_device_update(catalog,*identity,"1.2.0","dev"));
    identity->api_version=Version(2,0,0);
    auto incompatible=plan_device_update(catalog,*identity,"1.2.0","dev");
    REQUIRE_FALSE(incompatible);REQUIRE(incompatible.error().code==ErrorCode::VersionNotFound);
}

TEST_CASE("Modern downgrades and conflicting cohort source identities are rejected", "[update][modern]") {
    auto catalog=matrix({device_entry()});
    auto identity=parse_device_descriptor(descriptor().dump(),R"({"version":"1.4.3"})"); REQUIRE(identity);
    auto plan=plan_device_update(catalog,*identity,"1.2.0"); REQUIRE_FALSE(plan); REQUIRE(plan.error().code==ErrorCode::DowngradeRejected);
    auto conflicting=device_entry("nvr"); conflicting["source_commit"]=std::string(40,'f');
    Json data={{"versions",Json::array()},{"device_versions",Json::array({device_entry(),conflicting})}};
    REQUIRE_FALSE(parse_versions_json(data.dump()));
}

TEST_CASE("Legacy HTTP500 or reset is pending and a success receipt must match all package identity", "[update][modern]") {
    REQUIRE(migration_upload_pending({ErrorCode::HttpError,"intentional legacy exit75",500}));
    REQUIRE(migration_upload_pending({ErrorCode::NetworkUnreachable,"reset"}));
    REQUIRE_FALSE(migration_upload_pending({ErrorCode::HttpError,"rejected",400}));
    auto catalog=matrix({device_entry()}); const auto &entry=catalog.device_versions.front();
    Json value={{"status","succeeded"},{"archive_sha256",entry.files[1].sha256},{"version","1.4.2"},
                {"source_commit",entry.source_commit},{"product_id","aio"},{"profile",entry.profile},
                {"role","updater-migration"},{"kind","updater-migration"},{"outcome","succeeded"}};
    auto receipt=parse_operation_receipt(value.dump()); REQUIRE(receipt); REQUIRE(receipt_matches(*receipt,entry,entry.files[1]));
    receipt->product_id="nvr"; REQUIRE_FALSE(receipt_matches(*receipt,entry,entry.files[1]));
    REQUIRE_FALSE(parse_operation_receipt(R"({"status":"succeeded"})"));
    auto log=Json{{"id",entry.files[1].sha256},{"status","failed"},{"error","fingerprint mismatch"}}.dump()+"\n";
    REQUIRE(migration_failure_log(log,entry.files[1].sha256)=="fingerprint mismatch");
    const auto no_error=Json{{"id",entry.files[1].sha256},{"status","failed"}}.dump();
    REQUIRE(migration_failure_log(no_error,entry.files[1].sha256)=="Updater migration failed; inspect recovery diagnostics");
    REQUIRE_FALSE(migration_failure_log(log,entry.files[0].sha256));
}

TEST_CASE("Cloud requires explicit HTTPS with no userinfo query or downgrade URL", "[update][modern]") {
    REQUIRE(secure_cloud_url("https://dl-thorvision-firmware.kontex.io"));
    for(auto url:{"http://dl-thorvision-firmware.kontex.io","dl-thorvision-firmware.kontex.io","https://user@evil.invalid","https://host.invalid/?redirect=http://other","file:///tmp/package"})
        REQUIRE_FALSE(secure_cloud_url(url));
}

TEST_CASE("Updater health refuses pending recovery or malformed readiness", "[update][modern]") {
    const Json healthy={{"updater_version","1.2.0"},{"protocol_version",1},
        {"compatibility",Json::array({"device_package_v1","pc_verified_package_v1"})},{"ready_for_updates",true},
        {"pending_application_transaction",false},{"recovery_error",nullptr}};
    REQUIRE(updater_ready(healthy.dump()));
    for(const auto &field:{"ready_for_updates","pending_application_transaction","recovery_error"}) {
        auto value=healthy;value.erase(field);REQUIRE_FALSE(updater_ready(value.dump()));
    }
    auto value=healthy;value["ready_for_updates"]=false;REQUIRE_FALSE(updater_ready(value.dump()));
    value=healthy;value["ready_for_updates"]="true";REQUIRE_FALSE(updater_ready(value.dump()));
    value=healthy;value["pending_application_transaction"]=true;REQUIRE_FALSE(updater_ready(value.dump()));
    value=healthy;value["pending_application_transaction"]="false";REQUIRE_FALSE(updater_ready(value.dump()));
    value=healthy;value["recovery_error"]="previous release failed health";REQUIRE_FALSE(updater_ready(value.dump()));
    value=healthy;value["recovery_error"]=false;REQUIRE_FALSE(updater_ready(value.dump()));
    value=healthy;value["recovery_error"]=Json::object();REQUIRE_FALSE(updater_ready(value.dump()));
    value=healthy;value["updater_version"]="1.0.9";REQUIRE_FALSE(updater_ready(value.dump()));
    value=healthy;value["protocol_version"]=2;REQUIRE_FALSE(updater_ready(value.dump()));
    value=healthy;value.erase("compatibility");REQUIRE_FALSE(updater_ready(value.dump()));
    value=healthy;value["compatibility"]=Json::array({"legacy_only"});REQUIRE_FALSE(updater_ready(value.dump()));
    value=healthy;value["updater_version"]="1.1.0";REQUIRE_FALSE(updater_ready(value.dump()));
    value=healthy;value["compatibility"]=Json::array({"device_package_v1","https_catalog_v1"});REQUIRE_FALSE(updater_ready(value.dump()));
    REQUIRE_FALSE(updater_ready("not JSON"));
}

TEST_CASE("A migration receipt cannot satisfy an application even with identical cohort identity", "[update][modern]") {
    auto catalog=matrix({device_entry()});const auto &entry=catalog.device_versions.front();
    const auto &artifact=entry.files[0];
    Json value={{"status","succeeded"},{"archive_sha256",artifact.sha256},{"version","1.4.2"},
        {"source_commit",entry.source_commit},{"product_id","aio"},{"profile",entry.profile},
        {"role","updater-migration"},{"kind","updater-migration"},{"outcome","succeeded"}};
    auto receipt=parse_operation_receipt(value.dump());REQUIRE(receipt);
    REQUIRE_FALSE(receipt_matches(*receipt,entry,artifact));
    value["role"]="application";value["kind"]="install";
    receipt=parse_operation_receipt(value.dump());REQUIRE(receipt);REQUIRE(receipt_matches(*receipt,entry,artifact));
    REQUIRE(receipt->kind=="install");REQUIRE(receipt->outcome=="succeeded");
    value["kind"]="rollback";receipt=parse_operation_receipt(value.dump());REQUIRE(receipt);
    REQUIRE_FALSE(receipt_matches(*receipt,entry,artifact));
    value["kind"]="updater-migration";REQUIRE_FALSE(parse_operation_receipt(value.dump()));
    value["kind"]="install";value["status"]="unrecognized";REQUIRE_FALSE(parse_operation_receipt(value.dump()));
    value["status"]="succeeded";value["role"]="unrecognized";REQUIRE_FALSE(parse_operation_receipt(value.dump()));
    value.erase("role");REQUIRE_FALSE(parse_operation_receipt(value.dump()));
}

TEST_CASE("Application health follows required capabilities without skipping AIO or advertised NVR health", "[update][modern]") {
    const Json healthy={{"protocol",1},{"sandbox",{{"bwrap",true},{"systemd_scope",true},
        {"memory_enforcement","cgroup"},{"problems",Json::array()}}}};
    auto catalog=matrix({device_entry("aio"),device_entry("nvr")});
    auto aio=parse_device_descriptor(descriptor("aio").dump(),R"({"version":"1.4.2"})");REQUIRE(aio);
    auto nvr=parse_device_descriptor(descriptor("nvr").dump(),R"({"version":"1.4.2"})");REQUIRE(nvr);
    const auto &aio_target=catalog.device_versions[0];const auto &nvr_target=catalog.device_versions[1];
    REQUIRE(application_requires_framelab(*aio,aio_target));
    REQUIRE_FALSE(application_health_ready(*aio,aio_target));
    REQUIRE(application_health_ready(*aio,aio_target,healthy.dump()));
    const auto healthy_aio=*aio;
    auto bad_api=healthy_aio;bad_api.api_version.reset();
    REQUIRE_FALSE(application_health_ready(bad_api,aio_target,healthy.dump()));
    bad_api=healthy_aio;bad_api.api_version=Version(0,2,0);
    REQUIRE_FALSE(application_health_ready(bad_api,aio_target,healthy.dump()));
    auto broken=healthy;broken["sandbox"]["problems"]=Json::array({"isolation unavailable"});
    REQUIRE_FALSE(application_health_ready(*aio,aio_target,broken.dump()));
    for(const auto protocol:{Json(2),Json(1.0),Json(4294967297ULL)}) {
        auto malformed_protocol=healthy;malformed_protocol["protocol"]=protocol;
        REQUIRE_FALSE(application_health_ready(*aio,aio_target,malformed_protocol.dump()));
    }
    REQUIRE_FALSE(application_health_ready(*aio,aio_target,"not JSON"));
    aio->framelab_supported=false;REQUIRE(application_requires_framelab(*aio,aio_target));
    REQUIRE_FALSE(application_health_ready(*aio,aio_target,healthy.dump()));
    REQUIRE(application_requires_framelab(*nvr,nvr_target));
    REQUIRE_FALSE(application_health_ready(*nvr,nvr_target));
    REQUIRE_FALSE(application_health_ready(*nvr,nvr_target,broken.dump()));
    REQUIRE(application_health_ready(*nvr,nvr_target,healthy.dump()));
    nvr->framelab_ready=false;REQUIRE_FALSE(application_health_ready(*nvr,nvr_target,healthy.dump()));
    nvr->framelab_supported=false;REQUIRE_FALSE(application_requires_framelab(*nvr,nvr_target));
    REQUIRE(application_health_ready(*nvr,nvr_target));
    bad_api=*nvr;bad_api.api_version.reset();REQUIRE_FALSE(application_health_ready(bad_api,nvr_target));
    bad_api=*nvr;bad_api.api_version=Version(0,2,0);REQUIRE_FALSE(application_health_ready(bad_api,nvr_target));
    nvr->build_version=Version(1,4,1);REQUIRE_FALSE(application_health_ready(*nvr,nvr_target));
    nvr->build_version=nvr_target.version;nvr->profile="wrong";
    REQUIRE_FALSE(application_health_ready(*nvr,nvr_target));
    nvr->profile=nvr_target.profile;nvr->product_id="aio";
    REQUIRE_FALSE(application_health_ready(*nvr,nvr_target));
}

TEST_CASE("Legacy labels do not become modern product identity and modern conflicts still refuse", "[update][modern]") {
    auto legacy=parse_device_descriptor(R"({"version":"0.1.3","features":["usb_topology_v1"],"device_type":"Raspberry Pi"})",
        R"({"message":"Build version","version":"0.1.3"})");
    REQUIRE(legacy);REQUIRE_FALSE(legacy->modern);REQUIRE(legacy->product_id.empty());REQUIRE(legacy->profile.empty());
    REQUIRE(legacy->api_version==Version(0,1,3));REQUIRE(legacy->build_version==Version(0,1,3));
    auto catalog=matrix({device_entry("aio"),device_entry("nvr")});
    REQUIRE_FALSE(plan_device_update(catalog,*legacy,"1.2.0"));
    auto modern=descriptor("aio");modern["device_type"]="nvr";
    REQUIRE_FALSE(parse_device_descriptor(modern.dump(),R"({"version":"1.4.2"})"));
}

TEST_CASE("Offline client refuses previously published online-only package minima", "[update][modern]") {
    auto old=device_entry();old["files"][0]["minimum_updater"]="1.1.0";
    auto catalog=matrix({old});
    auto legacy=parse_device_descriptor(R"({"version":"0.1.0"})",R"({"version":"0.0.32"})");REQUIRE(legacy);
    auto result=plan_device_update(catalog,*legacy,"","release",true);
    REQUIRE_FALSE(result);REQUIRE(result.error().code==ErrorCode::VersionNotFound);
}

TEST_CASE("Online-only modern updater is refused before attempting legacy migration", "[update][modern]") {
    auto catalog=matrix({device_entry()});
    auto device=parse_device_descriptor(descriptor().dump(),R"({"version":"1.4.1"})");REQUIRE(device);
    auto plan=plan_device_update(catalog,*device,"1.1.0");
    REQUIRE_FALSE(plan);REQUIRE(plan.error().code==ErrorCode::VersionNotFound);
}

TEST_CASE("Legacy application can resume after successful updater migration", "[update][modern]") {
    auto catalog=matrix({device_entry()});
    auto device=parse_device_descriptor(R"({"version":"0.1.0"})",R"({"version":"0.0.32"})");REQUIRE(device);
    auto plan=plan_device_update(catalog,*device,"1.2.0","release",true);REQUIRE(plan);
    REQUIRE(plan->update_available);REQUIRE_FALSE(plan->migration_required);
    REQUIRE_FALSE(plan->legacy_aio_confirmation);
}

TEST_CASE("Definitive legacy setup failures cannot enter migration receipt polling", "[update][modern]") {
    const auto error=[](std::string rc) { return Error{ErrorCode::HttpError,
        "http://127.0.0.1/transfer/local returned HTTP 500: {\"detail\":\"setup.sh from package root failed. Status: failed, RC: "+rc+"\"}",500}; };
    REQUIRE(legacy_setup_return_code(error("1").message)==1);
    REQUIRE(legacy_setup_return_code("Script setup.sh failed with return code 75")==75);
    REQUIRE_FALSE(legacy_setup_return_code("Script relocate_server.sh failed with return code 1"));
    REQUIRE_FALSE(legacy_setup_return_code("Callback output RC: 1"));
    REQUIRE_FALSE(legacy_setup_return_code("Script setup.sh failed with return code 99999999999999999"));
    REQUIRE_FALSE(legacy_setup_return_code(error("1garbage").message));
    REQUIRE_FALSE(migration_upload_pending(error("1")));
    REQUIRE(migration_upload_pending(error("75")));
    for(auto code:{"1","-9","0"}) {
        auto result=migration_upload_result(std::unexpected(error(code)));
        REQUIRE_FALSE(result);REQUIRE(result.error().code==ErrorCode::DeviceUpdateFailed);
        REQUIRE(result.error().message.find("exit code "+std::string(code))!=std::string::npos);
    }
    REQUIRE(migration_upload_result(std::unexpected(error("75"))));
    const Error reset{ErrorCode::NetworkUnreachable,"connection reset"};
    REQUIRE(migration_upload_result(std::unexpected(reset)));
    auto failed_stream=migration_upload_result(std::unexpected(reset),1);
    REQUIRE_FALSE(failed_stream);REQUIRE(failed_stream.error().code==ErrorCode::DeviceUpdateFailed);
    REQUIRE(migration_upload_result(std::unexpected(reset),75));
    REQUIRE_FALSE(migration_upload_result(std::unexpected(error("1")),75));
    REQUIRE_FALSE(migration_upload_result(std::unexpected(error("75")),1));
    REQUIRE(migration_upload_result(std::unexpected(Error{ErrorCode::HttpError,"ambiguous failure",500})));
    REQUIRE_FALSE(migration_upload_result(std::unexpected(Error{ErrorCode::HttpError,"rejected",400}),75));
}
