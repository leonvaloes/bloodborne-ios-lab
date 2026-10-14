#ifndef BB_VFS_H
#define BB_VFS_H

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <memory>

namespace bb {

enum class VfsError {
    ok = 0,
    not_found,
    access_denied,
    invalid_path,
    traversal_detected,
    io_error,
    not_mounted
};

struct VfsStat {
    std::uint64_t size{0};
    bool is_directory{false};
    bool is_readonly{false};
};

class Bhd5Reader;
class Bnd4Reader;

class VirtualFileSystem {
public:
    VirtualFileSystem();

    // Monta um caminho host em um ponto de montagem virtual (ex: "/app0" -> "/var/.../app0")
    bool mount(const std::string &mount_point, const std::string &host_dir, bool read_only = true);
    bool unmount(const std::string &mount_point);
    bool is_mounted(const std::string &mount_point) const;

    // Montagem direta de pacotes FromSoftware BHD5/BDT e BND4 no VFS
    bool mount_bhd5(const std::string &mount_point, std::shared_ptr<Bhd5Reader> reader);
    bool mount_bnd4(const std::string &mount_point, std::shared_ptr<Bnd4Reader> reader);

    // Normaliza e resolve um caminho virtual para caminho f?sico host
    VfsError resolve(const std::string &virtual_path, std::string &out_host_path, bool &out_readonly) const;

    // Opera??es de arquivo
    bool exists(const std::string &virtual_path) const;
    VfsError stat(const std::string &virtual_path, VfsStat &out_stat) const;
    VfsError read_file(const std::string &virtual_path, std::vector<std::uint8_t> &out_data) const;
    VfsError write_file(const std::string &virtual_path, const void *data, std::size_t size, bool atomic = true);
    VfsError remove_file(const std::string &virtual_path);

    // Utilit?rios de seguran?a e integridade
    static std::string normalize_path(const std::string &path);
    static bool contains_traversal(const std::string &normalized_path);
    static std::uint32_t compute_crc32(const void *data, std::size_t size);

private:
    struct MountPoint {
        std::string host_path;
        bool read_only{true};
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, MountPoint> mounts_;
    std::unordered_map<std::string, std::shared_ptr<Bhd5Reader>> bhd5_mounts_;
    std::unordered_map<std::string, std::shared_ptr<Bnd4Reader>> bnd4_mounts_;
};

} // namespace bb

#endif // BB_VFS_H
