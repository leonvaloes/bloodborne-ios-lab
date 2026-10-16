#include "game_package.h"

#include <filesystem>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <iomanip>
#include <unordered_set>

namespace bb {

const char *package_type_to_string(PackageType type) {
    switch (type) {
        case PackageType::base: return "base";
        case PackageType::patch: return "patch";
        case PackageType::dlc: return "dlc";
        case PackageType::unknown:
        default:
            return "unknown";
    }
}

GamePackageManager::GamePackageManager() = default;

GamePackageManager::~GamePackageManager() {
    clear();
}

void GamePackageManager::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &pkg : packages_) {
        if (pkg.reader) {
            pkg.reader->close();
            pkg.reader.reset();
        }
    }
    packages_.clear();
}

PackageType GamePackageManager::detect_package_type(const std::string &package_name) {
    std::string lower;
    lower.reserve(package_name.size());
    for (char c : package_name) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    if (lower.find("patch") != std::string::npos || lower.find("update") != std::string::npos) {
        return PackageType::patch;
    }
    if (lower.find("dlc") != std::string::npos || lower.find("expansion") != std::string::npos) {
        return PackageType::dlc;
    }
    if (lower.find("data") != std::string::npos || lower.find("base") != std::string::npos) {
        return PackageType::base;
    }
    return PackageType::base;
}

int GamePackageManager::default_priority_for(PackageType type, const std::string &package_name) {
    int numeric_suffix = 0;
    std::string num_str;
    for (char c : package_name) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            num_str.push_back(c);
        }
    }
    if (!num_str.empty()) {
        try {
            numeric_suffix = std::stoi(num_str);
        } catch (...) {
            numeric_suffix = 0;
        }
    }

    switch (type) {
        case PackageType::patch:
            return 200 + numeric_suffix;
        case PackageType::dlc:
            return 100 + numeric_suffix;
        case PackageType::base:
            return 10 + numeric_suffix;
        case PackageType::unknown:
        default:
            return numeric_suffix;
    }
}

void GamePackageManager::sort_packages_locked() {
    std::stable_sort(packages_.begin(), packages_.end(),
        [](const PackageInfo &a, const PackageInfo &b) {
            return a.priority > b.priority;
        });
}

bool GamePackageManager::add_package(const std::string &bhd_path, const std::string &bdt_path,
                                     PackageType type, int priority) {
    if (bhd_path.empty()) return false;
    std::error_code ec;
    std::filesystem::path bhd_p = std::filesystem::u8path(bhd_path);
    if (!std::filesystem::is_regular_file(bhd_p, ec)) return false;

    std::string stem = bhd_p.stem().string();
    std::filesystem::path bdt_p;
    if (!bdt_path.empty()) {
        bdt_p = std::filesystem::u8path(bdt_path);
    } else {
        // Procura pelo .bdt correspondente no mesmo diretório
        bdt_p = bhd_p.parent_path() / (stem + ".bdt");
        if (!std::filesystem::is_regular_file(bdt_p, ec)) {
            bdt_p = bhd_p.parent_path() / (stem + ".BDT");
        }
        if (!std::filesystem::is_regular_file(bdt_p, ec)) {
            // Busca case-insensitive no diretório pai
            for (const auto &entry : std::filesystem::directory_iterator(bhd_p.parent_path(), ec)) {
                if (entry.is_regular_file(ec)) {
                    std::string entry_stem = entry.path().stem().string();
                    std::string entry_ext = entry.path().extension().string();
                    if (entry_ext.size() == 4 &&
                        std::tolower(static_cast<unsigned char>(entry_ext[1])) == 'b' &&
                        std::tolower(static_cast<unsigned char>(entry_ext[2])) == 'd' &&
                        std::tolower(static_cast<unsigned char>(entry_ext[3])) == 't') {
                        if (entry_stem.size() == stem.size() &&
                            std::equal(entry_stem.begin(), entry_stem.end(), stem.begin(),
                                       [](char a, char b) {
                                           return std::tolower(static_cast<unsigned char>(a)) ==
                                                  std::tolower(static_cast<unsigned char>(b));
                                       })) {
                            bdt_p = entry.path();
                            break;
                        }
                    }
                }
            }
        }
    }

    if (!std::filesystem::is_regular_file(bdt_p, ec)) {
        return false;
    }

    std::string canon_bhd = std::filesystem::absolute(bhd_p, ec).string();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &pkg : packages_) {
            if (pkg.bhd_path == canon_bhd || pkg.name == stem) {
                return false;
            }
        }
    }

    auto reader = std::make_shared<Bhd5Reader>();
    BinderError b_err = reader->open(bhd_p.string(), bdt_p.string());
    if (b_err != BinderError::ok) {
        return false;
    }

    if (type == PackageType::unknown) {
        type = detect_package_type(stem);
    }
    if (priority < 0) {
        priority = default_priority_for(type, stem);
    }

    std::uint64_t bdt_sz = std::filesystem::file_size(bdt_p, ec);
    if (ec) bdt_sz = 0;
    std::uint64_t bhd_sz = std::filesystem::file_size(bhd_p, ec);
    if (ec) bhd_sz = 0;

    PackageInfo info;
    info.name = stem;
    info.type = type;
    info.priority = priority;
    info.bhd_path = canon_bhd;
    info.bdt_path = std::filesystem::absolute(bdt_p, ec).string();
    info.bdt_size = bdt_sz;
    info.bhd_size = bhd_sz;
    info.file_count = reader->file_count();
    info.reader = std::move(reader);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        packages_.push_back(std::move(info));
        sort_packages_locked();
    }
    return true;
}

std::size_t GamePackageManager::scan_directory(const std::string &host_dir, bool recursive) {
    if (host_dir.empty()) return 0;
    std::error_code ec;
    std::filesystem::path root = std::filesystem::u8path(host_dir);
    if (!std::filesystem::is_directory(root, ec)) return 0;

    std::size_t added_count = 0;
    auto check_file = [&](const std::filesystem::directory_entry &entry) {
        if (!entry.is_regular_file(ec)) return;
        std::string ext = entry.path().extension().string();
        for (char &c : ext) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (ext == ".bhd5" || ext == ".bhd") {
            if (add_package(entry.path().string())) {
                added_count++;
            }
        }
    };

    if (recursive) {
        for (const auto &entry : std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec)) {
            check_file(entry);
        }
    } else {
        for (const auto &entry : std::filesystem::directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec)) {
            check_file(entry);
        }
        if (added_count == 0) {
            const char *candidates[] = {
                "dvdroot_ps4",
                "Image0/dvdroot_ps4",
                "app0/dvdroot_ps4",
                "dvdroot"
            };
            for (const char *cand : candidates) {
                std::filesystem::path sub = root / cand;
                if (std::filesystem::is_directory(sub, ec)) {
                    for (const auto &entry : std::filesystem::directory_iterator(sub, std::filesystem::directory_options::skip_permission_denied, ec)) {
                        check_file(entry);
                    }
                    if (added_count > 0) break;
                }
            }
        }
    }
    return added_count;
}

std::size_t GamePackageManager::package_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return packages_.size();
}

std::vector<PackageInfo> GamePackageManager::packages() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return packages_;
}

bool GamePackageManager::has_package(const std::string &name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &pkg : packages_) {
        if (pkg.name == name) return true;
    }
    return false;
}

bool GamePackageManager::get_package_info(const std::string &name, PackageInfo &out_info) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &pkg : packages_) {
        if (pkg.name == name) {
            out_info = pkg;
            return true;
        }
    }
    return false;
}

const PackageInfo *GamePackageManager::get_package(const std::string &name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &pkg : packages_) {
        if (pkg.name == name) return &pkg;
    }
    return nullptr;
}

std::size_t GamePackageManager::total_files_indexed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t total = 0;
    for (const auto &pkg : packages_) {
        total += pkg.file_count;
    }
    return total;
}

std::size_t GamePackageManager::unique_files_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::unordered_set<std::uint64_t> unique_hashes;
    for (const auto &pkg : packages_) {
        if (pkg.reader) {
            for (const auto &entry : pkg.reader->entries()) {
                unique_hashes.insert(entry.hash);
            }
        }
    }
    return unique_hashes.size();
}

std::uint64_t GamePackageManager::total_bdt_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint64_t total = 0;
    for (const auto &pkg : packages_) {
        total += pkg.bdt_size;
    }
    return total;
}

std::uint64_t GamePackageManager::total_bhd_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint64_t total = 0;
    for (const auto &pkg : packages_) {
        total += pkg.bhd_size;
    }
    return total;
}

bool GamePackageManager::has_file(std::uint64_t hash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &pkg : packages_) {
        if (pkg.reader && pkg.reader->has_file(hash)) return true;
    }
    return false;
}

bool GamePackageManager::has_file(const std::string &path) const {
    return has_file(Bhd5Reader::hash_path(path));
}

const Bhd5FileEntry *GamePackageManager::find_entry(std::uint64_t hash, std::string *out_package_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &pkg : packages_) {
        if (pkg.reader) {
            const auto *entry = pkg.reader->get_entry(hash);
            if (entry) {
                if (out_package_name) *out_package_name = pkg.name;
                return entry;
            }
        }
    }
    return nullptr;
}

const Bhd5FileEntry *GamePackageManager::find_entry(const std::string &path, std::string *out_package_name) const {
    return find_entry(Bhd5Reader::hash_path(path), out_package_name);
}

BinderError GamePackageManager::read_file(std::uint64_t hash, std::vector<std::uint8_t> &out_data) const {
    std::shared_ptr<Bhd5Reader> reader;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &pkg : packages_) {
            if (pkg.reader && pkg.reader->has_file(hash)) {
                reader = pkg.reader;
                break;
            }
        }
    }
    if (!reader) return BinderError::file_not_found;
    return reader->read_file(hash, out_data);
}

BinderError GamePackageManager::read_file(const std::string &path, std::vector<std::uint8_t> &out_data) const {
    return read_file(Bhd5Reader::hash_path(path), out_data);
}

BinderError GamePackageManager::stream_file(std::uint64_t hash, std::uint64_t offset,
                                            void *dest, std::size_t bytes_to_read, std::size_t &out_bytes_read) {
    std::shared_ptr<Bhd5Reader> reader;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &pkg : packages_) {
            if (pkg.reader && pkg.reader->has_file(hash)) {
                reader = pkg.reader;
                break;
            }
        }
    }
    if (!reader) {
        out_bytes_read = 0;
        return BinderError::file_not_found;
    }
    return reader->stream_file(hash, offset, dest, bytes_to_read, out_bytes_read);
}

BinderError GamePackageManager::stream_file(const std::string &path, std::uint64_t offset,
                                            void *dest, std::size_t bytes_to_read, std::size_t &out_bytes_read) {
    return stream_file(Bhd5Reader::hash_path(path), offset, dest, bytes_to_read, out_bytes_read);
}

bool GamePackageManager::mount_all(VirtualFileSystem &vfs, const std::vector<std::string> &mount_points) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (packages_.empty()) return false;
    bool any_mounted = false;
    for (const auto &pkg : packages_) {
        if (!pkg.reader || !pkg.reader->is_open()) continue;
        for (const auto &mp : mount_points) {
            if (vfs.mount_bhd5(mp, pkg.reader, pkg.priority)) {
                any_mounted = true;
            }
        }
    }
    return any_mounted;
}

bool GamePackageManager::unmount_all(VirtualFileSystem &vfs, const std::vector<std::string> &mount_points) {
    bool any_unmounted = false;
    for (const auto &mp : mount_points) {
        if (vfs.unmount(mp)) {
            any_unmounted = true;
        }
    }
    return any_unmounted;
}

std::vector<std::string> GamePackageManager::default_essential_paths() {
    return {
        "param/gameparam.parambnd.dcx",
        "param/drawparam/drawparam.parambnd.dcx",
        "msg/engus/item.msgbnd.dcx",
        "msg/engus/menu.msgbnd.dcx",
        "map/m10_00_00_00.msb",
        "event/common.emevd.dcx"
    };
}

PackageInspectionReport GamePackageManager::verify_essential_assets(const std::vector<std::string> &essential_paths) const {
    const auto &paths = essential_paths.empty() ? default_essential_paths() : essential_paths;
    std::vector<std::pair<std::string, std::uint64_t>> items;
    items.reserve(paths.size());
    for (const auto &p : paths) {
        items.emplace_back(p, Bhd5Reader::hash_path(p));
    }
    return verify_essential_hashes(items);
}

PackageInspectionReport GamePackageManager::verify_essential_hashes(const std::vector<std::pair<std::string, std::uint64_t>> &essential_items) const {
    PackageInspectionReport report;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        report.package_count = packages_.size();
        for (const auto &pkg : packages_) {
            report.packages_loaded.push_back(pkg.name);
        }
    }
    report.total_files_indexed = total_files_indexed();
    report.unique_files_count = unique_files_count();
    report.total_bdt_size = total_bdt_size();
    report.total_bhd_size = total_bhd_size();

    for (const auto &item : essential_items) {
        const std::string &label = item.first;
        std::uint64_t hash = item.second;
        std::string resolver_pkg;
        const auto *entry = find_entry(hash, &resolver_pkg);
        if (entry) {
            report.essential_files_found.push_back(label);
            report.essential_file_package_map[label] = resolver_pkg;
        } else {
            report.essential_files_missing.push_back(label);
        }
    }

    report.valid = (report.package_count > 0 && report.essential_files_missing.empty());

    std::ostringstream ss;
    if (report.valid) {
        ss << "[OK] Inspecao de pacotes valida: " << report.package_count << " pacote(s) carregado(s) ("
           << report.total_files_indexed << " arquivos catalogados, "
           << report.unique_files_count << " unicos, "
           << report.total_bdt_size << " bytes BDT total). "
           << "Todos os " << report.essential_files_found.size() << " ativos essenciais foram verificados com sucesso.";
    } else {
        ss << "[FALHA] Inspecao de pacotes incompleta: ";
        if (report.package_count == 0) {
            ss << "Nenhum contêiner BHD5/BDT foi carregado.";
        } else {
            ss << report.package_count << " pacote(s) carregado(s), mas "
               << report.essential_files_missing.size() << " ativo(s) essencial(is) estao ausentes: [";
            for (std::size_t i = 0; i < report.essential_files_missing.size(); ++i) {
                if (i > 0) ss << ", ";
                ss << report.essential_files_missing[i];
            }
            ss << "].";
        }
    }
    report.diagnostic_summary = ss.str();
    return report;
}

void GamePackageManager::synthesize_defensive_stubs(VirtualFileSystem &vfs) {
    vfs.register_shader_gui_stubs();
}

} // namespace bb
