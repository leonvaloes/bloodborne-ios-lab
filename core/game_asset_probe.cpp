#include "game_asset_probe.h"
#include "elf_parser.h"
#include "flver_parser.h"
#include <cstring>
#include "vfs.h"
#include <exception>
#include <vector>

namespace bb {

DcxProbeResult probe_dcx_in_documents(const std::string& documents_dir) {
    DcxProbeResult result;
    VirtualFileSystem vfs;
    if (!vfs.mount("/app0", documents_dir, true)) {
        result.status = "mount_failed";
        return result;
    }
    constexpr const char* path = "/app0/m29_00_00_00_408010.flver.dcx";
    VfsStat info;
    const auto stat_error = vfs.stat(path, info);
    if (stat_error != VfsError::ok) {
        result.status = stat_error == VfsError::not_found ? "not_found" : "stat_error";
        return result;
    }
    result.size_bytes = info.size;
    if (info.is_directory || info.size < 76 || info.size > 1024 * 1024) {
        result.status = "invalid_size";
        return result;
    }
    std::vector<std::uint8_t> bytes;
    if (vfs.read_file(path, bytes) != VfsError::ok) {
        result.status = "read_error";
        return result;
    }
    result.size_bytes = bytes.size();
    if (bytes.size() < 76 || bytes.size() > 1024 * 1024 ||
        !FlverParser::is_dcx_data(bytes.data(), bytes.size())) {
        result.status = "invalid_dcx";
        return result;
    }
    result.crc32 = VirtualFileSystem::compute_crc32(bytes.data(), bytes.size());
    // The decompressor uses the DCS declared size for allocation: cap it first.
    bool bounded = false;
    for (std::size_t i = 0; i + 12 <= bytes.size(); ++i) {
        if (std::memcmp(bytes.data() + i, "DCS\0", 4) == 0) {
            const std::uint32_t length = (std::uint32_t(bytes[i + 4]) << 24) |
                (std::uint32_t(bytes[i + 5]) << 16) |
                (std::uint32_t(bytes[i + 6]) << 8) | bytes[i + 7];
            bounded = length >= 8 && length <= 8 * 1024 * 1024;
            break;
        }
    }
    if (!bounded) {
        result.status = "invalid_decoded_size";
        return result;
    }
    std::vector<std::uint8_t> decoded;
    std::string error;
    if (!FlverParser::decompress_dcx(bytes.data(), bytes.size(), decoded, error)) {
        result.status = "decompress_failed";
        return result;
    }
    result.decoded_bytes = decoded.size();
    if (!FlverParser::is_flver_data(decoded.data(), decoded.size())) {
        result.status = "invalid_flver_magic";
        return result;
    }
    result.decoded_crc32 = VirtualFileSystem::compute_crc32(decoded.data(), decoded.size());
    result.valid = true;
    result.status = "ok";
    return result;
}

SfoProbeResult probe_sfo_in_documents(const std::string& documents_dir) {
    SfoProbeResult result;
    VirtualFileSystem vfs;
    if (!vfs.mount("/app0", documents_dir, true)) {
        result.status = "mount_failed";
        return result;
    }
    VfsStat info;
    const auto stat_error = vfs.stat("/app0/param.sfo", info);
    if (stat_error != VfsError::ok) {
        result.status = stat_error == VfsError::not_found ? "not_found" : "stat_error";
        return result;
    }
    result.size_bytes = info.size;
    if (info.is_directory || info.size < 20 || info.size > 1024 * 1024) {
        result.status = "invalid_size";
        return result;
    }
    std::vector<std::uint8_t> bytes;
    if (vfs.read_file("/app0/param.sfo", bytes) != VfsError::ok) {
        result.status = "read_error";
        return result;
    }
    result.size_bytes = bytes.size();
    if (bytes.size() < 20 || bytes.size() > 1024 * 1024) {
        result.status = "invalid_size";
        return result;
    }
    result.crc32 = VirtualFileSystem::compute_crc32(bytes.data(), bytes.size());
    try {
        const auto metadata = SafeGameParser::parse_sfo(bytes.data(), bytes.size());
        if (metadata.title_id.empty()) {
            result.status = "missing_title_id";
            return result;
        }
        result.title = metadata.title;
        result.title_id = metadata.title_id;
        result.app_ver = metadata.app_ver;
        result.valid = true;
        result.status = "ok";
    } catch (const std::exception&) {
        result.status = "invalid_sfo";
    }
    return result;
}

} // namespace bb
