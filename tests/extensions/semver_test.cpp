#include "test_util.hpp"

#include "extensions/semver.hpp"

using namespace edgedesk::extensions;

namespace {

SemVer parse_or_fail(const char* text) {
    SemVer version;
    std::string error;
    const bool ok = SemVer::parse(text, version, error);
    CHECK(ok);
    if (!ok) std::fprintf(stderr, "parse failed for %s: %s\n", text, error.c_str());
    return version;
}

void test_valid() {
    CHECK_STR_EQ(parse_or_fail("1.2.3").to_string(), "1.2.3");
    CHECK_STR_EQ(parse_or_fail("0.0.0").to_string(), "0.0.0");
    CHECK_STR_EQ(parse_or_fail("1.0.0-alpha.1").to_string(), "1.0.0-alpha.1");
    CHECK_STR_EQ(parse_or_fail("1.0.0+build.5").to_string(), "1.0.0+build.5");
    CHECK_STR_EQ(parse_or_fail("10.20.30-rc.1+sha.abc").to_string(), "10.20.30-rc.1+sha.abc");
}

void test_invalid() {
    const char* invalid[] = {
        "", "1", "1.2", "1.2.3.4", "v1.2.3", "01.2.3", "1.02.3", "1.2.03",
        "1.2.3-", "1.2.3+", "1.2.3-alpha..1", "1.2.3-alpha.01", "1.2.3-ü",
        "1.2.3+build..1", " 1.2.3", "1.2.3 ", "4294967296.0.0",
    };
    for (const char* text : invalid) {
        SemVer version;
        std::string error;
        CHECK(!SemVer::parse(text, version, error));
        CHECK(!error.empty());
    }
}

void test_order() {
    CHECK(compare(parse_or_fail("1.0.0"), parse_or_fail("2.0.0")) < 0);
    CHECK(compare(parse_or_fail("1.0.0"), parse_or_fail("1.1.0")) < 0);
    CHECK(compare(parse_or_fail("1.0.0"), parse_or_fail("1.0.1")) < 0);
    CHECK(compare(parse_or_fail("1.0.0-alpha"), parse_or_fail("1.0.0")) < 0);
    CHECK(compare(parse_or_fail("1.0.0-alpha"), parse_or_fail("1.0.0-alpha.1")) < 0);
    CHECK(compare(parse_or_fail("1.0.0-alpha.1"), parse_or_fail("1.0.0-alpha.beta")) < 0);
    CHECK(compare(parse_or_fail("1.0.0-beta.2"), parse_or_fail("1.0.0-beta.11")) < 0);
    CHECK(compare(parse_or_fail("1.0.0+build.1"), parse_or_fail("1.0.0+build.2")) == 0);
    CHECK(compare(parse_or_fail("1.0.0-alpha.1"), parse_or_fail("1.0.0-alpha.1")) == 0);
}

void test_host_range() {
    const auto host = parse_or_fail("1.4.2");
    CHECK(host_api_compatible(host, parse_or_fail("1.0.0"), parse_or_fail("1.9.9")));
    CHECK(host_api_compatible(host, parse_or_fail("1.4.2"), parse_or_fail("1.4.2")));
    CHECK(!host_api_compatible(host, parse_or_fail("1.4.3"), parse_or_fail("2.0.0")));
    CHECK(!host_api_compatible(host, parse_or_fail("1.0.0"), parse_or_fail("1.4.1")));
    CHECK(!host_api_compatible(host, parse_or_fail("2.0.0"), parse_or_fail("1.0.0")));
}

} // namespace

int main() {
    test_valid();
    test_invalid();
    test_order();
    test_host_range();
    return edgedesk::test::finish("semver_test");
}
