#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bb {

struct ManifestFunction {
    std::string guest_rva;
    std::size_t size;
    std::string entry_symbol;
    bool indirect_targets_resolved;
    std::string status;
};

struct TranslationManifest {
    unsigned schema_version = 1;
    std::string guest_arch = "x86_64";
    std::string host_arch = "arm64-apple-ios";
    std::string binary_sha256;
    std::vector<ManifestFunction> functions;

    std::string to_json() const;
    bool validate() const;
};

} // namespace bb
