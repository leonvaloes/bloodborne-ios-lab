#include "save_data.h"
#include "vfs.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <vector>
#include <limits>

namespace {

class Sha256 {
public:
    Sha256() { reset(); }

    void reset() {
        state_[0] = 0x6a09e667;
        state_[1] = 0xbb67ae85;
        state_[2] = 0x3c6ef372;
        state_[3] = 0xa54ff53a;
        state_[4] = 0x510e527f;
        state_[5] = 0x9b05688c;
        state_[6] = 0x1f83d9ab;
        state_[7] = 0x5be0cd19;
        count_ = 0;
        buffer_len_ = 0;
    }

    void update(const void *data, std::size_t len) {
        const auto *p = static_cast<const uint8_t *>(data);
        count_ += len;
        while (len > 0) {
            std::size_t to_copy = std::min(len, 64 - buffer_len_);
            std::memcpy(buffer_ + buffer_len_, p, to_copy);
            buffer_len_ += to_copy;
            p += to_copy;
            len -= to_copy;
            if (buffer_len_ == 64) {
                transform(buffer_);
                buffer_len_ = 0;
            }
        }
    }

    std::string finalize() {
        uint64_t total_bits = count_ * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (buffer_len_ != 56) {
            update(&zero, 1);
        }
        for (int i = 7; i >= 0; --i) {
            uint8_t b = static_cast<uint8_t>((total_bits >> (i * 8)) & 0xff);
            update(&b, 1);
        }

        std::ostringstream ss;
        ss << std::hex << std::setfill('0');
        for (uint32_t val : state_) {
            ss << std::setw(8) << val;
        }
        return ss.str();
    }

private:
    static inline uint32_t rotr(uint32_t x, uint32_t n) {
        return (x >> n) | (x << (32 - n));
    }

    static inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) {
        return (x & y) ^ (~x & z);
    }

    static inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) {
        return (x & y) ^ (x & z) ^ (y & z);
    }

    static inline uint32_t sigma0(uint32_t x) {
        return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
    }

    static inline uint32_t sigma1(uint32_t x) {
        return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
    }

    static inline uint32_t gamma0(uint32_t x) {
        return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
    }

    static inline uint32_t gamma1(uint32_t x) {
        return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
    }

    void transform(const uint8_t chunk[64]) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
            0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
            0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
            0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
            0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
            0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
        };

        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(chunk[i * 4]) << 24) |
                   (static_cast<uint32_t>(chunk[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(chunk[i * 4 + 2]) << 8) |
                   (static_cast<uint32_t>(chunk[i * 4 + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            w[i] = gamma1(w[i - 2]) + w[i - 7] + gamma0(w[i - 15]) + w[i - 16];
        }

        uint32_t a = state_[0];
        uint32_t b = state_[1];
        uint32_t c = state_[2];
        uint32_t d = state_[3];
        uint32_t e = state_[4];
        uint32_t f = state_[5];
        uint32_t g = state_[6];
        uint32_t h = state_[7];

        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = h + sigma1(e) + ch(e, f, g) + K[i] + w[i];
            uint32_t t2 = sigma0(a) + maj(a, b, c);
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    uint32_t state_[8];
    uint64_t count_;
    uint8_t buffer_[64];
    std::size_t buffer_len_;
};

} // namespace

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
    if (initialized_) {
        if (base_dir_ != base_directory) return false; // não descartar slots montados
        max_quota_per_slot_ = max_quota > 0 ? max_quota : DEFAULT_MAX_QUOTA_PER_SLOT;
        vfs_ = vfs;
        return true;
    }

    // Verificar ambos os diretórios antes de publicar estado: create_directories
    // pode falhar (ou encontrar um arquivo) sem lançar quando recebe error_code.
    std::filesystem::path root(base_directory);
    if (root.filename() != "savedata") root /= "savedata";
    std::error_code ec;
    const bool root_created = std::filesystem::create_directories(root, ec);
    if (ec || !std::filesystem::is_directory(root, ec) || ec) return false;
    std::filesystem::create_directories(root / "_work", ec);
    if (ec || !std::filesystem::is_directory(root / "_work", ec) || ec) {
        if (root_created) {
            std::error_code cleanup_ec;
            std::filesystem::remove(root, cleanup_ec); // só se vazio; nunca remove dados preexistentes
        }
        return false;
    }

    base_dir_ = base_directory;
    max_quota_per_slot_ = max_quota > 0 ? max_quota : DEFAULT_MAX_QUOTA_PER_SLOT;
    vfs_ = vfs;
    for (auto &slot : slots_) slot = SaveSlotInfo{};
    initialized_ = true;
    recover_interrupted_commits();
    return true;
}

bool SaveDataManager::terminate() {
    // umount() owns mutex_ and is the only path that coordinates data, metadata
    // and checksum rollback. Never discard a mounted work directory on failure.
    for (;;) {
        std::string mount_point;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!initialized_) return false;
            for (const auto &slot : slots_) {
                if (slot.used) {
                    mount_point = slot.mount_point;
                    break;
                }
            }
            if (mount_point.empty()) {
                user_memories_.clear();
                initialized_ = false;
                return true;
            }
        }
        if (umount(mount_point, true) != SCE_SAVEDATA_OK) return false;
    }
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

std::filesystem::path SaveDataManager::resolve_savedata_root() const {
    std::filesystem::path b(base_dir_);
    while (b.filename().empty() && b.has_parent_path() && b != b.root_path()) {
        b = b.parent_path();
    }
    if (b.filename() == "savedata") {
        return b;
    }
    return b / "savedata";
}

bool SaveDataManager::is_confined_to_savedata_root(const std::filesystem::path &target) const {
    std::filesystem::path root = resolve_savedata_root();
    std::error_code ec;
    std::filesystem::path canonical_root = std::filesystem::weakly_canonical(root, ec);
    if (ec || canonical_root.empty()) return false;

    std::filesystem::path canonical_target = std::filesystem::weakly_canonical(target, ec);
    if (ec || canonical_target.empty()) return false;

    // Check if target or any existing component of target is a symlink pointing outside canonical_root
    std::filesystem::path curr = target;
    while (!curr.empty() && curr != curr.root_path()) {
        if (std::filesystem::is_symlink(curr, ec)) {
            std::filesystem::path sym_dest = std::filesystem::canonical(curr, ec);
            if (ec) return false;
            auto sym_rel = std::filesystem::relative(sym_dest, canonical_root, ec);
            auto first_sym = sym_rel.begin() != sym_rel.end() ? sym_rel.begin()->string() : "";
            if (ec || sym_rel.empty() || first_sym == ".." || sym_rel.is_absolute()) {
                return false;
            }
        }
        if (curr == root) break;
        curr = curr.parent_path();
    }

    auto rel = std::filesystem::relative(canonical_target, canonical_root, ec);
    auto first_elem = rel.begin() != rel.end() ? rel.begin()->string() : "";
    if (ec || rel.empty() || first_elem == ".." || rel.is_absolute()) {
        return false;
    }
    return true;
}

void SaveDataManager::recover_interrupted_commits() {
    std::error_code ec;
    std::filesystem::path root = resolve_savedata_root();
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) return;

    std::filesystem::path work_dir = root / "_work";
    if (std::filesystem::exists(work_dir, ec) && std::filesystem::is_directory(work_dir, ec)) {
        for (const auto &w_entry : std::filesystem::directory_iterator(work_dir, ec)) {
            bool is_active = false;
            for (const auto &slot : slots_) {
                if (slot.used && !slot.work_path.empty()) {
                    std::error_code eq_ec;
                    if (std::filesystem::equivalent(slot.work_path, w_entry.path(), eq_ec)) {
                        is_active = true;
                        break;
                    }
                }
            }
            if (!is_active) {
                std::filesystem::remove_all(w_entry.path(), ec);
            }
        }
    }

    for (const auto &user_entry : std::filesystem::directory_iterator(root, ec)) {
        if (!user_entry.is_directory(ec)) continue;
        if (user_entry.path().filename() == "_work") continue;

        for (const auto &title_entry : std::filesystem::directory_iterator(user_entry.path(), ec)) {
            if (!title_entry.is_directory(ec)) continue;

            std::vector<std::filesystem::path> bak_dirs;
            std::vector<std::filesystem::path> tmp_dirs;
            std::vector<std::filesystem::path> delbak_dirs;

            for (const auto &entry : std::filesystem::directory_iterator(title_entry.path(), ec)) {
                std::string fname = entry.path().filename().string();
                if (fname.find(".delbak") != std::string::npos) {
                    delbak_dirs.push_back(entry.path());
                } else if (fname.find(".bak") != std::string::npos) {
                    bak_dirs.push_back(entry.path());
                } else if (fname.find(".tmp") != std::string::npos) {
                    tmp_dirs.push_back(entry.path());
                }
            }

            for (const auto &delbak_p : delbak_dirs) {
                std::string fname = delbak_p.filename().string();
                std::size_t pos = fname.find(".delbak");
                std::string base_name = fname.substr(0, pos);
                std::filesystem::path dest = title_entry.path() / base_name;
                std::filesystem::path meta = title_entry.path() / (base_name + ".sce_sys");

                bool dest_exists = std::filesystem::exists(dest, ec);
                bool meta_exists = std::filesystem::exists(meta, ec);

                if (!dest_exists && meta_exists) {
                    // T148: Interrupção/crash durante exclusão com metadados ainda presentes; restaura dados
                    std::filesystem::rename(delbak_p, dest, ec);
                    if (ec) {
                        ec.clear();
                        std::filesystem::copy(delbak_p, dest, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
                        if (!ec) {
                            std::filesystem::remove_all(delbak_p, ec);
                        }
                    }
                } else {
                    std::filesystem::remove_all(delbak_p, ec);
                }
            }

            for (const auto &bak_p : bak_dirs) {
                std::string fname = bak_p.filename().string();
                std::size_t pos = fname.find(".bak");
                std::string base_name = fname.substr(0, pos);
                std::filesystem::path dest = title_entry.path() / base_name;

                bool dest_exists = std::filesystem::exists(dest, ec);
                bool should_restore = !dest_exists;
                if (dest_exists && base_name.rfind(".sce_sys") == std::string::npos) {
                    std::filesystem::path meta_path = title_entry.path() / (base_name + ".sce_sys");
                    if (std::filesystem::exists(meta_path, ec)) {
                        if (!verify_checksum(dest, meta_path)) {
                            // Inconsistência detectada entre dados e metadados; restaura backup consistente
                            should_restore = true;
                            std::filesystem::remove_all(dest, ec);
                        }
                    }
                } else if (dest_exists && base_name.rfind(".sce_sys") != std::string::npos) {
                    // Se metadados existem mas param.bin ou checksum.sha256 estão ausentes, metadados estão corrompidos
                    if (!std::filesystem::exists(dest / "param.bin", ec) || !std::filesystem::exists(dest / "checksum.sha256", ec)) {
                        should_restore = true;
                        std::filesystem::remove_all(dest, ec);
                    }
                }

                if (should_restore) {
                    std::filesystem::rename(bak_p, dest, ec);
                    if (ec) {
                        ec.clear();
                        std::filesystem::copy(bak_p, dest, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
                        if (!ec) {
                            std::filesystem::remove_all(bak_p, ec);
                        }
                    }
                } else {
                    std::filesystem::remove_all(bak_p, ec);
                }
            }

            for (const auto &tmp_p : tmp_dirs) {
                std::filesystem::remove_all(tmp_p, ec);
            }
        }
    }
}

bool SaveDataManager::check_work_quota(const std::string &file_path, std::int64_t current_file_size, std::int64_t new_file_size) const {
    if (new_file_size <= current_file_size) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return true;

    std::filesystem::path fp(file_path);
    std::error_code ec;
    std::filesystem::path canonical_fp = std::filesystem::weakly_canonical(fp, ec);
    if (ec) canonical_fp = fp;

    for (const auto &slot : slots_) {
        if (!slot.used || slot.work_path.empty()) continue;
        std::filesystem::path wp(slot.work_path);
        std::filesystem::path canonical_wp = std::filesystem::weakly_canonical(wp, ec);
        if (ec) canonical_wp = wp;

        auto rel = std::filesystem::relative(canonical_fp, canonical_wp, ec);
        if (!ec && !rel.empty() && rel != "." && rel.begin()->string() != ".." && !rel.is_absolute()) {
            std::size_t cur_dir_size = calculate_directory_size(slot.work_path);
            std::uint64_t disk_sz = 0;
            if (std::filesystem::exists(canonical_fp, ec) && !std::filesystem::is_directory(canonical_fp, ec)) {
                disk_sz = std::filesystem::file_size(canonical_fp, ec);
                if (ec) disk_sz = 0;
            }
            std::size_t other_size = (cur_dir_size >= disk_sz) ? (cur_dir_size - disk_sz) : 0;
            std::size_t prospective_total = other_size + static_cast<std::size_t>(new_file_size);
            if (prospective_total > max_quota_per_slot_) {
                return false;
            }
            return true;
        }
    }
    return true;
}

std::string SaveDataManager::resolve_save_dir(int32_t user, const char *title, const char *dir_name) const {
    const std::string t = (title && title[0] != '\0') ? title : default_title_id_;
    std::filesystem::path p = resolve_savedata_root() / std::to_string(user) / t / dir_name;
    return p.string();
}

std::string SaveDataManager::resolve_meta_dir(int32_t user, const char *title, const char *dir_name) const {
    const std::string t = (title && title[0] != '\0') ? title : default_title_id_;
    std::filesystem::path p = resolve_savedata_root() / std::to_string(user) / t / (std::string(dir_name) + ".sce_sys");
    return p.string();
}

bool SaveDataManager::write_meta_param(const std::string &meta_dir, const SceSaveDataParam &param) {
    std::error_code ec;
    std::filesystem::create_directories(meta_dir, ec);
    if (ec) return false;
    std::filesystem::path param_file = std::filesystem::path(meta_dir) / "param.bin";
    std::ofstream out(param_file, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    SceSaveDataParam copy = param;
    if (copy.mtime == 0) {
        copy.mtime = static_cast<int64_t>(std::time(nullptr));
    }
    out.write(reinterpret_cast<const char *>(&copy), sizeof(copy));
    out.close();
    return out.good();
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

    if (!is_confined_to_savedata_root(host_path) || !is_confined_to_savedata_root(meta_path)) {
        return SCE_SAVEDATA_ERROR_PATH_TRAVERSAL;
    }

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
    std::filesystem::path work_path = resolve_savedata_root() / "_work" / ("slot_" + std::to_string(slot_idx));
    std::filesystem::remove_all(work_path, ec);
    std::filesystem::create_directories(work_path, ec);
    if (ec) return SCE_SAVEDATA_ERROR_INTERNAL;

    if (exists) {
        // Validação de integridade SHA-256 persistida (T122)
        if (!verify_checksum(host_path, meta_path)) {
            return SCE_SAVEDATA_ERROR_BROKEN;
        }
        std::filesystem::copy(host_path, work_path,
                              std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            std::filesystem::remove_all(work_path, ec);
            return SCE_SAVEDATA_ERROR_INTERNAL;
        }
        if (sound_hack_enabled_ && !(mode & SCE_SAVEDATA_MOUNT_MODE_RDONLY)) {
            apply_bloodborne_sound_hack(work_path.string());
        }
    } else {
        std::filesystem::create_directories(meta_path, ec);
        SceSaveDataParam empty{};
        std::strncpy(empty.title, dir_name, sizeof(empty.title) - 1);
        if (!write_meta_param(meta_path, empty)) {
            std::filesystem::remove_all(work_path, ec);
            std::filesystem::remove_all(meta_path, ec);
            return SCE_SAVEDATA_ERROR_INTERNAL;
        }
        // Grava checksum inicial para o novo slot
        std::string initial_sha = compute_directory_sha256(host_path);
        if (test_write_failure_phase_ == WriteFailurePhase::Checksum || !write_checksum(meta_path, initial_sha)) {
            std::filesystem::remove_all(work_path, ec);
            std::filesystem::remove_all(meta_path, ec);
            return SCE_SAVEDATA_ERROR_INTERNAL;
        }
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

    if (exists) {
        slot.created_slot = false;
        slot.original_param_exists = read_meta_param(meta_path, slot.original_param);
        std::filesystem::path icon_p = std::filesystem::path(meta_path) / "icon0.png";
        if (std::filesystem::exists(icon_p, ec)) {
            slot.original_icon_exists = true;
            std::ifstream ifile(icon_p, std::ios::binary);
            if (ifile.is_open()) {
                slot.original_icon = std::vector<uint8_t>((std::istreambuf_iterator<char>(ifile)), std::istreambuf_iterator<char>());
            }
        } else {
            slot.original_icon_exists = false;
            slot.original_icon.clear();
        }
    } else {
        slot.created_slot = true;
        slot.original_param_exists = false;
        slot.original_icon_exists = false;
        slot.original_icon.clear();
    }

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

    if (!is_confined_to_savedata_root(slot.host_path) || !is_confined_to_savedata_root(slot.meta_path)) {
        return SCE_SAVEDATA_ERROR_PATH_TRAVERSAL;
    }

    if (slot.read_only || !commit) {
        // Rollback / descarte: apenas desmonta e descarta copia de trabalho
        if (!commit) {
            if (slot.created_slot) {
                std::filesystem::remove_all(slot.meta_path, ec);
            } else {
                if (slot.original_param_exists) {
                    write_meta_param(slot.meta_path, slot.original_param);
                    std::string sha = compute_directory_sha256(slot.host_path);
                    write_checksum(slot.meta_path, sha);
                } else {
                    std::filesystem::remove(std::filesystem::path(slot.meta_path) / "param.bin", ec);
                }
                std::filesystem::path icon_p = std::filesystem::path(slot.meta_path) / "icon0.png";
                if (slot.original_icon_exists) {
                    std::ofstream ofile(icon_p, std::ios::binary | std::ios::trunc);
                    if (ofile.is_open() && !slot.original_icon.empty()) {
                        ofile.write(reinterpret_cast<const char*>(slot.original_icon.data()), static_cast<std::streamsize>(slot.original_icon.size()));
                    }
                } else {
                    std::filesystem::remove(icon_p, ec);
                }
            }
        }
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

    // T111: Backup coordenado de dados e metadados para garantir rollback atômico e recuperável
    std::filesystem::path meta_dir(slot.meta_path);
    auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path meta_bak = meta_dir.parent_path() / (meta_dir.filename().string() + ".bak." + std::to_string(now_ns));
    bool meta_existed = std::filesystem::exists(meta_dir, ec);
    if (ec) return SCE_SAVEDATA_ERROR_INTERNAL;
    if (meta_existed) {
        std::filesystem::copy(meta_dir, meta_bak, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) return SCE_SAVEDATA_ERROR_INTERNAL;
    }

    std::filesystem::path host_dir(slot.host_path);
    std::filesystem::path host_bak = host_dir.parent_path() / (host_dir.filename().string() + ".bak." + std::to_string(now_ns));
    bool host_existed = std::filesystem::exists(host_dir, ec);
    if (ec) return SCE_SAVEDATA_ERROR_INTERNAL;
    if (host_existed) {
        std::filesystem::copy(host_dir, host_bak, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) return SCE_SAVEDATA_ERROR_INTERNAL;
    }

    // Substituicao atomica com rollback em caso de falha
    bool ok = test_write_failure_phase_ != WriteFailurePhase::Checksum &&
              atomic_commit_directory(slot.work_path, slot.host_path);
    if (!ok) {
        // A falha pode ter ocorrido depois de mover o destino para um backup.
        // Nao apagar a unica copia recuperavel antes de verificar o estado em disco.
        // A recuperacao na proxima inicializacao decide se o backup e necessario.
        return SCE_SAVEDATA_ERROR_INTERNAL;
    }

    // Atualiza mtime nos metadados e calcula/persiste checksum SHA-256
    bool meta_ok = true;
    if (test_write_failure_phase_ == WriteFailurePhase::MetadataUpdate) {
        meta_ok = false;
    } else {
        SceSaveDataParam p{};
        if (read_meta_param(slot.meta_path, p)) {
            p.mtime = static_cast<int64_t>(std::time(nullptr));
            if (!write_meta_param(slot.meta_path, p)) meta_ok = false;
        }
        if (meta_ok) {
            std::string sha = compute_directory_sha256(slot.host_path);
            if (!write_checksum(slot.meta_path, sha)) meta_ok = false;
        }
    }

    if (!meta_ok) {
        // T111 / T146: Rollback determinístico de dados e metadados se a atualização dos metadados falhar
        if (host_existed) {
            std::filesystem::remove_all(host_dir, ec);
            std::filesystem::rename(host_bak, host_dir, ec);
            if (ec) {
                ec.clear();
                std::filesystem::copy(host_bak, host_dir, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
                if (!ec) {
                    std::filesystem::remove_all(host_bak, ec);
                }
            }
        } else {
            std::filesystem::remove_all(host_dir, ec);
        }

        if (meta_existed) {
            std::filesystem::remove_all(meta_dir, ec);
            std::filesystem::rename(meta_bak, meta_dir, ec);
            if (ec) {
                ec.clear();
                std::filesystem::copy(meta_bak, meta_dir, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
                if (!ec) {
                    std::filesystem::remove_all(meta_bak, ec);
                }
            }
        } else {
            std::filesystem::remove_all(meta_dir, ec);
        }

        // Keep the staging directory and mounted slot for retry or recovery.
        // A failed metadata/checksum update must not silently erase user changes.
        return SCE_SAVEDATA_ERROR_INTERNAL;
    }

    if (meta_existed) std::filesystem::remove_all(meta_bak, ec);
    if (host_existed) std::filesystem::remove_all(host_bak, ec);

    if (vfs_) {
        vfs_->unmount(slot.mount_point);
    }
    std::filesystem::remove_all(slot.work_path, ec);
    slot = SaveSlotInfo{};
    return SCE_SAVEDATA_OK;
}

bool SaveDataManager::atomic_commit_directory(const std::string &staging_dir, const std::string &dest_dir) {
    if (test_write_failure_phase_ == WriteFailurePhase::TmpWrite) {
        return false;
    }

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

    // Passo 2: Troca atomica (atomic rename) com fallback transacional seguro
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

        if (test_write_failure_phase_ == WriteFailurePhase::Backup) {
            std::error_code rollback_ec;
            std::filesystem::rename(bak_dir, dest, rollback_ec);
            if (rollback_ec) return false; // backup retained for recovery
            std::filesystem::remove_all(temp_dir, rollback_ec);
            return false;
        }

        ec.clear();
        std::filesystem::rename(temp_dir, dest, ec);
        if (ec) {
            // Fallback transacional se rename falhar temporariamente por lock no Windows
            ec.clear();
            std::filesystem::create_directories(dest, ec);
            std::filesystem::copy(temp_dir, dest, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                // Rollback estrito: remove dest parcial e restaura backup
                std::error_code r_ec;
                std::filesystem::remove_all(dest, r_ec);
                std::filesystem::rename(bak_dir, dest, r_ec);
                if (r_ec) {
                    r_ec.clear();
                    std::filesystem::copy(bak_dir, dest, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, r_ec);
                }
                std::filesystem::remove_all(temp_dir, r_ec);
                return false;
            }
        }

        if (test_write_failure_phase_ == WriteFailurePhase::Replacement) {
            std::error_code rollback_ec;
            std::filesystem::remove_all(dest, rollback_ec);
            if (rollback_ec) return false;
            std::filesystem::rename(bak_dir, dest, rollback_ec);
            if (rollback_ec) return false; // backup retained for recovery
            std::filesystem::remove_all(temp_dir, rollback_ec);
            return false;
        }

        // Remocao do backup e do temp apos substituicao concluida com sucesso
        std::filesystem::remove_all(temp_dir, ec);
        std::filesystem::remove_all(bak_dir, ec);
    } else {
        ec.clear();
        std::filesystem::rename(temp_dir, dest, ec);
        if (ec) {
            ec.clear();
            std::filesystem::create_directories(dest, ec);
            std::filesystem::copy(temp_dir, dest, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                std::filesystem::remove_all(dest, ec);
                std::filesystem::remove_all(temp_dir, ec);
                return false;
            }
        }
        std::filesystem::remove_all(temp_dir, ec);
    }

    return true;
}

void SaveDataManager::set_delete_filesystem_for_testing(SaveDeleteFilesystem *filesystem) {
    std::lock_guard<std::mutex> lock(mutex_);
    delete_filesystem_for_testing_ = filesystem;
}

int32_t SaveDataManager::delete_save(int32_t user, const char *title, const char *dir_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return SCE_SAVEDATA_ERROR_NOT_INITIALIZED;
    if (user < 0) return SCE_SAVEDATA_ERROR_INVALID_USER;
    if (!is_valid_dir_name(dir_name)) return SCE_SAVEDATA_ERROR_PARAMETER;
    if (title && !is_valid_title_id(title)) return SCE_SAVEDATA_ERROR_PARAMETER;

    const std::string host_path = resolve_save_dir(user, title, dir_name);
    const std::string meta_path = resolve_meta_dir(user, title, dir_name);

    if (!is_confined_to_savedata_root(host_path) || !is_confined_to_savedata_root(meta_path)) {
        return SCE_SAVEDATA_ERROR_PATH_TRAVERSAL;
    }

    // Nao pode remover slot montado
    for (const auto &slot : slots_) {
        if (slot.used && slot.host_path == host_path) {
            return SCE_SAVEDATA_ERROR_BAD_MOUNTED;
        }
    }

    std::error_code ec;
    auto exists = [&](const std::filesystem::path &path) {
        return delete_filesystem_for_testing_ ? delete_filesystem_for_testing_->exists(path, ec)
                                              : std::filesystem::exists(path, ec);
    };
    auto remove = [&](const std::filesystem::path &path) {
        return delete_filesystem_for_testing_ ? delete_filesystem_for_testing_->remove_all(path, ec)
                                              : std::filesystem::remove_all(path, ec);
    };
    const bool present = exists(host_path);
    if (ec) return SCE_SAVEDATA_ERROR_INTERNAL;
    if (!present) return SCE_SAVEDATA_ERROR_NOT_FOUND;

    // Consulte os dois caminhos antes de alterar qualquer um: erro no metadata
    // nao deve apagar o save principal sem sequer tentar validar seu acesso.
    const bool meta_present = exists(meta_path);
    if (ec) return SCE_SAVEDATA_ERROR_INTERNAL;

    std::filesystem::path host_p(host_path);
    std::filesystem::path parent = host_p.parent_path();
    auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path host_bak = parent / (host_p.filename().string() + ".delbak." + std::to_string(now_ns));

    // T148: Se meta_path estiver presente, cria backup protetivo de host_path para rollback
    // transacional caso a remoção de metadados falhe no meio da operação.
    if (meta_present) {
        std::filesystem::copy(host_p, host_bak, std::filesystem::copy_options::recursive, ec);
        if (ec) {
            ec.clear();
            std::filesystem::remove_all(host_bak, ec);
            return SCE_SAVEDATA_ERROR_INTERNAL;
        }
    }

    remove(host_path);
    if (ec) {
        if (meta_present) {
            std::error_code r_ec;
            std::filesystem::remove_all(host_bak, r_ec);
        }
        return SCE_SAVEDATA_ERROR_INTERNAL;
    }

    if (meta_present) {
        remove(meta_path);
        if (ec) {
            // T148: Falha intermediária na exclusão dos metadados:
            // Rollback transacional para recompor host_path a partir do backup,
            // impedindo que o save sofra perda parcial e seja corrompido!
            std::error_code r_ec;
            std::filesystem::rename(host_bak, host_p, r_ec);
            if (r_ec) {
                r_ec.clear();
                std::filesystem::copy(host_bak, host_p,
                                      std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
                                      r_ec);
                if (!r_ec) {
                    std::filesystem::remove_all(host_bak, r_ec);
                }
            }
            return SCE_SAVEDATA_ERROR_INTERNAL;
        }
        // Exclusão bem-sucedida: limpa o backup protetivo
        std::error_code r_ec;
        std::filesystem::remove_all(host_bak, r_ec);
    }

    return SCE_SAVEDATA_OK;
}

int32_t SaveDataManager::dir_name_search(int32_t user, const char *title, const char *pattern,
                                         uint32_t sort_key, uint32_t sort_order,
                                         std::vector<SaveSearchEntry> &out_entries) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return SCE_SAVEDATA_ERROR_NOT_INITIALIZED;
    if (user < 0) return SCE_SAVEDATA_ERROR_INVALID_USER;

    const std::string t = (title && title[0] != '\0') ? title : default_title_id_;
    if (!is_valid_title_id(t.c_str())) return SCE_SAVEDATA_ERROR_PARAMETER;

    out_entries.clear();
    std::filesystem::path search_base = resolve_savedata_root() / std::to_string(user) / t;
    if (!is_confined_to_savedata_root(search_base)) {
        return SCE_SAVEDATA_ERROR_PARAMETER;
    }

    std::error_code ec;
    if (!std::filesystem::exists(search_base, ec) || !std::filesystem::is_directory(search_base, ec)) {
        return SCE_SAVEDATA_OK;
    }

    std::string pat = (pattern && pattern[0] != '\0') ? pattern : "%";

    for (const auto &entry : std::filesystem::directory_iterator(search_base, ec)) {
        if (!entry.is_directory(ec)) continue;
        if (entry.is_symlink(ec)) continue;
        if (!is_confined_to_savedata_root(entry.path())) continue;

        std::string filename = entry.path().filename().string();
        if (filename.empty() || filename[0] == '.') continue;
        if (filename.size() > 8 && filename.rfind(".sce_sys") == filename.size() - 8) continue;
        if (filename.rfind(".tmp") != std::string::npos || filename.rfind(".bak") != std::string::npos) continue;

        if (!pattern_matches(filename, pat)) continue;

        std::filesystem::path meta_dir = entry.path().string() + ".sce_sys";
        if (std::filesystem::is_symlink(meta_dir, ec) || !is_confined_to_savedata_root(meta_dir)) {
            continue;
        }

        SaveSearchEntry se{};
        se.dir_name = filename;
        read_meta_param(meta_dir.string(), se.param);
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
    if (!buf || size == 0 || size > 65536) return SCE_SAVEDATA_ERROR_PARAMETER;
    int slot_idx = find_slot(mount_point);
    if (slot_idx < 0) return SCE_SAVEDATA_ERROR_NOT_MOUNTED;

    SaveSlotInfo &slot = slots_[slot_idx];
    if (slot.read_only) return SCE_SAVEDATA_ERROR_PARAMETER;
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

    return write_meta_param(slot.meta_path, p) ? SCE_SAVEDATA_OK : SCE_SAVEDATA_ERROR_INTERNAL;
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
    constexpr std::size_t kMaxIconSize = 2 * 1024 * 1024;
    if (!data || size == 0 || size > kMaxIconSize) return SCE_SAVEDATA_ERROR_PARAMETER;
    int slot_idx = find_slot(mount_point);
    if (slot_idx < 0) return SCE_SAVEDATA_ERROR_NOT_MOUNTED;

    const SaveSlotInfo &slot = slots_[slot_idx];
    if (slot.read_only) return SCE_SAVEDATA_ERROR_PARAMETER;
    std::error_code ec;
    std::filesystem::create_directories(slot.meta_path, ec);
    if (ec) return SCE_SAVEDATA_ERROR_INTERNAL;

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

    // T156: Confinar canonicamente e validar title_id
    if (!is_valid_title_id(default_title_id_.c_str())) {
        return SCE_SAVEDATA_ERROR_PARAMETER;
    }

    std::filesystem::path mem_dir = resolve_savedata_root() / std::to_string(user) / (default_title_id_ + ".memory");
    std::filesystem::path mem_file = mem_dir / "memory.dat";

    if (!is_confined_to_savedata_root(mem_dir) || !is_confined_to_savedata_root(mem_file)) {
        return SCE_SAVEDATA_ERROR_PARAMETER;
    }

    std::error_code ec;
    std::filesystem::create_directories(mem_dir, ec);
    if (ec) return SCE_SAVEDATA_ERROR_INTERNAL;

    std::string mem_path = mem_file.string();
    bool exists = std::filesystem::exists(mem_file, ec);
    if (ec) return SCE_SAVEDATA_ERROR_INTERNAL;

    // T147: Impedir truncamento destrutivo de memory.dat preexistente
    if (!exists) {
        std::ofstream f(mem_path, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return SCE_SAVEDATA_ERROR_INTERNAL;
        std::vector<char> zero(static_cast<std::size_t>(size), 0);
        f.write(zero.data(), zero.size());
        if (!f.good()) return SCE_SAVEDATA_ERROR_INTERNAL;
        f.close();
    } else {
        auto fsz = std::filesystem::file_size(mem_file, ec);
        if (ec) return SCE_SAVEDATA_ERROR_INTERNAL;
        if (fsz < size) {
            std::fstream f(mem_path, std::ios::in | std::ios::out | std::ios::binary);
            if (!f.is_open()) return SCE_SAVEDATA_ERROR_INTERNAL;
            f.seekp(0, std::ios::end);
            std::size_t diff = static_cast<std::size_t>(size - fsz);
            std::vector<char> zero(diff, 0);
            f.write(zero.data(), zero.size());
            if (!f.good()) return SCE_SAVEDATA_ERROR_INTERNAL;
            f.close();
        } else if (fsz > size) {
            std::filesystem::resize_file(mem_file, size, ec);
            if (ec) return SCE_SAVEDATA_ERROR_INTERNAL;
        }
    }

    user_memories_[user] = UserMemoryState{mem_path, size};

    if (param) {
        if (!write_meta_param(mem_dir.string(), *param)) return SCE_SAVEDATA_ERROR_INTERNAL;
    }
    return SCE_SAVEDATA_OK;
}

int32_t SaveDataManager::get_memory(int32_t user, void *buffer, uint64_t size, int64_t offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return SCE_SAVEDATA_ERROR_NOT_INITIALIZED;
    if (user < 0) return SCE_SAVEDATA_ERROR_INVALID_USER;
    if (!buffer || offset < 0) return SCE_SAVEDATA_ERROR_PARAMETER;
    if (size > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        return SCE_SAVEDATA_ERROR_PARAMETER;
    }

    // T156: Validar title_id
    if (!is_valid_title_id(default_title_id_.c_str())) {
        return SCE_SAVEDATA_ERROR_PARAMETER;
    }

    auto it = user_memories_.find(user);
    if (it == user_memories_.end()) {
        std::filesystem::path mem_dir = resolve_savedata_root() / std::to_string(user) / (default_title_id_ + ".memory");
        std::filesystem::path mem_file = mem_dir / "memory.dat";
        if (!is_confined_to_savedata_root(mem_dir) || !is_confined_to_savedata_root(mem_file)) {
            return SCE_SAVEDATA_ERROR_PARAMETER;
        }
        std::error_code ec;
        if (std::filesystem::exists(mem_file, ec)) {
            auto fsz = std::filesystem::file_size(mem_file, ec);
            if (!ec && fsz > 0) {
                it = user_memories_.emplace(user, UserMemoryState{mem_file.string(), static_cast<uint64_t>(fsz)}).first;
            }
        }
    }
    if (it == user_memories_.end() || it->second.size == 0 || it->second.path.empty()) {
        return SCE_SAVEDATA_ERROR_MEMORY_NOT_READY;
    }
    if (!is_confined_to_savedata_root(it->second.path)) {
        return SCE_SAVEDATA_ERROR_PARAMETER;
    }
    const uint64_t u_offset = static_cast<uint64_t>(offset);
    if (u_offset > it->second.size || size > (it->second.size - u_offset)) {
        return SCE_SAVEDATA_ERROR_PARAMETER;
    }
    if (size == 0) return SCE_SAVEDATA_OK;

    std::ifstream f(it->second.path, std::ios::binary);
    if (!f.is_open()) return SCE_SAVEDATA_ERROR_INTERNAL;
    f.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!f.read(reinterpret_cast<char *>(buffer), static_cast<std::streamsize>(size))) {
        return SCE_SAVEDATA_ERROR_INTERNAL;
    }
    return SCE_SAVEDATA_OK;
}

int32_t SaveDataManager::set_memory(int32_t user, const void *buffer, uint64_t size, int64_t offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return SCE_SAVEDATA_ERROR_NOT_INITIALIZED;
    if (user < 0) return SCE_SAVEDATA_ERROR_INVALID_USER;
    if (!buffer || offset < 0) return SCE_SAVEDATA_ERROR_PARAMETER;
    if (size > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        return SCE_SAVEDATA_ERROR_PARAMETER;
    }

    // T156: Validar title_id
    if (!is_valid_title_id(default_title_id_.c_str())) {
        return SCE_SAVEDATA_ERROR_PARAMETER;
    }

    auto it = user_memories_.find(user);
    if (it == user_memories_.end()) {
        std::filesystem::path mem_dir = resolve_savedata_root() / std::to_string(user) / (default_title_id_ + ".memory");
        std::filesystem::path mem_file = mem_dir / "memory.dat";
        if (!is_confined_to_savedata_root(mem_dir) || !is_confined_to_savedata_root(mem_file)) {
            return SCE_SAVEDATA_ERROR_PARAMETER;
        }
        std::error_code ec;
        if (std::filesystem::exists(mem_file, ec)) {
            auto fsz = std::filesystem::file_size(mem_file, ec);
            if (!ec && fsz > 0) {
                it = user_memories_.emplace(user, UserMemoryState{mem_file.string(), static_cast<uint64_t>(fsz)}).first;
            }
        }
    }
    if (it == user_memories_.end() || it->second.size == 0 || it->second.path.empty()) {
        return SCE_SAVEDATA_ERROR_MEMORY_NOT_READY;
    }
    if (!is_confined_to_savedata_root(it->second.path)) {
        return SCE_SAVEDATA_ERROR_PARAMETER;
    }
    const uint64_t u_offset = static_cast<uint64_t>(offset);
    if (u_offset > it->second.size || size > (it->second.size - u_offset)) {
        return SCE_SAVEDATA_ERROR_PARAMETER;
    }
    if (size == 0) return SCE_SAVEDATA_OK;

    std::fstream f(it->second.path, std::ios::in | std::ios::out | std::ios::binary);
    if (!f.is_open()) return SCE_SAVEDATA_ERROR_INTERNAL;
    f.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!f.write(reinterpret_cast<const char *>(buffer), static_cast<std::streamsize>(size))) {
        return SCE_SAVEDATA_ERROR_INTERNAL;
    }
    f.flush();
    return f.good() ? SCE_SAVEDATA_OK : SCE_SAVEDATA_ERROR_INTERNAL;
}

static bool is_confined_to_work_path(const std::string &work_path_str, const std::string &relative_path, std::filesystem::path &out_target) {
    if (relative_path.empty() || SaveDataManager::contains_traversal(relative_path)) {
        return false;
    }
    std::filesystem::path rel_p(relative_path);
    if (rel_p.is_absolute() || rel_p.has_root_name() || rel_p.has_root_directory()) {
        return false;
    }

    std::error_code ec;
    std::filesystem::path work = std::filesystem::weakly_canonical(std::filesystem::path(work_path_str), ec);
    if (ec || work.empty()) return false;

    std::filesystem::path target = std::filesystem::path(work_path_str) / relative_path;
    std::filesystem::path canonical_target = std::filesystem::weakly_canonical(target, ec);
    if (ec || canonical_target.empty()) return false;

    if (std::filesystem::is_symlink(target, ec)) {
        std::filesystem::path sym_dest = std::filesystem::canonical(target, ec);
        if (ec) return false;
        auto sym_rel = std::filesystem::relative(sym_dest, work, ec);
        auto first_sym = sym_rel.begin() != sym_rel.end() ? sym_rel.begin()->string() : "";
        if (ec || sym_rel.empty() || sym_rel == "." || first_sym == ".." || sym_rel.is_absolute()) {
            return false;
        }
    }

    auto rel = std::filesystem::relative(canonical_target, work, ec);
    auto first_elem = rel.begin() != rel.end() ? rel.begin()->string() : "";
    if (ec || rel.empty() || rel == "." || first_elem == ".." || rel.is_absolute()) {
        return false;
    }
    out_target = target;
    return true;
}

int32_t SaveDataManager::write_file(const std::string &mount_point, const std::string &relative_path,
                                    const void *data, std::size_t size, bool atomic) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return SCE_SAVEDATA_ERROR_NOT_INITIALIZED;
    int slot_idx = find_slot(mount_point);
    if (slot_idx < 0) return SCE_SAVEDATA_ERROR_NOT_MOUNTED;
    if (slots_[slot_idx].read_only) return SCE_SAVEDATA_ERROR_PARAMETER;
    // Rejeitar antes de tocar o filesystem: nullptr com bytes a gravar não é arquivo vazio.
    if (size > 0 && data == nullptr) return SCE_SAVEDATA_ERROR_PARAMETER;

    std::filesystem::path target;
    if (!is_confined_to_work_path(slots_[slot_idx].work_path, relative_path, target)) {
        return SCE_SAVEDATA_ERROR_PARAMETER;
    }
    std::error_code ec;
    std::size_t old_size = 0;
    bool exists = std::filesystem::exists(target, ec);
    if (ec) return SCE_SAVEDATA_ERROR_INTERNAL;
    if (exists) {
        auto fsz = std::filesystem::file_size(target, ec);
        if (ec) return SCE_SAVEDATA_ERROR_INTERNAL;
        old_size = static_cast<std::size_t>(fsz);
    }
    std::size_t current_total = calculate_directory_size(slots_[slot_idx].work_path);
    // T178: Calcular quota sem underflow (current_total < old_size) ou overflow (net_total + size)
    std::size_t net_total = (current_total >= old_size) ? (current_total - old_size) : 0;
    if (size > max_quota_per_slot_ || net_total > max_quota_per_slot_ - size) {
        return SCE_SAVEDATA_ERROR_QUOTA_EXCEEDED;
    }

    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), ec);
    }

    if (atomic) {
        // Protocolo de gravação em 4 fases: tmp -> checksum -> backup -> rename atômico (T025, T042)
        if (test_write_failure_phase_ == WriteFailurePhase::TmpWrite) {
            return SCE_SAVEDATA_ERROR_INTERNAL;
        }

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

        // Fase 2: Verificação de integridade dos dados gravados em blocos (T162)
        if (test_write_failure_phase_ == WriteFailurePhase::Checksum) {
            std::filesystem::remove(tmp, ec);
            return SCE_SAVEDATA_ERROR_INTERNAL;
        }
        if (size > 0 && data) {
            std::ifstream in(tmp, std::ios::binary);
            if (!in.is_open()) {
                std::filesystem::remove(tmp, ec);
                return SCE_SAVEDATA_ERROR_INTERNAL;
            }
            // T162: Verificação em blocos fixos na stack sem alocação proporcional à quota
            constexpr std::size_t VERIFY_CHUNK_SIZE = 64 * 1024;
            std::array<uint8_t, VERIFY_CHUNK_SIZE> chunk;
            std::size_t bytes_verified = 0;
            const auto *orig_bytes = static_cast<const uint8_t *>(data);

            while (bytes_verified < size) {
                std::size_t to_read = std::min(VERIFY_CHUNK_SIZE, size - bytes_verified);
                in.read(reinterpret_cast<char *>(chunk.data()), static_cast<std::streamsize>(to_read));
                std::streamsize got = in.gcount();
                if (got != static_cast<std::streamsize>(to_read)) {
                    std::filesystem::remove(tmp, ec);
                    return SCE_SAVEDATA_ERROR_INTERNAL;
                }
                if (std::memcmp(chunk.data(), orig_bytes + bytes_verified, to_read) != 0) {
                    std::filesystem::remove(tmp, ec);
                    return SCE_SAVEDATA_ERROR_INTERNAL;
                }
                bytes_verified += to_read;
            }
        }

        // Fase 3: Backup do arquivo de destino existente para .bak
        bool target_existed = std::filesystem::exists(target, ec);
        if (target_existed) {
            if (test_write_failure_phase_ == WriteFailurePhase::Backup) {
                std::filesystem::remove(tmp, ec);
                return SCE_SAVEDATA_ERROR_INTERNAL;
            }
            std::filesystem::rename(target, bak, ec);
            if (ec) {
                std::filesystem::copy_file(target, bak, std::filesystem::copy_options::overwrite_existing, ec);
                if (ec) {
                    std::filesystem::remove(tmp, ec);
                    return SCE_SAVEDATA_ERROR_INTERNAL;
                }
            }
        }

        // Fase 4: Rename atômico de .tmp para target com garantia de rollback
        if (test_write_failure_phase_ == WriteFailurePhase::Replacement) {
            if (target_existed) {
                std::error_code roll_ec;
                std::filesystem::rename(bak, target, roll_ec);
                if (roll_ec) {
                    std::filesystem::copy_file(bak, target, std::filesystem::copy_options::overwrite_existing, roll_ec);
                }
            }
            std::filesystem::remove(tmp, ec);
            std::filesystem::remove(bak, ec);
            return SCE_SAVEDATA_ERROR_INTERNAL;
        }

        std::filesystem::rename(tmp, target, ec);
        if (ec) {
            if (target_existed) {
                std::error_code roll_ec;
                std::filesystem::rename(bak, target, roll_ec);
                if (roll_ec) {
                    std::filesystem::copy_file(bak, target, std::filesystem::copy_options::overwrite_existing, roll_ec);
                }
            }
            std::filesystem::remove(tmp, ec);
            std::filesystem::remove(bak, ec);
            return SCE_SAVEDATA_ERROR_INTERNAL;
        }

        // Limpeza do backup após rename com sucesso
        if (target_existed) {
            std::filesystem::remove(bak, ec);
        }
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

    std::filesystem::path target;
    if (!is_confined_to_work_path(slots_[slot_idx].work_path, relative_path, target)) {
        return SCE_SAVEDATA_ERROR_PARAMETER;
    }

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

    std::filesystem::path target;
    if (!is_confined_to_work_path(slots_[slot_idx].work_path, relative_path, target)) {
        return SCE_SAVEDATA_ERROR_PARAMETER;
    }

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

    std::filesystem::path target;
    if (!is_confined_to_work_path(slots_[slot_idx].work_path, relative_path, target)) {
        return false;
    }

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

void SaveDataManager::set_sound_hack_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    sound_hack_enabled_ = enabled;
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

std::string SaveDataManager::compute_directory_sha256(const std::filesystem::path &dir_path) {
    std::error_code ec;
    if (!std::filesystem::exists(dir_path, ec) || !std::filesystem::is_directory(dir_path, ec)) {
        Sha256 hasher;
        return hasher.finalize();
    }

    std::vector<std::filesystem::path> paths;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(dir_path, ec)) {
        if (entry.is_regular_file(ec)) {
            paths.push_back(entry.path());
        }
    }
    std::sort(paths.begin(), paths.end());

    Sha256 hasher;
    for (const auto &p : paths) {
        std::string rel = std::filesystem::relative(p, dir_path, ec).generic_string();
        hasher.update(rel.data(), rel.size());
        std::ifstream in(p, std::ios::binary);
        if (in.is_open()) {
            char buf[4096];
            while (in.read(buf, sizeof(buf))) {
                hasher.update(buf, in.gcount());
            }
            if (in.gcount() > 0) {
                hasher.update(buf, in.gcount());
            }
        }
    }
    return hasher.finalize();
}

bool SaveDataManager::write_checksum(const std::filesystem::path &meta_dir, const std::string &checksum) {
    std::error_code ec;
    std::filesystem::create_directories(meta_dir, ec);
    if (ec) return false;
    std::filesystem::path chk_file = meta_dir / "checksum.sha256";
    std::ofstream out(chk_file, std::ios::trunc);
    if (!out.is_open()) return false;
    out << checksum << "\n";
    return out.good();
}

bool SaveDataManager::verify_checksum(const std::filesystem::path &host_dir, const std::filesystem::path &meta_dir) {
    std::filesystem::path chk_file = meta_dir / "checksum.sha256";
    std::error_code ec;
    if (!std::filesystem::exists(chk_file, ec)) {
        return true;
    }
    std::ifstream in(chk_file);
    if (!in.is_open()) return false;
    std::string expected;
    in >> expected;
    if (expected.empty()) return true;

    std::string actual = compute_directory_sha256(host_dir);
    return actual == expected;
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
