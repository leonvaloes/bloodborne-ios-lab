#include "save_data.h"
#include "vfs.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <ctime>

namespace bb {

SaveDataManager::SaveDataManager() = default;

SaveDataManager::~SaveDataManager() {
    terminate();
}

SaveDataManager& SaveDataManager::instance() {
    static SaveDataManager s_instance;
    return s_instance;
}

bool SaveDataManager::initialize(const std::string &base_directory,
                                 std::size_t max_quota,
                                 VirtualFileSystem *vfs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (base_directory.empty()) return false;
    base_dir_ = base_directory;
    max_quota_per_slot_ = max_quota > 0 ? max_quota : DEFAULT_MAX_QUOTA_PER_SLOT;
    vfs_ = vfs;

    std::error_code ec;
    std::filesystem::create_directories(base_dir_, ec);
    std::filesystem::create_directories(std::filesystem::path(base_dir_) / "_work", ec);

    for (auto &slot : slots_) {
        slot = SaveSlotInfo{};
    }

    initialized_ = true;
    return true;
}

bool SaveDataManager::terminate() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return false;

    std::error_code ec;
    for (std::size_t i = 0; i < MAX_SLOTS; ++i) {
        if (slots_[i].used) {
            if (vfs_) {
                vfs_->unmount(slots_[i].mount_point);
            }
            if (!slots_[i].work_path.empty()) {
                std::filesystem::remove_all(slots_[i].work_path, ec);
            }
            slots_[i] = SaveSlotInfo{};
        }
    }

    if (!base_dir_.empty()) {
        std::filesystem::remove_all(std::filesystem::path(base_dir_) / "_work", ec);
    }

    memory_path_.clear();
    memory_size_ = 0;
    initialized_ = false;
    return true;
}

bool SaveDataManager::is_initialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
}

void SaveDataManager::set_base_directory(const std::string &base_directory) {
    std::lock_guard<std::mutex> lock(mutex_);
    base_dir_ = base_directory;
}

const std::string& SaveDataManager::get_base_directory() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return base_dir_;
}

void SaveDataManager::set_default_title_id(const std::string &title_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    default_title_id_ = title_id;
}

const std::string& SaveDataManager::get_default_title_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return default_title_id_;
}

void SaveDataManager::set_max_quota_per_slot(std::size_t quota_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_quota_per_slot_ = quota_bytes;
}

std::size_t SaveDataManager::get_max_quota_per_slot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return max_quota_per_slot_;
}

void SaveDataManager::set_vfs(VirtualFileSystem *vfs) {
    std::lock_guard<std::mutex> lock(mutex_);
    vfs_ = vfs;
}

VirtualFileSystem* SaveDataManager::get_vfs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return vfs_;
}

std::string SaveDataManager::resolve_save_dir(int32_t user, const char *title, const char *dir_name) const {
    const std::string t = (title && title[0] != '\0') ? title : default_title_id_;
    std::filesystem::path p = std::filesystem::path(base_dir_) / "savedata" / std::to_string(user) / t / dir_name;
    return p.string();
}

std::string SaveDataManager::resolve_meta_dir(int32_t user, const char *title, const char *dir_name) const {
    const std::string t = (title && title[0] != '\0') ? title : default_title_id_;
    std::filesystem::path p = std::filesystem::path(base_dir_) / "savedata" / std::to_string(user) / t / (std::string(dir_name) + ".sce_sys");
    return p.string();
}

void SaveDataManager::write_meta_param(const std::string &meta_dir, const SceSaveDataParam &param) {
    std::error_code ec;
    std::filesystem::create_directories(meta_dir, ec);
    std::filesystem::path param_file = std::filesystem::path(meta_dir) / "param.bin";
    std::ofstream out(param_file, std::ios::binary | std::ios::trunc);
    if (out.is_open()) {
        SceSaveDataParam copy = param;
        if (copy.mtime == 0) {
            copy.mtime = static_cast<int64_t>(std::time(nullptr));
        }
        out.write(reinterpret_cast<const char *>(&copy), sizeof(copy));
    }
}

bool SaveDataManager::read_meta_param(const std::string &meta_dir, SceSaveDataParam &param) const {
    std::filesystem::path param_file = std::filesystem::path(meta_dir) / "param.bin";
    std::memset(&param, 0, sizeof(param));
    std::ifstream in(param_file, std::ios::binary);
    if (!in.is_open()) return false;
    in.read(reinterpret_cast<char *>(&param), sizeof(param));
    return in.gcount() == static_cast<std::streamsize>(sizeof(param));
}

int32_t SaveDataManager::mount(int32_t user, const char *title, const char *dir_name, uint32_t mode,
                               uint64_t blocks, std::string &out_mount_point, uint32_t &out_status) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return SCE_SAVEDATA_ERROR_NOT_INITIALIZED;
    if (user < 0) return SCE_SAVEDATA_ERROR_INVALID_USER;
    if (!is_valid_dir_name(dir_name)) return SCE_SAVEDATA_ERROR_PARAMETER;
    if (title && !is_valid_title_id(title)) return SCE_SAVEDATA_ERROR_PARAMETER;

    const std::string host_path = resolve_save_dir(user, title, dir_name);
    const std::string meta_path = resolve_meta_dir(user, title, dir_name);

    std::error_code ec;
    bool exists = std::filesystem::exists(host_path, ec) && std::filesystem::is_directory(host_path, ec);

    if ((mode & SCE_SAVEDATA_MOUNT_MODE_CREATE) && exists) {
        return SCE_SAVEDATA_ERROR_EXISTS;
    }
    if (!(mode & (SCE_SAVEDATA_MOUNT_MODE_CREATE | SCE_SAVEDATA_MOUNT_MODE_CREATE2)) && !exists) {
        return SCE_SAVEDATA_ERROR_NOT_FOUND;
    }

    // Verifica se ja esta montado
    for (std::size_t i = 0; i < MAX_SLOTS; ++i) {
        if (slots_[i].used && slots_[i].host_path == host_path) {
            return SCE_SAVEDATA_ERROR_BAD_MOUNTED;
        }
    }

    // Busca slot livre
    int slot_idx = -1;
    for (std::size_t i = 0; i < MAX_SLOTS; ++i) {
        if (!slots_[i].used) {
            slot_idx = static_cast<int>(i);
            break;
        }
    }
    if (slot_idx < 0) return SCE_SAVEDATA_ERROR_MOUNT_FULL;

    // Cria diretorio de trabalho isolado (staging)
    std::filesystem::path work_path = std::filesystem::path(base_dir_) / "_work" / ("slot_" + std::to_string(slot_idx));
    std::filesystem::remove_all(work_path, ec);
    std::filesystem::create_directories(work_path, ec);
    if (ec) return SCE_SAVEDATA_ERROR_INTERNAL;

    if (exists) {
        std::filesystem::copy(host_path, work_path,
                              std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            std::filesystem::remove_all(work_path, ec);
            return SCE_SAVEDATA_ERROR_INTERNAL;
        }
        apply_bloodborne_sound_hack(work_path.string());
    } else {
        std::filesystem::create_directories(meta_path, ec);
        SceSaveDataParam empty{};
        std::strncpy(empty.title, dir_name, sizeof(empty.title) - 1);
        write_meta_param(meta_path, empty);
    }

    SaveSlotInfo &slot = slots_[slot_idx];
    slot.used = true;
    slot.user = user;
    slot.title_id = (title && title[0] != '\0') ? title : default_title_id_;
    slot.dir_name = dir_name;
    slot.host_path = host_path;
    slot.meta_path = meta_path;
    slot.work_path = work_path.string();
    slot.mount_point = "/savedata" + std::to_string(slot_idx);
    slot.mode = mode;
    slot.read_only = (mode & SCE_SAVEDATA_MOUNT_MODE_RDONLY) != 0;
    slot.blocks = blocks;

    if (vfs_) {
        vfs_->mount(slot.mount_point, slot.work_path, slot.read_only);
    }

    out_mount_point = slot.mount_point;
    out_status = exists ? SCE_SAVEDATA_MOUNT_STATUS_EXISTING : SCE_SAVEDATA_MOUNT_STATUS_CREATED;
    return SCE_SAVEDATA_OK;
}

int32_t SaveDataManager::umount(const std::string &mount_point, bool commit) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return SCE_SAVEDATA_ERROR_NOT_INITIALIZED;

    int slot_idx = find_slot(mount_point);
    if (slot_idx < 0) return SCE_SAVEDATA_ERROR_NOT_MOUNTED;

    SaveSlotInfo &slot = slots_[slot_idx];
    std::error_code ec;

    if (slot.read_only || !commit) {
        // Rollback / descarte: apenas desmonta e descarta copia de trabalho
        if (vfs_) {
            vfs_->unmount(slot.mount_point);
        }
        std::filesystem::remove_all(slot.work_path, ec);
        slot = SaveSlotInfo{};
        return SCE_SAVEDATA_OK;
    }

    // Commit requisitado: verifica limite estrito de quota primeiro
    std::size_t total_size = calculate_directory_size(slot.work_path);
    if (total_size > max_quota_per_slot_) {
        return SCE_SAVEDATA_ERROR_QUOTA_EXCEEDED;
    }

    // Substituicao atomica com rollback em caso de falha
    bool ok = atomic_commit_directory(slot.work_path, slot.host_path);
    if (!ok) {
        return SCE_SAVEDATA_ERROR_INTERNAL;
    }

    // Atualiza mtime nos metadados
    SceSaveDataParam p{};
    if (read_meta_param(slot.meta_path, p)) {
        p.mtime = static_cast<int64_t>(std::time(nullptr));
        write_meta_param(slot.meta_path, p);
    }

    if (vfs_) {
        vfs_->unmount(slot.mount_point);
    }
    std::filesystem::remove_all(slot.work_path, ec);
    slot = SaveSlotInfo{};
    return SCE_SAVEDATA_OK;
}

bool SaveDataManager::atomic_commit_directory(const std::string &staging_dir, const std::string &dest_dir) {
    std::error_code ec;
    std::filesystem::path dest(dest_dir);
    std::filesystem::path parent = dest.parent_path();
    std::filesystem::create_directories(parent, ec);
    if (ec) return false;

    auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path temp_dir = parent / (dest.filename().string() + ".tmp." + std::to_string(now_ns));
    std::filesystem::path bak_dir = parent / (dest.filename().string() + ".bak." + std::to_string(now_ns));

    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::remove_all(bak_dir, ec);

    // Passo 1: Copia staging para diretorio temporario
    std::filesystem::create_directories(temp_dir, ec);
    if (ec) return false;

    std::filesystem::copy(staging_dir, temp_dir,
                          std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        std::filesystem::remove_all(temp_dir, ec);
        return false;
    }

    // Passo 2: Troca atomica (atomic rename) com fallback seguro para Windows
    bool dest_existed = std::filesystem::exists(dest, ec);
    if (dest_existed) {
        std::filesystem::rename(dest, bak_dir, ec);
        if (ec) {
            ec.clear();
            std::filesystem::copy(dest, bak_dir, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
            if (!ec) {
                std::filesystem::remove_all(dest, ec);
            }
        }
        if (ec) {
            std::filesystem::remove_all(temp_dir, ec);
            return false;
        }

        ec.clear();
        std::filesystem::rename(temp_dir, dest, ec);
        if (ec) {
            // Fallback de copia se rename falhar temporariamente por lock no Windows
            ec.clear();
            std::filesystem::create_directories(dest, ec);
            std::filesystem::copy(temp_dir, dest, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                // Rollback: restaura backup
                std::error_code r_ec;
                std::filesystem::rename(bak_dir, dest, r_ec);
                std::filesystem::remove_all(temp_dir, r_ec);
                return false;
            }
        }

        // Remocao do backup e do temp apos substituicao concluida com sucesso
        std::filesystem::remove_all(temp_dir, ec);
        std::filesystem::remove_all(bak_dir, ec);
    } else {
        std::filesystem::rename(temp_dir, dest, ec);
        if (ec) {
            ec.clear();
            std::filesystem::create_directories(dest, ec);
            std::filesystem::copy(temp_dir, dest, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                std::filesystem::remove_all(temp_dir, ec);
                return false;
            }
        }
        std::filesystem::remove_all(temp_dir, ec);
    }

    return true;
}

int32_t SaveDataManager::delete_save(int32_t user, const char *title, const char *dir_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return SCE_SAVEDATA_ERROR_NOT_INITIALIZED;
    if (user < 0) return SCE_SAVEDATA_ERROR_INVALID_USER;
    if (!is_valid_dir_name(dir_name)) return SCE_SAVEDATA_ERROR_PARAMETER;
    if (title && !is_valid_title_id(title)) return SCE_SAVEDATA_ERROR_PARAMETER;

    const std::string host_path = resolve_save_dir(user, title, dir_name);
    const std::string meta_path = resolve_meta_dir(user, title, dir_name);

    // Nao pode remover slot montado
    for (const auto &slot : slots_) {
        if (slot.used && slot.host_path == host_path) {
            return SCE_SAVEDATA_ERROR_BAD_MOUNTED;
        }
    }

    std::error_code ec;
    if (!std::filesystem::exists(host_path, ec)) {
        return SCE_SAVEDATA_ERROR_NOT_FOUND;
    }

    std::filesystem::remove_all(host_path, ec);
    std::filesystem::remove_all(meta_path, ec);
    return SCE_SAVEDATA_OK;
}

int32_t SaveDataManager::dir_name_search(int32_t user, const char *title, const char *pattern,
                                         uint32_t sort_key, uint32_t sort_order,
                                         std::vector<SaveSearchEntry> &out_entries) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return SCE_SAVEDATA_ERROR_NOT_INITIALIZED;
    if (user < 0) return SCE_SAVEDATA_ERROR_INVALID_USER;
    if (title && !is_valid_title_id(title)) return SCE_SAVEDATA_ERROR_PARAMETER;

    out_entries.clear();
    const std::string t = (title && title[0] != '\0') ? title : default_title_id_;
    std::filesystem::path search_base = std::filesystem::path(base_dir_) / "savedata" / std::to_string(user) / t;

    std::error_code ec;
    if (!std::filesystem::exists(search_base, ec) || !std::filesystem::is_directory(search_base, ec)) {
        return SCE_SAVEDATA_OK;
    }

    std::string pat = (pattern && pattern[0] != '\0') ? pattern : "%";

    for (const auto &entry : std::filesystem::directory_iterator(search_base, ec)) {
        if (!entry.is_directory(ec)) continue;

        std::string filename = entry.path().filename().string();
        if (filename.empty() || filename[0] == '.') continue;
        if (filename.size() > 8 && filename.rfind(".sce_sys") == filename.size() - 8) continue;
        if (filename.rfind(".tmp") != std::string::npos || filename.rfind(".bak") != std::string::npos) continue;

        if (!pattern_matches(filename, pat)) continue;

        SaveSearchEntry se{};
        se.dir_name = filename;
        std::string meta_dir = (entry.path().string() + ".sce_sys");
        read_meta_param(meta_dir, se.param);
        se.blocks = 32768;
        se.free_blocks = 16384;
        out_entries.push_back(se);
    }

    std::sort(out_entries.begin(), out_entries.end(), [sort_key, sort_order](const SaveSearchEntry &a, const SaveSearchEntry &b) {
        int cmp = 0;
        if (sort_key == SCE_SAVEDATA_SORT_KEY_USER_PARAM) {
            if (a.param.user_param < b.param.user_param) cmp = -1;
            else if (a.param.user_param > b.param.user_param) cmp = 1;
        } else if (sort_key == SCE_SAVEDATA_SORT_KEY_MTIME) {
            if (a.param.mtime < b.param.mtime) cmp = -1;
            else if (a.param.mtime > b.param.mtime) cmp = 1;
        }
        if (cmp == 0) {
            cmp = a.dir_name.compare(b.dir_name);
        }
        return (sort_order == SCE_SAVEDATA_SORT_ORDER_DESC) ? (cmp > 0) : (cmp < 0);
    });

    return SCE_SAVEDATA_OK;
}

int32_t SaveDataManager::set_param(const std::string &mount_point, uint32_t type, const void *buf, std::size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return SCE_SAVEDATA_ERROR_NOT_INITIALIZED;
    if (!buf) return SCE_SAVEDATA_ERROR_PARAMETER;
    int slot_idx = find_slot(mount_point);
    if (slot_idx < 0) return SCE_SAVEDATA_ERROR_NOT_MOUNTED;

    SaveSlotInfo &slot = slots_[slot_idx];
    SceSaveDataParam p{};
    read_meta_param(slot.meta_path, p);

    switch (type) {
    case 0:
        if (size < sizeof(SceSaveDataParam)) return SCE_SAVEDATA_ERROR_PARAMETER;
        std::memcpy(&p, buf, sizeof(p));
        break;
    case 1:
        std::memset(p.title, 0, sizeof(p.title));
        std::strncpy(p.title, reinterpret_cast<const char *>(buf), std::min<std::size_t>(size, sizeof(p.title) - 1));
        break;
    case 2:
        std::memset(p.subtitle, 0, sizeof(p.subtitle));
        std::strncpy(p.subtitle, reinterpret_cast<const char *>(buf), std::min<std::size_t>(size, sizeof(p.subtitle) - 1));
        break;
    case 3:
        std::memset(p.detail, 0, sizeof(p.detail));
        std::strncpy(p.detail, reinterpret_cast<const char *>(buf), std::min<std::size_t>(size, sizeof(p.detail) - 1));
        break;
    case 4:
        if (size < sizeof(uint32_t)) return SCE_SAVEDATA_ERROR_PARAMETER;
        std::memcpy(&p.user_param, buf, sizeof(uint32_t));
        break;
    default:
        return SCE_SAVEDATA_ERROR_PARAMETER;
    }

    write_meta_param(slot.meta_path, p);
    return SCE_SAVEDATA_OK;
}

int32_t SaveDataManager::get_param(const std::string &mount_point, SceSaveDataParam &out_param) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return SCE_SAVEDATA_ERROR_NOT_INITIALIZED;
    int slot_idx = find_slot(mount_point);
    if (slot_idx < 0) return SCE_SAVEDATA_ERROR_NOT_MOUNTED;

    const SaveSlotInfo &slot = slots_[slot_idx];
    if (!read_meta_param(slot.meta_path, out_param)) {
        return SCE_SAVEDATA_ERROR_NOT_FOUND;
    }
    return SCE_SAVEDATA_OK;
}

int32_t SaveDataManager::save_icon(const std::string &mount_point, const void *data, std::size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return SCE_SAVEDATA_ERROR_NOT_INITIALIZED;
    if (!data || size == 0) return SCE_SAVEDATA_ERROR_PARAMETER;
    int slot_idx = find_slot(mount_point);
    if (slot_idx < 0) return SCE_SAVEDATA_ERROR_NOT_MOUNTED;

    const SaveSlotInfo &slot = slots_[slot_idx];
    std::error_code ec;
    std::filesystem::create_directories(slot.meta_path, ec);

    std::filesystem::path icon_path = std::filesystem::path(slot.meta_path) / "icon0.png";
    std::ofstream out(icon_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return SCE_SAVEDATA_ERROR_INTERNAL;
    out.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
    return out.good() ? SCE_SAVEDATA_OK : SCE_SAVEDATA_ERROR_INTERNAL;
}

int32_t SaveDataManager::setup_memory(int32_t user, uint64_t size, const SceSaveDataParam *param) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return SCE_SAVEDATA_ERROR_NOT_INITIALIZED;
    if (user < 0) return SCE_SAVEDATA_ERROR_INVALID_USER;
    if (size == 0) return SCE_SAVEDATA_ERROR_PARAMETER;
    if (size > max_quota_per_slot_) return SCE_SAVEDATA_ERROR_QUOTA_EXCEEDED;

    std::filesystem::path mem_dir = std::filesystem::path(base_dir_) / "savedata" / std::to_string(user) / (default_title_id_ + ".memory");
    std::error_code ec;
    std::filesystem::create_directories(mem_dir, ec);
    if (ec) return SCE_SAVEDATA_ERROR_INTERNAL;

    memory_path_ = (mem_dir / "memory.dat").string();
    std::ofstream f(memory_path_, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return SCE_SAVEDATA_ERROR_INTERNAL;
    std::vector<char> zero(static_cast<std::size_t>(size), 0);
    f.write(zero.data(), zero.size());
    if (!f.good()) return SCE_SAVEDATA_ERROR_INTERNAL;
    f.close();

    memory_size_ = size;

    if (param) {
        write_meta_param(mem_dir.string(), *param);
    }
    return SCE_SAVEDATA_OK;
}

int32_t SaveDataManager::get_memory(int32_t user, void *buffer, uint64_t size, int64_t offset) {
    (void)user;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return SCE_SAVEDATA_ERROR_NOT_INITIALIZED;
    if (!buffer || offset < 0) return SCE_SAVEDATA_ERROR_PARAMETER;
    if (memory_size_ == 0 || memory_path_.empty()) return SCE_SAVEDATA_ERROR_MEMORY_NOT_READY;
    if (static_cast<uint64_t>(offset) + size > memory_size_) return SCE_SAVEDATA_ERROR_PARAMETER;

    std::ifstream f(memory_path_, std::ios::binary);
    if (!f.is_open()) return SCE_SAVEDATA_ERROR_INTERNAL;
    f.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!f.read(reinterpret_cast<char *>(buffer), static_cast<std::streamsize>(size))) {
        return SCE_SAVEDATA_ERROR_INTERNAL;
    }
    return SCE_SAVEDATA_OK;
}

int32_t SaveDataManager::set_memory(int32_t user, const void *buffer, uint64_t size, int64_t offset) {
    (void)user;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return SCE_SAVEDATA_ERROR_NOT_INITIALIZED;
    if (!buffer || offset < 0) return SCE_SAVEDATA_ERROR_PARAMETER;
    if (memory_size_ == 0 || memory_path_.empty()) return SCE_SAVEDATA_ERROR_MEMORY_NOT_READY;
    if (static_cast<uint64_t>(offset) + size > memory_size_) return SCE_SAVEDATA_ERROR_PARAMETER;

    std::fstream f(memory_path_, std::ios::in | std::ios::out | std::ios::binary);
    if (!f.is_open()) return SCE_SAVEDATA_ERROR_INTERNAL;
    f.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!f.write(reinterpret_cast<const char *>(buffer), static_cast<std::streamsize>(size))) {
        return SCE_SAVEDATA_ERROR_INTERNAL;
    }
    f.flush();
    return f.good() ? SCE_SAVEDATA_OK : SCE_SAVEDATA_ERROR_INTERNAL;
}

int32_t SaveDataManager::write_file(const std::string &mount_point, const std::string &relative_path,
                                    const void *data, std::size_t size, bool atomic) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return SCE_SAVEDATA_ERROR_NOT_INITIALIZED;
    int slot_idx = find_slot(mount_point);
    if (slot_idx < 0) return SCE_SAVEDATA_ERROR_NOT_MOUNTED;
    if (slots_[slot_idx].read_only) return SCE_SAVEDATA_ERROR_PARAMETER;
    if (relative_path.empty() || contains_traversal(relative_path)) {
        return SCE_SAVEDATA_ERROR_PARAMETER;
    }

    std::filesystem::path target = std::filesystem::path(slots_[slot_idx].work_path) / relative_path;
    std::error_code ec;
    std::size_t old_size = 0;
    if (std::filesystem::exists(target, ec)) {
        old_size = static_cast<std::size_t>(std::filesystem::file_size(target, ec));
    }
    std::size_t current_total = calculate_directory_size(slots_[slot_idx].work_path);
    if (current_total - old_size + size > max_quota_per_slot_) {
        return SCE_SAVEDATA_ERROR_QUOTA_EXCEEDED;
    }

    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), ec);
    }

    if (atomic) {
        // Protocolo de gravação em 4 fases: tmp -> checksum -> backup -> rename atômico (T025)
        auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
        std::filesystem::path tmp = target.string() + ".tmp." + std::to_string(ts);
        std::filesystem::path bak = target.string() + ".bak." + std::to_string(ts);

        // Fase 1: Escrita no arquivo temporário .tmp
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) return SCE_SAVEDATA_ERROR_INTERNAL;
            if (size > 0 && data) {
                out.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
            }
            out.flush();
            if (!out.good()) {
                std::filesystem::remove(tmp, ec);
                return SCE_SAVEDATA_ERROR_INTERNAL;
            }
        }

        // Fase 2: Verificação de integridade via checksum CRC32 dos dados gravados
        if (size > 0 && data) {
            std::ifstream in(tmp, std::ios::binary);
            if (!in.is_open()) {
                std::filesystem::remove(tmp, ec);
                return SCE_SAVEDATA_ERROR_INTERNAL;
            }
            std::vector<uint8_t> verify_buf(size);
            in.read(reinterpret_cast<char *>(verify_buf.data()), static_cast<std::streamsize>(size));
            if (!in.good() && !in.eof()) {
                std::filesystem::remove(tmp, ec);
                return SCE_SAVEDATA_ERROR_INTERNAL;
            }
            uint32_t expected_crc = VirtualFileSystem::compute_crc32(data, size);
            uint32_t actual_crc = VirtualFileSystem::compute_crc32(verify_buf.data(), verify_buf.size());
            if (expected_crc != actual_crc) {
                std::filesystem::remove(tmp, ec);
                return SCE_SAVEDATA_ERROR_INTERNAL;
            }
        }

        // Fase 3: Backup do arquivo de destino existente para .bak
        bool target_existed = std::filesystem::exists(target, ec);
        if (target_existed) {
            std::filesystem::copy_file(target, bak, std::filesystem::copy_options::overwrite_existing, ec);
        }

        // Fase 4: Rename atômico de .tmp para target com garantia de rollback
        std::filesystem::rename(tmp, target, ec);
        if (ec) {
            std::filesystem::remove(target, ec);
            std::filesystem::rename(tmp, target, ec);
            if (ec) {
                if (target_existed) {
                    std::error_code r_ec;
                    std::filesystem::copy_file(bak, target, std::filesystem::copy_options::overwrite_existing, r_ec);
                }
                std::filesystem::remove(tmp, ec);
                std::filesystem::remove(bak, ec);
                return SCE_SAVEDATA_ERROR_INTERNAL;
            }
        }

        // Limpeza do backup após rename com sucesso
        std::filesystem::remove(bak, ec);
    } else {
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return SCE_SAVEDATA_ERROR_INTERNAL;
        if (size > 0 && data) {
            out.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
        }
        out.flush();
        if (!out.good()) return SCE_SAVEDATA_ERROR_INTERNAL;
    }
    return SCE_SAVEDATA_OK;
}

int32_t SaveDataManager::read_file(const std::string &mount_point, const std::string &relative_path,
                                   std::vector<uint8_t> &out_data) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return SCE_SAVEDATA_ERROR_NOT_INITIALIZED;
    int slot_idx = find_slot(mount_point);
    if (slot_idx < 0) return SCE_SAVEDATA_ERROR_NOT_MOUNTED;
    if (relative_path.empty() || contains_traversal(relative_path)) {
        return SCE_SAVEDATA_ERROR_PARAMETER;
    }

    std::filesystem::path target = std::filesystem::path(slots_[slot_idx].work_path) / relative_path;
    std::ifstream file(target, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return SCE_SAVEDATA_ERROR_NOT_FOUND;
    auto size = file.tellg();
    if (size < 0) return SCE_SAVEDATA_ERROR_INTERNAL;

    out_data.resize(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    if (size > 0 && !file.read(reinterpret_cast<char *>(out_data.data()), size)) {
        return SCE_SAVEDATA_ERROR_INTERNAL;
    }
    return SCE_SAVEDATA_OK;
}

int32_t SaveDataManager::remove_file(const std::string &mount_point, const std::string &relative_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return SCE_SAVEDATA_ERROR_NOT_INITIALIZED;
    int slot_idx = find_slot(mount_point);
    if (slot_idx < 0) return SCE_SAVEDATA_ERROR_NOT_MOUNTED;
    if (slots_[slot_idx].read_only) return SCE_SAVEDATA_ERROR_PARAMETER;
    if (relative_path.empty() || contains_traversal(relative_path)) {
        return SCE_SAVEDATA_ERROR_PARAMETER;
    }

    std::filesystem::path target = std::filesystem::path(slots_[slot_idx].work_path) / relative_path;
    std::error_code ec;
    if (!std::filesystem::exists(target, ec)) {
        return SCE_SAVEDATA_ERROR_NOT_FOUND;
    }
    std::filesystem::remove(target, ec);
    return ec ? SCE_SAVEDATA_ERROR_INTERNAL : SCE_SAVEDATA_OK;
}

bool SaveDataManager::file_exists(const std::string &mount_point, const std::string &relative_path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return false;
    int slot_idx = find_slot(mount_point);
    if (slot_idx < 0) return false;
    if (relative_path.empty() || contains_traversal(relative_path)) return false;

    std::filesystem::path target = std::filesystem::path(slots_[slot_idx].work_path) / relative_path;
    std::error_code ec;
    return std::filesystem::exists(target, ec);
}

int SaveDataManager::find_slot(const std::string &mount_point) const {
    for (std::size_t i = 0; i < MAX_SLOTS; ++i) {
        if (slots_[i].used && slots_[i].mount_point == mount_point) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const SaveSlotInfo* SaveDataManager::get_slot(int index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= 0 && index < static_cast<int>(MAX_SLOTS) && slots_[index].used) {
        return &slots_[index];
    }
    return nullptr;
}

std::size_t SaveDataManager::calculate_directory_size(const std::string &dir_path) const {
    std::error_code ec;
    if (!std::filesystem::exists(dir_path, ec) || !std::filesystem::is_directory(dir_path, ec)) {
        return 0;
    }
    std::size_t total = 0;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(dir_path, ec)) {
        if (entry.is_regular_file(ec)) {
            total += static_cast<std::size_t>(entry.file_size(ec));
        }
    }
    return total;
}

std::size_t SaveDataManager::calculate_slot_size(int index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= 0 && index < static_cast<int>(MAX_SLOTS) && slots_[index].used) {
        return calculate_directory_size(slots_[index].work_path);
    }
    return 0;
}

bool SaveDataManager::is_valid_dir_name(const char *name, std::size_t max_len) {
    if (!name || name[0] == '\0') return false;
    std::size_t len = 0;
    while (name[len] != '\0') {
        if (len >= max_len - 1) return false;
        char c = name[len];
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' ||
            static_cast<unsigned char>(c) < 32) {
            return false;
        }
        ++len;
    }
    if (name[0] == '.' || name[0] == ' ') return false;
    std::string str(name, len);
    if (contains_traversal(str)) return false;
    return true;
}

bool SaveDataManager::is_valid_title_id(const char *title, std::size_t max_len) {
    if (!title || title[0] == '\0') return false;
    std::size_t len = 0;
    while (title[len] != '\0') {
        if (len >= max_len - 1) return false;
        char c = title[len];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return false;
        }
        ++len;
    }
    return true;
}

bool SaveDataManager::contains_traversal(const std::string &path) {
    if (path.empty()) return false;
    std::size_t pos = 0;
    while ((pos = path.find("..", pos)) != std::string::npos) {
        bool left_boundary = (pos == 0 || path[pos - 1] == '/' || path[pos - 1] == '\\');
        bool right_boundary = (pos + 2 == path.size() || path[pos + 2] == '/' || path[pos + 2] == '\\');
        if (left_boundary && right_boundary) {
            return true;
        }
        pos += 2;
    }
    return false;
}

bool SaveDataManager::pattern_matches(const std::string &str, const std::string &pattern) {
    auto it_s = str.begin();
    auto it_p = pattern.begin();
    auto star_s = str.end();
    auto star_p = pattern.end();

    while (it_s != str.end()) {
        if (it_p != pattern.end() && (*it_p == '%' || *it_p == '*')) {
            star_p = it_p++;
            star_s = it_s;
        } else if (it_p != pattern.end() && (*it_p == '_' || *it_p == '?' || *it_p == *it_s)) {
            ++it_s;
            ++it_p;
        } else if (star_p != pattern.end()) {
            it_p = star_p + 1;
            it_s = ++star_s;
        } else {
            return false;
        }
    }
    while (it_p != pattern.end() && (*it_p == '%' || *it_p == '*')) {
        ++it_p;
    }
    return it_p == pattern.end();
}

void SaveDataManager::apply_bloodborne_sound_hack(const std::string &work_path) {
    const char *env = std::getenv("BB_SOUND_HACK");
    if (env && env[0] == '0') return;

    std::filesystem::path user_file = std::filesystem::path(work_path) / "userdata0010";
    std::error_code ec;
    if (!std::filesystem::exists(user_file, ec)) return;

    auto fsize = std::filesystem::file_size(user_file, ec);
    if (ec || fsize <= 0x204E) return;

    std::fstream f(user_file, std::ios::in | std::ios::out | std::ios::binary);
    if (!f.is_open()) return;

    f.seekg(0x204E, std::ios::beg);
    char val = 0;
    if (f.read(&val, 1) && val != 1) {
        f.seekp(0x204E, std::ios::beg);
        char flag = 1;
        f.write(&flag, 1);
        f.flush();
    }
}

} // namespace bb

// ============================================================================
// libSceSaveData C HLE Export Functions
// ============================================================================

extern "C" {

int32_t sceSaveDataInitialize(const bb::SceSaveDataInitParams *init_params) {
    std::size_t quota = bb::SCE_SAVEDATA_DEFAULT_QUOTA;
    if (init_params && init_params->max_quota_per_slot > 0) {
        quota = static_cast<std::size_t>(init_params->max_quota_per_slot);
    }
    bool ok = bb::SaveDataManager::instance().initialize("savedata", quota, nullptr);
    return ok ? bb::SCE_SAVEDATA_OK : bb::SCE_SAVEDATA_ERROR_INTERNAL;
}

int32_t sceSaveDataInitialize2(const bb::SceSaveDataInitParams *init_params) {
    return sceSaveDataInitialize(init_params);
}

int32_t sceSaveDataInitialize3(const bb::SceSaveDataInitParams *init_params) {
    return sceSaveDataInitialize(init_params);
}

int32_t sceSaveDataTerminate(void) {
    bool ok = bb::SaveDataManager::instance().terminate();
    return ok ? bb::SCE_SAVEDATA_OK : bb::SCE_SAVEDATA_ERROR_NOT_INITIALIZED;
}

int32_t sceSaveDataMount(const bb::SceSaveDataMount *mount, bb::SceSaveDataMountResult *result) {
    if (!mount || !result) return bb::SCE_SAVEDATA_ERROR_PARAMETER;
    const char *title = mount->title ? mount->title->data : nullptr;
    const char *dir = mount->dir ? mount->dir->data : nullptr;
    std::string mount_pt;
    uint32_t status = 0;
    int32_t res = bb::SaveDataManager::instance().mount(
        mount->user, title, dir, mount->mode, mount->blocks, mount_pt, status);
    if (res == bb::SCE_SAVEDATA_OK) {
        std::memset(result, 0, sizeof(*result));
        std::strncpy(result->point.data, mount_pt.c_str(), sizeof(result->point.data) - 1);
        result->status = status;
        result->required_blocks = mount->blocks;
    }
    return res;
}

int32_t sceSaveDataMount2(const bb::SceSaveDataMount2 *mount, bb::SceSaveDataMountResult *result) {
    if (!mount || !result) return bb::SCE_SAVEDATA_ERROR_PARAMETER;
    const char *dir = mount->dir ? mount->dir->data : nullptr;
    std::string mount_pt;
    uint32_t status = 0;
    int32_t res = bb::SaveDataManager::instance().mount(
        mount->user, nullptr, dir, mount->mode, mount->blocks, mount_pt, status);
    if (res == bb::SCE_SAVEDATA_OK) {
        std::memset(result, 0, sizeof(*result));
        std::strncpy(result->point.data, mount_pt.c_str(), sizeof(result->point.data) - 1);
        result->status = status;
        result->required_blocks = mount->blocks;
    }
    return res;
}

int32_t sceSaveDataUmount(const bb::SceSaveDataMountPoint *mount_point, uint32_t flags) {
    if (!mount_point) return bb::SCE_SAVEDATA_ERROR_PARAMETER;
    bool commit = (flags & bb::SCE_SAVEDATA_UMOUNT_FLAG_COMMIT) != 0;
    return bb::SaveDataManager::instance().umount(mount_point->data, commit);
}

int32_t sceSaveDataUmountWithCommit(const bb::SceSaveDataMountPoint *mount_point, bool commit) {
    if (!mount_point) return bb::SCE_SAVEDATA_ERROR_PARAMETER;
    return bb::SaveDataManager::instance().umount(mount_point->data, commit);
}

int32_t sceSaveDataDelete(const bb::SceSaveDataDelete *del) {
    if (!del || !del->dir) return bb::SCE_SAVEDATA_ERROR_PARAMETER;
    const char *title = del->title ? del->title->data : nullptr;
    return bb::SaveDataManager::instance().delete_save(del->user, title, del->dir->data);
}

int32_t sceSaveDataDirNameSearch(const bb::SceSaveDataSearchCond *cond, bb::SceSaveDataSearchResult *result) {
    if (!cond || !result) return bb::SCE_SAVEDATA_ERROR_PARAMETER;
    const char *title = cond->title ? cond->title->data : nullptr;
    const char *dir_pattern = cond->dir ? cond->dir->data : nullptr;
    std::vector<bb::SaveSearchEntry> entries;
    int32_t res = bb::SaveDataManager::instance().dir_name_search(
        cond->user, title, dir_pattern, cond->key, cond->order, entries);
    if (res != bb::SCE_SAVEDATA_OK) return res;

    result->hits = static_cast<uint32_t>(entries.size());
    uint32_t set = static_cast<uint32_t>(std::min<std::size_t>(entries.size(), result->names_capacity));
    result->set_count = set;
    for (uint32_t i = 0; i < set; ++i) {
        if (result->names) {
            std::memset(result->names[i].data, 0, sizeof(result->names[i].data));
            std::strncpy(result->names[i].data, entries[i].dir_name.c_str(), sizeof(result->names[i].data) - 1);
        }
        if (result->params) {
            result->params[i] = entries[i].param;
        }
        if (result->infos) {
            result->infos[i].blocks = entries[i].blocks;
            result->infos[i].free_blocks = entries[i].free_blocks;
            std::memset(result->infos[i].reserved, 0, sizeof(result->infos[i].reserved));
        }
    }
    return bb::SCE_SAVEDATA_OK;
}

int32_t sceSaveDataSetParam(const bb::SceSaveDataMountPoint *mount_point, uint32_t type, const void *buffer, uint64_t size) {
    if (!mount_point || !buffer) return bb::SCE_SAVEDATA_ERROR_PARAMETER;
    return bb::SaveDataManager::instance().set_param(mount_point->data, type, buffer, static_cast<std::size_t>(size));
}

int32_t sceSaveDataSaveIcon(const bb::SceSaveDataMountPoint *mount_point, const bb::SceSaveDataIcon *icon) {
    if (!mount_point || !icon || !icon->buffer) return bb::SCE_SAVEDATA_ERROR_PARAMETER;
    return bb::SaveDataManager::instance().save_icon(mount_point->data, icon->buffer, static_cast<std::size_t>(icon->data_size));
}

int32_t sceSaveDataSetupSaveDataMemory(int32_t user, uint64_t size, const bb::SceSaveDataParam *param) {
    return bb::SaveDataManager::instance().setup_memory(user, size, param);
}

int32_t sceSaveDataGetSaveDataMemory(int32_t user, void *buffer, uint64_t size, int64_t offset) {
    return bb::SaveDataManager::instance().get_memory(user, buffer, size, offset);
}

int32_t sceSaveDataSetSaveDataMemory(int32_t user, const void *buffer, uint64_t size, int64_t offset) {
    return bb::SaveDataManager::instance().set_memory(user, buffer, size, offset);
}

} // extern "C"
