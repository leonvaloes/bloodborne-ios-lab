#include "check.h"
#include "manifest.h"

int main() { return run([] {
    using namespace bb;
    TranslationManifest m;
    m.schema_version = 1;
    m.guest_arch = "x86_64";
    m.host_arch = "arm64-apple-ios";
    m.binary_sha256 = "0123456789abcdef";
    m.functions.push_back({"0x00001000", 64, "bb_func_1000", true, "aot-compiled"});
    m.functions.push_back({"0x00002000", 128, "bb_func_2000", false, "stub"});

    CHECK(m.validate());
    auto json = m.to_json();
    CHECK(json.find("schema_version") != std::string::npos);
    CHECK(json.find("guest_arch") != std::string::npos);
    CHECK(json.find("x86_64") != std::string::npos);
    CHECK(json.find("arm64-apple-ios") != std::string::npos);
    CHECK(json.find("bb_func_1000") != std::string::npos);

    // Invalid schema version
    auto m_bad = m;
    m_bad.schema_version = 2;
    CHECK(!m_bad.validate());

    // Invalid arch
    m_bad = m;
    m_bad.guest_arch = "arm32";
    CHECK(!m_bad.validate());

    // Invalid function size
    m_bad = m;
    m_bad.functions[0].size = 0;
    CHECK(!m_bad.validate());

    std::cout << "manifest: structured JSON serialization and schema validation passed\n";
}); }
