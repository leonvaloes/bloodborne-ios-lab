#pragma once
#include <cstdint>
#include <string>

namespace bb {

struct SfoProbeResult {
    std::string status{"not_found"};
    bool valid{false};
    std::uint64_t size_bytes{0};
    std::uint32_t crc32{0};
    std::string title;
    std::string title_id;
    std::string app_ver;
};

struct DcxProbeResult {
    std::string status{"not_found"};
    bool valid{false};
    std::uint64_t size_bytes{0};
    std::uint64_t decoded_bytes{0};
    std::uint32_t crc32{0};
    std::uint32_t decoded_crc32{0};
};

DcxProbeResult probe_dcx_in_documents(const std::string& documents_dir);

// Reads only /app0/param.sfo from the supplied host Documents directory.
SfoProbeResult probe_sfo_in_documents(const std::string& documents_dir);

} // namespace bb
