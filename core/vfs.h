#ifndef BB_VFS_H
#define BB_VFS_H

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <memory>
#include <functional>
#include <filesystem>

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
    bool mount_bhd5(const std::string &mount_point, std::shared_ptr<Bhd5Reader> reader, int priority = 0);
    bool mount_bnd4(const std::string &mount_point, std::shared_ptr<Bnd4Reader> reader);

    // Suporte a arquivos sintéticos em memória (stubs para testes, shaders e GUI ausentes)
    bool set_synthetic_file(const std::string &virtual_path, std::vector<std::uint8_t> data);
    bool has_synthetic_file(const std::string &virtual_path) const;
    bool has_archive_file(const std::string &virtual_path) const;
    bool has_archive_or_synthetic_file(const std::string &virtual_path) const;
    bool remove_synthetic_file(const std::string &virtual_path);
    void register_shader_gui_stubs();

    // Normaliza e resolve um caminho virtual para caminho f?sico host
    VfsError resolve(const std::string &virtual_path, std::string &out_host_path, bool &out_readonly) const;

    // Opera??es de arquivo
    bool exists(const std::string &virtual_path) const;
    VfsError stat(const std::string &virtual_path, VfsStat &out_stat) const;
    VfsError read_file(const std::string &virtual_path, std::vector<std::uint8_t> &out_data) const;
    // Bounded random-access read of a local file, BHD5/BDT entry, or BND4 entry; never allocates the file's size.
    // Returns ok with out_bytes_read < bytes_to_read at EOF (including zero beyond EOF).
    // dest may be null only for a zero-byte request.
    VfsError read_at(const std::string &virtual_path, std::uint64_t offset,
                     void *dest, std::size_t bytes_to_read, std::size_t &out_bytes_read) const;
    VfsError write_file(const std::string &virtual_path, const void *data, std::size_t size, bool atomic = true);
    VfsError remove_file(const std::string &virtual_path);

    // Test hook for simulating rename failures during atomic write
    using RenameHook = std::function<std::error_code(const std::filesystem::path &from, const std::filesystem::path &to)>;
    void set_rename_hook_for_testing(RenameHook hook) {
        std::lock_guard<std::mutex> lock(mutex_);
        rename_hook_ = std::move(hook);
    }

    // Utilit?rios de seguran?a e integridade
    static std::string normalize_path(const std::string &path);
    static bool contains_traversal(const std::string &normalized_path);
    static std::uint32_t compute_crc32(const void *data, std::size_t size);

private:
    struct MountPoint {
        std::string host_path;
        bool read_only{true};
    };

    struct Bhd5Mount {
        std::string mount_point;
        std::shared_ptr<Bhd5Reader> reader;
        int priority{0};
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, MountPoint> mounts_;
    std::vector<Bhd5Mount> bhd5_mounts_;
    std::unordered_map<std::string, std::shared_ptr<Bnd4Reader>> bnd4_mounts_;
    std::unordered_map<std::string, std::vector<std::uint8_t>> synthetic_files_;
    RenameHook rename_hook_;
};

} // namespace bb

#endif // BB_VFS_H
