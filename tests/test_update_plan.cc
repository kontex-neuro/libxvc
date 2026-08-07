// Tier 1 (ADR 0009): the ADR 0007 policy rule table. Pure logic -- no network, no device,
// no filesystem.
//
// This is where most of the update API's risk lives. The rules encode a COMPATIBILITY-based
// policy, not a latest-based one, and several of them are counter-intuitive on first read:
// downgrades into the required series are normal, a device ahead of the series is just as
// blocking as one behind it, and a deprecated release triggers a patch-level downgrade.

#include <catch2/catch_test_macros.hpp>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "../xdaqvc/update/plan.h"

using namespace xvc::update;

namespace
{

// Builds a matrix directly rather than parsing JSON, so a policy failure cannot be masked by
// a parser failure. Platform defaults to the real device platform.
VersionEntry make_entry(
    Version version, std::string status = "stable", std::string environment = "release",
    std::string platform = std::string{DEVICE_PLATFORM}
)
{
    VersionEntry entry;
    entry.version = version;
    entry.platform = std::move(platform);
    entry.environment = std::move(environment);
    entry.status = std::move(status);
    entry.download_base = std::format("/dist/v{}/linux-arm64/", version.to_string());
    entry.files.push_back(Artifact{
        .name = std::format("ThorVisionServer-{}-linux-arm64.tar.xz", version.to_string()),
        .size = 46070024,
        .sha256 = std::string(64, 'a')
    });
    return entry;
}

VersionMatrix make_matrix(std::vector<VersionEntry> entries)
{
    VersionMatrix matrix;
    matrix.versions = std::move(entries);
    return matrix;
}

// The series the application declares it supports.
constexpr VersionSeries REQUIRED{0, 1};

}  // namespace

TEST_CASE("Rule 3: target is the highest patch in the required series", "[update][plan]")
{
    auto matrix = make_matrix({
        make_entry(Version{0, 1, 0}),
        make_entry(Version{0, 1, 2}),
        make_entry(Version{0, 1, 1}),
    });

    auto plan = plan_update(Version{0, 1, 0}, matrix, REQUIRED);
    REQUIRE(plan.has_value());
    CHECK(plan->target.version == Version{0, 1, 2});
}

TEST_CASE("Rule 2: no candidates in the required series is a loud failure", "[update][plan]")
{
    // The app requires 0.2.x but the cloud publishes only 0.1.x. This is a deployment error
    // and must NOT be silently reported as "up to date".
    auto matrix = make_matrix({make_entry(Version{0, 1, 2})});

    auto plan = plan_update(Version{0, 1, 2}, matrix, VersionSeries{0, 2});
    REQUIRE_FALSE(plan.has_value());
    CHECK(plan.error().code == ErrorCode::VersionNotFound);
}

TEST_CASE("Rule 1: candidates exclude dev, deprecated, and other platforms", "[update][plan]")
{
    SECTION("environment != release is not a candidate")
    {
        auto matrix = make_matrix({
            make_entry(Version{0, 1, 1}),
            make_entry(Version{0, 1, 5}, "stable", "dev"),
        });
        auto plan = plan_update(Version{0, 1, 0}, matrix, REQUIRED);
        REQUIRE(plan.has_value());
        CHECK(plan->target.version == Version{0, 1, 1});  // the dev 0.1.5 is ignored
    }

    SECTION("status != stable is not a candidate")
    {
        auto matrix = make_matrix({
            make_entry(Version{0, 1, 1}),
            make_entry(Version{0, 1, 5}, "deprecated"),
        });
        auto plan = plan_update(Version{0, 1, 0}, matrix, REQUIRED);
        REQUIRE(plan.has_value());
        CHECK(plan->target.version == Version{0, 1, 1});
    }

    SECTION("a different platform is not a candidate")
    {
        auto matrix = make_matrix({
            make_entry(Version{0, 1, 1}),
            make_entry(Version{0, 1, 9}, "stable", "release", "linux-x86_64"),
        });
        auto plan = plan_update(Version{0, 1, 0}, matrix, REQUIRED);
        REQUIRE(plan.has_value());
        CHECK(plan->target.version == Version{0, 1, 1});
    }
}

TEST_CASE("Rule 4: device current within the series yields None", "[update][plan]")
{
    auto matrix = make_matrix({
        make_entry(Version{0, 1, 1}),
        make_entry(Version{0, 1, 2}),
    });

    SECTION("device exactly at target")
    {
        auto plan = plan_update(Version{0, 1, 2}, matrix, REQUIRED);
        REQUIRE(plan.has_value());
        CHECK(plan->action == UpdateAction::None);
        CHECK_FALSE(plan->required);
    }

    SECTION("device ahead on patch is left alone")
    {
        // Not expected in practice, but it must not trigger a downgrade: the device is
        // already compatible, which is the only property that matters.
        auto plan = plan_update(Version{0, 1, 7}, matrix, REQUIRED);
        REQUIRE(plan.has_value());
        CHECK(plan->action == UpdateAction::None);
        CHECK_FALSE(plan->required);
    }
}

TEST_CASE("Rule 4 exception: a deprecated device version triggers recall", "[update][plan]")
{
    // The recall path. The device sits on 0.1.5, which was later deprecated; the newest
    // stable patch is 0.1.2. Policy must move the device DOWN rather than leave it stranded
    // on a known-bad build. Only the matrix knows 0.1.5's status -- the device version alone
    // does not reveal it.
    auto deprecated = make_entry(Version{0, 1, 5}, "deprecated");
    deprecated.deprecation_reason = "corrupts the SPI log on rotation";

    auto matrix = make_matrix({
        make_entry(Version{0, 1, 1}),
        make_entry(Version{0, 1, 2}),
        deprecated,
    });

    auto plan = plan_update(Version{0, 1, 5}, matrix, REQUIRED);
    REQUIRE(plan.has_value());
    CHECK(plan->action == UpdateAction::Update);
    CHECK(plan->target.version == Version{0, 1, 2});  // numerically lower, deliberately
    // Still inside the supported series, so this is not the blocking node-E case.
    CHECK_FALSE(plan->required);
    CHECK(plan->reason.find("corrupts the SPI log") != std::string::npos);
}

TEST_CASE("Rule 4 exception does not fire for a stable device version", "[update][plan]")
{
    // Guards against the recall check being over-eager: a device ahead on patch whose exact
    // version is in the matrix as *stable* must still yield None.
    auto matrix = make_matrix({
        make_entry(Version{0, 1, 2}),
        make_entry(Version{0, 1, 5}),
    });

    auto plan = plan_update(Version{0, 1, 5}, matrix, REQUIRED);
    REQUIRE(plan.has_value());
    CHECK(plan->action == UpdateAction::None);
}

TEST_CASE("Rule 5: in-series but behind on patch is optional", "[update][plan]")
{
    auto matrix = make_matrix({
        make_entry(Version{0, 1, 0}),
        make_entry(Version{0, 1, 2}),
    });

    auto plan = plan_update(Version{0, 1, 0}, matrix, REQUIRED);
    REQUIRE(plan.has_value());
    CHECK(plan->action == UpdateAction::Update);
    CHECK(plan->target.version == Version{0, 1, 2});
    // The device is already compatible, so the app decides whether to surface this at all.
    CHECK_FALSE(plan->required);
}

TEST_CASE("Rule 6: outside the series is a REQUIRED update, either direction", "[update][plan]")
{
    auto matrix = make_matrix({
        make_entry(Version{0, 1, 1}),
        make_entry(Version{0, 1, 2}),
    });

    SECTION("device behind the series -- upgrade, blocking")
    {
        auto plan = plan_update(Version{0, 0, 9}, matrix, REQUIRED);
        REQUIRE(plan.has_value());
        CHECK(plan->action == UpdateAction::Update);
        CHECK(plan->target.version == Version{0, 1, 2});
        CHECK(plan->required);
    }

    SECTION("device ahead of the series -- DOWNGRADE, still blocking")
    {
        // A latest-based policy gets this exactly wrong. The device is on 0.2.0 while the
        // app supports 0.1.x, so it must move down to 0.1.2 and the app cannot run until
        // it does. Direction is irrelevant; compatibility is the gate.
        auto plan = plan_update(Version{0, 2, 0}, matrix, REQUIRED);
        REQUIRE(plan.has_value());
        CHECK(plan->action == UpdateAction::Update);
        CHECK(plan->target.version == Version{0, 1, 2});
        CHECK(plan->required);
    }

    SECTION("major version mismatch is blocking")
    {
        auto plan = plan_update(Version{1, 1, 0}, matrix, REQUIRED);
        REQUIRE(plan.has_value());
        CHECK(plan->action == UpdateAction::Update);
        CHECK(plan->required);
    }
}

TEST_CASE("latest_release drives no decision", "[update][plan]")
{
    // ADR 0007: latest_release is part of the wire format and is parsed, but policy must not
    // read it. Here it advertises 0.9.9 -- which is not even in versions[] -- and the plan
    // must still resolve within the required series.
    auto matrix = make_matrix({
        make_entry(Version{0, 1, 1}),
        make_entry(Version{0, 1, 2}),
    });
    matrix.latest_release.emplace(std::string{DEVICE_PLATFORM}, Version{0, 9, 9});

    auto plan = plan_update(Version{0, 1, 1}, matrix, REQUIRED);
    REQUIRE(plan.has_value());
    CHECK(plan->target.version == Version{0, 1, 2});
}

TEST_CASE("plan_forced_update targets an exact version", "[update][plan]")
{
    auto matrix = make_matrix({
        make_entry(Version{0, 1, 1}),
        make_entry(Version{0, 1, 2}),
        make_entry(Version{0, 2, 0}),
    });

    SECTION("forward across series is allowed without a flag")
    {
        auto plan = plan_forced_update(Version{0, 1, 1}, matrix, Version{0, 2, 0});
        REQUIRE(plan.has_value());
        CHECK(plan->action == UpdateAction::Update);
        CHECK(plan->target.version == Version{0, 2, 0});
        CHECK_FALSE(plan->required);  // an operator override, not a compatibility gate
    }

    SECTION("downgrade is rejected unless explicitly allowed")
    {
        auto rejected = plan_forced_update(Version{0, 2, 0}, matrix, Version{0, 1, 1});
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().code == ErrorCode::DowngradeRejected);

        auto allowed =
            plan_forced_update(Version{0, 2, 0}, matrix, Version{0, 1, 1}, /*allow_downgrade=*/true);
        REQUIRE(allowed.has_value());
        CHECK(allowed->target.version == Version{0, 1, 1});
    }

    SECTION("already on the forced target yields None")
    {
        auto plan = plan_forced_update(Version{0, 1, 2}, matrix, Version{0, 1, 2});
        REQUIRE(plan.has_value());
        CHECK(plan->action == UpdateAction::None);
    }

    SECTION("a missing target is VersionNotFound")
    {
        auto plan = plan_forced_update(Version{0, 1, 1}, matrix, Version{9, 9, 9});
        REQUIRE_FALSE(plan.has_value());
        CHECK(plan.error().code == ErrorCode::VersionNotFound);
    }
}

TEST_CASE("plan_forced_update gates deprecated targets", "[update][plan]")
{
    auto matrix = make_matrix({
        make_entry(Version{0, 1, 1}),
        make_entry(Version{0, 1, 2}, "deprecated"),
    });

    auto rejected = plan_forced_update(Version{0, 1, 1}, matrix, Version{0, 1, 2});
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == ErrorCode::ReleaseDeprecated);

    auto allowed = plan_forced_update(
        Version{0, 1, 1}, matrix, Version{0, 1, 2}, /*allow_downgrade=*/false,
        /*allow_deprecated=*/true
    );
    REQUIRE(allowed.has_value());
    CHECK(allowed->target.version == Version{0, 1, 2});
}
