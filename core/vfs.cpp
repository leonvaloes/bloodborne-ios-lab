#include "vfs.h"
#include "binder.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>

namespace bb {

VirtualFileSystem::VirtualFileSystem() = default;

bool VirtualFileSystem::mount_bhd5(const std::string &mount_point, std::shared_ptr<Bhd5Reader> reader) {
    if (mount_point.empty() || !reader) return false;
    std::string norm_mount = normalize_path(mount_point);
    std::lock_guard<std::mutex> lock(mutex_);
    bhd5_mounts_[norm_mount] = std::move(reader);
    return true;
}

bool VirtualFileSystem::mount_bnd4(const std::string &mount_point, std::shared_ptr<Bnd4Reader> reader) {
    if (mount_point.empty() || !reader) return false;
    std::string norm_mount = normalize_path(mount_point);
    std::lock_guard<std::mutex> lock(mutex_);
    bnd4_mounts_[norm_mount] = std::move(reader);
    return true;
}

std::string VirtualFileSystem::normalize_path(const std::string &path) {
    if (path.empty()) return "/";
    std::string norm;
    norm.reserve(path.size());

    // Converte barras invertidas para barras normais
    for (char c : path) {
        if (c == '\\') norm.push_back('/');
        else norm.push_back(c);
    }

    // Colapsa barras consecutivas
    std::string collapsed;
    collapsed.reserve(norm.size());
    bool last_was_slash = false;
    for (char c : norm) {
        if (c == '/') {
            if (!last_was_slash) {
                collapsed.push_back(c);
                last_was_slash = true;
            }
        } else {
            collapsed.push_back(c);
            last_was_slash = false;
        }
    }

    // Remove barra final a menos que seja raiz "/"
    if (collapsed.size() > 1 && collapsed.back() == '/') {
        collapsed.pop_back();
    }
    return collapsed;
}

bool VirtualFileSystem::contains_traversal(const std::string &normalized_path) {
    // Procura por ".." em componentes de caminho
    std::size_t pos = 0;
    while ((pos = normalized_path.find("..", pos)) != std::string::npos) {
        bool left_boundary = (pos == 0 || normalized_path[pos - 1] == '/');
        bool right_boundary = (pos + 2 == normalized_path.size() || normalized_path[pos + 2] == '/');
        if (left_boundary && right_boundary) {
            return true;
        }
        pos += 2;
    }
    return false;
}

std::uint32_t VirtualFileSystem::compute_crc32(const void *data, std::size_t size) {
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0xEDB88320u & (-(static_cast<std::int32_t>(crc & 1))));
        }
    }
    return ~crc;
}

bool VirtualFileSystem::mount(const std::string &mount_point, const std::string &host_dir, bool read_only) {
    if (mount_point.empty() || host_dir.empty()) return false;
    std::string norm_mount = normalize_path(mount_point);
    std::lock_guard<std::mutex> lock(mutex_);
    mounts_[norm_mount] = MountPoint{host_dir, read_only};
    return true;
}

bool VirtualFileSystem::unmount(const std::string &mount_point) {
    std::string norm_mount = normalize_path(mount_point);
    std::lock_guard<std::mutex> lock(mutex_);
    bool erased = mounts_.erase(norm_mount) > 0;
    erased = (bhd5_mounts_.erase(norm_mount) > 0) || erased;
    erased = (bnd4_mounts_.erase(norm_mount) > 0) || erased;
    return erased;
}

bool VirtualFileSystem::is_mounted(const std::string &mount_point) const {
    std::string norm_mount = normalize_path(mount_point);
    std::lock_guard<std::mutex> lock(mutex_);
    return mounts_.find(norm_mount) != mounts_.end() ||
           bhd5_mounts_.find(norm_mount) != bhd5_mounts_.end() ||
           bnd4_mounts_.find(norm_mount) != bnd4_mounts_.end();
}

VfsError VirtualFileSystem::resolve(const std::string &virtual_path, std::string &out_host_path, bool &out_readonly) const {
    std::string norm = normalize_path(virtual_path);
    if (contains_traversal(norm)) {
        return VfsError::traversal_detected;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    // Busca o ponto de montagem com maior prefix match
    std::string best_mount;
    MountPoint best_point{};
    for (const auto &pair : mounts_) {
        const std::string &m = pair.first;
        if (norm == m || (norm.rfind(m + "/", 0) == 0)) {
            if (m.size() > best_mount.size()) {
                best_mount = m;
                best_point = pair.second;
            }
        }
    }

    if (best_mount.empty()) {
        return VfsError::not_mounted;
    }

    std::string relative = norm.substr(best_mount.size());
    if (!relative.empty() && relative.front() == '/') {
        relative.erase(0, 1);
    }

    std::filesystem::path host(best_point.host_path);
    if (!relative.empty()) {
        host /= relative;
    }

    // Resolve existing symlinks (including parent directories of a new file) so a
    // Documents mount cannot be used to access files outside its host directory.
    std::error_code ec;
    const auto root = std::filesystem::weakly_canonical(best_point.host_path, ec);
    if (ec) return VfsError::io_error;
    const auto target = std::filesystem::weakly_canonical(host, ec);
    if (ec) return VfsError::io_error;
    auto root_part = root.begin();
    auto target_part = target.begin();
    for (; root_part != root.end(); ++root_part, ++target_part) {
        if (target_part == target.end() || *root_part != *target_part)
            return VfsError::access_denied;
    }

    out_host_path = host.string();
    out_readonly = best_point.read_only;
    return VfsError::ok;
}

bool VirtualFileSystem::exists(const std::string &virtual_path) const {
    std::string norm = normalize_path(virtual_path);
    if (contains_traversal(norm)) return false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &pair : bhd5_mounts_) {
            const std::string &m = pair.first;
            if (norm == m || norm.rfind(m + "/", 0) == 0) {
                std::string rel = norm.substr(m.size());
                if (!rel.empty() && rel.front() == '/') rel.erase(0, 1);
                if (pair.second && pair.second->has_file(rel)) return true;
            }
        }
        for (const auto &pair : bnd4_mounts_) {
            const std::string &m = pair.first;
            if (norm == m || norm.rfind(m + "/", 0) == 0) {
                std::string rel = norm.substr(m.size());
                if (!rel.empty() && rel.front() == '/') rel.erase(0, 1);
                if (pair.second && pair.second->has_file(rel)) return true;
            }
        }
    }

    std::string host_path;
    bool ro = false;
    if (resolve(virtual_path, host_path, ro) != VfsError::ok) return false;
    std::error_code ec;
    return std::filesystem::exists(host_path, ec);
}

VfsError VirtualFileSystem::stat(const std::string &virtual_path, VfsStat &out_stat) const {
    std::string norm = normalize_path(virtual_path);
    if (contains_traversal(norm)) return VfsError::traversal_detected;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &pair : bhd5_mounts_) {
            const std::string &m = pair.first;
            if (norm == m || norm.rfind(m + "/", 0) == 0) {
                std::string rel = norm.substr(m.size());
                if (!rel.empty() && rel.front() == '/') rel.erase(0, 1);
                if (pair.second) {
                    const auto *entry = pair.second->get_entry(rel);
                    if (entry) {
                        out_stat.size = entry->file_size;
                        out_stat.is_directory = false;
                        out_stat.is_readonly = true;
                        return VfsError::ok;
                    }
                }
            }
        }
        for (const auto &pair : bnd4_mounts_) {
            const std::string &m = pair.first;
            if (norm == m || norm.rfind(m + "/", 0) == 0) {
                std::string rel = norm.substr(m.size());
                if (!rel.empty() && rel.front() == '/') rel.erase(0, 1);
                if (pair.second) {
                    const auto *entry = pair.second->get_entry(rel);
                    if (entry) {
                        out_stat.size = entry->compressed_size;
                        out_stat.is_directory = false;
                        out_stat.is_readonly = true;
                        return VfsError::ok;
                    }
                }
            }
        }
    }

    std::string host_path;
    bool ro = false;
    VfsError err = resolve(virtual_path, host_path, ro);
    if (err != VfsError::ok) return err;

    std::error_code ec;
    const auto status = std::filesystem::status(host_path, ec);
    if (ec == std::errc::no_such_file_or_directory) return VfsError::not_found;
    if (ec) return VfsError::io_error;
    if (!std::filesystem::exists(status)) return VfsError::not_found;

    VfsStat result{};
    result.is_directory = std::filesystem::is_directory(status);
    result.is_readonly = ro;
    if (!result.is_directory) {
        result.size = std::filesystem::file_size(host_path, ec);
        if (ec) return VfsError::io_error;
    }
    out_stat = result;
    return VfsError::ok;
}

VfsError VirtualFileSystem::read_file(const std::string &virtual_path, std::vector<std::uint8_t> &out_data) const {
    std::string norm = normalize_path(virtual_path);
    if (contains_traversal(norm)) return VfsError::traversal_detected;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &pair : bhd5_mounts_) {
            const std::string &m = pair.first;
            if (norm == m || norm.rfind(m + "/", 0) == 0) {
                std::string rel = norm.substr(m.size());
                if (!rel.empty() && rel.front() == '/') rel.erase(0, 1);
                if (pair.second && pair.second->has_file(rel)) {
                    BinderError b_err = pair.second->read_file(rel, out_data);
                    if (b_err == BinderError::ok) return VfsError::ok;
                    return VfsError::io_error;
                }
            }
        }
        for (const auto &pair : bnd4_mounts_) {
            const std::string &m = pair.first;
            if (norm == m || norm.rfind(m + "/", 0) == 0) {
                std::string rel = norm.substr(m.size());
                if (!rel.empty() && rel.front() == '/') rel.erase(0, 1);
                if (pair.second && pair.second->has_file(rel)) {
                    BinderError b_err = pair.second->read_file(rel, out_data);
                    if (b_err == BinderError::ok) return VfsError::ok;
                    return VfsError::io_error;
                }
            }
        }
    }

    std::string host_path;
    bool ro = false;
    VfsError err = resolve(virtual_path, host_path, ro);
    if (err != VfsError::ok) return err;

    std::ifstream file(host_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return VfsError::not_found;
    }

    const auto size = file.tellg();
    if (size < 0) return VfsError::io_error;

    out_data.resize(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    if (size > 0 && !file.read(reinterpret_cast<char *>(out_data.data()), size)) {
        return VfsError::io_error;
    }
    return VfsError::ok;
}

VfsError VirtualFileSystem::write_file(const std::string &virtual_path, const void *data, std::size_t size, bool atomic) {
    std::string host_path;
    bool ro = false;
    VfsError err = resolve(virtual_path, host_path, ro);
    if (err != VfsError::ok) return err;
    if (ro) return VfsError::access_denied;

    std::filesystem::path p(host_path);
    std::error_code ec;
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path(), ec);
    }

    if (atomic) {
        // Escrita at?mica via arquivo tempor?rio + rename
        auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
        std::string tmp_path = host_path + ".tmp." + std::to_string(now_ns);
        {
            std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) return VfsError::io_error;
            if (size > 0 && data) {
                out.write(reinterpret_cast<const char *>(data), size);
            }
            out.flush();
            if (!out.good()) {
                std::filesystem::remove(tmp_path, ec);
                return VfsError::io_error;
            }
        }
        std::filesystem::rename(tmp_path, host_path, ec);
        if (ec) {
            // Em caso de falha de rename (ex: destino j? existente no Windows), tenta remover e renomear
            std::filesystem::remove(host_path, ec);
            std::filesystem::rename(tmp_path, host_path, ec);
            if (ec) {
                std::filesystem::remove(tmp_path, ec);
                return VfsError::io_error;
            }
        }
    } else {
        std::ofstream out(host_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return VfsError::io_error;
        if (size > 0 && data) {
            out.write(reinterpret_cast<const char *>(data), size);
        }
        out.flush();
        if (!out.good()) return VfsError::io_error;
    }
    return VfsError::ok;
}

VfsError VirtualFileSystem::remove_file(const std::string &virtual_path) {
    std::string host_path;
    bool ro = false;
    VfsError err = resolve(virtual_path, host_path, ro);
    if (err != VfsError::ok) return err;
    if (ro) return VfsError::access_denied;

    std::error_code ec;
    if (!std::filesystem::remove(host_path, ec)) {
        return VfsError::not_found;
    }
    return VfsError::ok;
}

} // namespace bb
