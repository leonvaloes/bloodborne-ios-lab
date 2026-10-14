#include "game_asset_probe.h"
#include "elf_parser.h"
#include "vfs.h"
#include <exception>
#include <vector>

namespace bb {

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
