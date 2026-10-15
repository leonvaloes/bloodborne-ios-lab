#include "game_boot_coordinator.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <iomanip>
#include <stdexcept>

namespace bb {

namespace {

std::string hex_val(std::uint64_t val) {
    std::ostringstream ss;
    ss << std::hex << val;
    return ss.str();
}

std::vector<std::uint8_t> read_file_data(const std::filesystem::path& path, std::size_t max_size = (512ull << 20)) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() < 0) {
        throw std::runtime_error("Arquivo ausente ou inacessivel: " + path.string());
    }
    const auto file_len = static_cast<std::uint64_t>(file.tellg());
    if (file_len > max_size) {
        throw std::runtime_error("Arquivo excede limite permitido: " + path.string());
    }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(file_len));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!file) {
        throw std::runtime_error("Falha ao ler conteudo completo de " + path.string());
    }
    return data;
}

} // namespace

const char* boot_state_to_string(BootState state) {
    switch (state) {
        case BootState::not_started: return "not_started";
        case BootState::packages_mounted: return "packages_mounted";
        case BootState::modules_initialized: return "modules_initialized";
        case BootState::running: return "running";
        case BootState::halted: return "halted";
        case BootState::error: return "error";
    }
    return "unknown";
}

std::string BootDiagnostics::summary() const {
    std::ostringstream ss;
    ss << "=== GameBootCoordinator Diagnostics ===\n";
    ss << "State: " << state_name << " (" << static_cast<int>(state) << ")\n";
    ss << "Total Steps Executed: " << total_steps << "\n";
    ss << "Total Imports Called: " << total_import_calls << "\n";
    if (!last_import_called.empty()) {
        ss << "Last Import: " << last_import_called << "\n";
    }
    ss << "Active Modules: " << modules_count;
    if (!active_modules.empty()) {
        ss << " (";
        for (std::size_t i = 0; i < active_modules.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << active_modules[i];
        }
        ss << ")";
    }
    ss << "\n";
    ss << "Packages Valid: " << (assets_valid ? "YES" : "NO")
       << " (" << package_report.package_count << " packages, "
       << package_report.total_files_indexed << " files)\n";
    if (!package_report.diagnostic_summary.empty()) {
        ss << "  " << package_report.diagnostic_summary << "\n";
    }
    ss << "Save System: " << (save_initialized ? "Initialized" : "Not Initialized");
    if (!save_mount_point.empty()) {
        ss << " (Mount: " << save_mount_point << ")";
    }
    ss << "\n";
    ss << "Audio Ports Open: " << open_audio_ports << "\n";
    if (entry_point != 0) {
        ss << "Entry Point: 0x" << std::hex << entry_point << std::dec << "\n";
    }
    if (tcb_address != 0) {
        ss << "TCB Address: 0x" << std::hex << tcb_address << std::dec << "\n";
    }
    if (current_rip != 0) {
        ss << "Current RIP: 0x" << std::hex << current_rip << std::dec << "\n";
    }
    if (!last_stop_reason.empty()) {
        ss << "Last Stop: " << last_stop_reason << "\n";
    }
    if (!last_error.empty()) {
        ss << "Error: " << last_error << "\n";
    }
    return ss.str();
}

GameBootCoordinator::GameBootCoordinator()
    : vfs_(std::make_unique<VirtualFileSystem>()) {}

GameBootCoordinator::~GameBootCoordinator() {
    reset();
}

void GameBootCoordinator::reset() {
    if (save_initialized_) {
        SaveDataManager::instance().umount(save_mount_point_, false);
        SaveDataManager::instance().terminate();
        save_initialized_ = false;
        save_mount_point_.clear();
    }
    AudioOutManager::instance().close_all_ports();

    package_mgr_.clear();
    vfs_ = std::make_unique<VirtualFileSystem>();

    cpu_.reset();
    runtime_.reset();
    guest_memory_.reset();
    image_ = Ps4Image();
    tcb_address_ = 0;

    total_steps_ = 0;
    total_import_calls_ = 0;
    import_call_counts_.clear();
    last_import_called_.clear();
    last_stop_reason_.clear();
    error_message_.clear();
    package_report_ = PackageInspectionReport();
    state_ = BootState::not_started;
}

bool GameBootCoordinator::initialize(const BootConfig& config) {
    reset();
    if (!phase1_mount_packages(config)) return false;
    if (!phase2_setup_saves(config)) return false;
    if (!phase3_load_modules(config)) return false;
    if (!phase4_initialize_runtime(config)) return false;
    return true;
}

bool GameBootCoordinator::phase1_mount_packages(const BootConfig& config) {
    config_ = config;
    if (config_.base_dir.empty()) {
        state_ = BootState::error;
        error_message_ = "Base directory cannot be empty";
        return false;
    }

    std::filesystem::path base_path = std::filesystem::u8path(config_.base_dir);
    std::error_code ec;
    if (!std::filesystem::exists(base_path, ec) || !std::filesystem::is_directory(base_path, ec)) {
        state_ = BootState::error;
        error_message_ = "Base directory does not exist or is not a directory: " + config_.base_dir;
        return false;
    }

    vfs_->mount("/app0", base_path.string(), true);

    std::filesystem::path dvdroot_path;
    if (!config_.dvdroot_dir.empty()) {
        dvdroot_path = std::filesystem::u8path(config_.dvdroot_dir);
    } else {
        dvdroot_path = base_path / "dvdroot_ps4";
    }

    if (std::filesystem::exists(dvdroot_path, ec) && std::filesystem::is_directory(dvdroot_path, ec)) {
        package_mgr_.scan_directory(dvdroot_path.string(), true);
    } else if (std::filesystem::is_directory(base_path, ec)) {
        package_mgr_.scan_directory(base_path.string(), false);
    }

    if (package_mgr_.package_count() > 0) {
        package_mgr_.mount_all(*vfs_, config_.package_mount_points);
    } else if (std::filesystem::exists(dvdroot_path, ec) && std::filesystem::is_directory(dvdroot_path, ec)) {
        vfs_->mount("/dvdroot", dvdroot_path.string(), true);
    }

    package_report_ = package_mgr_.verify_essential_assets(config_.essential_asset_paths);

    if (config_.require_essential_assets && !package_report_.valid) {
        state_ = BootState::error;
        error_message_ = "Essential asset verification failed: " + package_report_.diagnostic_summary;
        return false;
    }

    state_ = BootState::packages_mounted;
    return true;
}

bool GameBootCoordinator::phase2_setup_saves(const BootConfig& config) {
    config_ = config;
    if (state_ != BootState::packages_mounted) {
        state_ = BootState::error;
        error_message_ = "Cannot setup saves: invalid state " + std::string(boot_state_to_string(state_));
        return false;
    }

    std::filesystem::path base_path = std::filesystem::u8path(config_.base_dir);
    std::filesystem::path save_path;
    if (!config_.save_dir.empty()) {
        save_path = std::filesystem::u8path(config_.save_dir);
    } else {
        save_path = base_path / "savedata";
    }

    std::error_code ec;
    std::filesystem::create_directories(save_path, ec);
    if (ec) {
        state_ = BootState::error;
        error_message_ = "Failed to create save directory: " + ec.message();
        return false;
    }

    auto& sm = SaveDataManager::instance();
    sm.terminate();
    if (!sm.initialize(save_path.string(), config_.save_quota_bytes, vfs_.get())) {
        state_ = BootState::error;
        error_message_ = "Failed to initialize SaveDataManager at " + save_path.string();
        return false;
    }
    sm.set_default_title_id(config_.title_id);

    std::string mount_pt;
    uint32_t status = 0;
    int32_t ret = sm.mount(
        config_.user_id,
        config_.title_id.c_str(),
        config_.save_dir_name.c_str(),
        SCE_SAVEDATA_MOUNT_MODE_RDWR | SCE_SAVEDATA_MOUNT_MODE_CREATE2,
        32768,
        mount_pt,
        status
    );
    if (ret != SCE_SAVEDATA_OK) {
        state_ = BootState::error;
        error_message_ = "Failed to mount default save slot: error " + std::to_string(ret);
        return false;
    }

    save_mount_point_ = mount_pt;
    save_initialized_ = true;
    return true;
}

bool GameBootCoordinator::phase3_load_modules(const BootConfig& config) {
    config_ = config;
    if (state_ != BootState::packages_mounted) {
        state_ = BootState::error;
        error_message_ = "Cannot load modules: invalid state " + std::string(boot_state_to_string(state_));
        return false;
    }

    std::filesystem::path base_path = std::filesystem::u8path(config_.base_dir);
    std::filesystem::path eboot_path = base_path / "eboot.bin";
    std::error_code ec;
    if (!std::filesystem::exists(eboot_path, ec) || !std::filesystem::is_regular_file(eboot_path, ec)) {
        eboot_path = base_path / "eboot.elf";
    }
    if (!std::filesystem::exists(eboot_path, ec) || !std::filesystem::is_regular_file(eboot_path, ec)) {
        state_ = BootState::error;
        error_message_ = "Neither eboot.bin nor eboot.elf found in " + base_path.string();
        return false;
    }

    std::vector<std::uint8_t> eboot_data;
    try {
        eboot_data = read_file_data(eboot_path);
    } catch (const std::exception& e) {
        state_ = BootState::error;
        error_message_ = "Failed to read eboot (" + eboot_path.filename().string() + "): " + std::string(e.what());
        return false;
    }

    std::map<std::string, std::vector<std::uint8_t>> modules;
    auto scan_prx_dir = [&](const std::filesystem::path& dir) {
        if (std::filesystem::exists(dir, ec) && std::filesystem::is_directory(dir, ec)) {
            for (const auto& item : std::filesystem::directory_iterator(dir, ec)) {
                if (!item.is_regular_file(ec) || item.path().extension() != ".prx") continue;
                std::string fname = item.path().filename().string();
                if (modules.find(fname) == modules.end()) {
                    try {
                        auto prx = read_file_data(item.path());
                        modules.emplace(std::move(fname), std::move(prx));
                    } catch (const std::exception& e) {
                        state_ = BootState::error;
                        error_message_ = "Failed to read module " + item.path().filename().string() + ": " + e.what();
                        return false;
                    }
                }
            }
        }
        return true;
    };

    std::filesystem::path module_dir = base_path / "sce_module";
    if (!scan_prx_dir(module_dir)) return false;
    if (!scan_prx_dir(base_path)) return false;

    try {
        guest_memory_ = std::make_unique<GuestMemory>(config_.guest_memory_size);
        image_ = Ps4Loader::load(*guest_memory_, eboot_data, modules);
    } catch (const std::exception& e) {
        state_ = BootState::error;
        error_message_ = "Ps4Loader::load failed: " + std::string(e.what());
        return false;
    }

    cpu_ = std::make_unique<X86Interpreter>(*guest_memory_);
    cpu_->add_trap_range(0x7f000000, 32 * (image_.imports.size() + 1));
    cpu_->add_trap_range(0x7e000000, 4096 * (image_.imports.size() + 1));
    cpu_->add_trap_range(Ps4Bootstrap::exit_trap, 64);

    try {
        tcb_address_ = Ps4Bootstrap::prepare(*guest_memory_, cpu_->state, image_);
    } catch (const std::exception& e) {
        state_ = BootState::error;
        error_message_ = "Ps4Bootstrap::prepare failed: " + std::string(e.what());
        return false;
    }

    return true;
}

bool GameBootCoordinator::phase4_initialize_runtime(const BootConfig& config) {
    config_ = config;
    if (state_ != BootState::packages_mounted || !guest_memory_ || !cpu_) {
        state_ = BootState::error;
        error_message_ = "Cannot initialize runtime: modules not loaded";
        return false;
    }

    runtime_ = std::make_unique<OrbisRuntime>(*guest_memory_);
    runtime_->set_procparam(image_.procparam);
    if (video_out_) {
        runtime_->attach_video_out(video_out_);
    }

    // Inicializa os módulos carregados na ordem topológica de dependência
    for (std::size_t i = 0; i < image_.modules.size(); ++i) {
        const auto& mod = image_.modules[i];
        if (!mod.init) continue;

        try {
            Ps4Bootstrap::module_call(*guest_memory_, cpu_->state, mod);
        } catch (const std::exception& e) {
            state_ = BootState::error;
            error_message_ = "Failed to setup module call for " + mod.name + ": " + e.what();
            return false;
        }

        std::uint64_t steps_in_mod = 0;
        bool mod_ok = false;
        while (steps_in_mod < config_.module_init_budget) {
            std::uint64_t slice = config_.module_init_budget - steps_in_mod;
            CpuRunResult res = cpu_->run(slice);
            steps_in_mod += res.steps;
            total_steps_ += res.steps;

            if (res.stop == CpuStop::trap) {
                if (res.rip == Ps4Bootstrap::exit_trap + 32) {
                    if (cpu_->state.gpr[0] != 0) {
                        state_ = BootState::error;
                        error_message_ = "Module " + mod.name + " init returned non-zero: " + std::to_string(cpu_->state.gpr[0]);
                        return false;
                    }
                    mod_ok = true;
                    break;
                }
                if (res.rip >= 0x7f000000 && res.rip < 0x7f000000 + 32 * image_.imports.size()) {
                    std::size_t idx = static_cast<std::size_t>((res.rip - 0x7f000000) / 32);
                    const auto& imp = image_.imports[idx];
                    import_call_counts_[imp.name]++;
                    total_import_calls_++;
                    last_import_called_ = imp.name;

                    std::uint64_t val = 0;
                    try {
                        val = runtime_->invoke(imp.name, cpu_->state);
                    } catch (const std::exception& e) {
                        state_ = BootState::error;
                        error_message_ = "Module " + mod.name + " init trap invoke failed for " + imp.name + ": " + e.what();
                        return false;
                    }

                    std::uint64_t ret_addr = 0;
                    if (guest_memory_->load(cpu_->state.gpr[4], &ret_addr, 8) != MemoryError::ok) {
                        state_ = BootState::error;
                        error_message_ = "Failed to load return address during module " + mod.name + " init";
                        return false;
                    }
                    cpu_->state.gpr[0] = val;
                    cpu_->state.gpr[4] += 8;
                    cpu_->state.rip = ret_addr;
                    continue;
                }
                state_ = BootState::error;
                error_message_ = "Module " + mod.name + " init hit unexpected trap at 0x" + hex_val(res.rip);
                return false;
            }
            state_ = BootState::error;
            error_message_ = "Module " + mod.name + " init stopped abnormally: " + res.describe();
            return false;
        }

        if (!mod_ok) {
            state_ = BootState::error;
            error_message_ = "Module " + mod.name + " init exceeded step budget";
            return false;
        }
    }

    // Configura o ponto de entrada principal do jogo
    Ps4Bootstrap::game_entry(*guest_memory_, cpu_->state, image_);

    state_ = BootState::modules_initialized;
    return true;
}

std::uint64_t GameBootCoordinator::step(std::uint64_t max_steps) {
    if (state_ == BootState::modules_initialized) {
        state_ = BootState::running;
    }
    if (state_ != BootState::running || !cpu_ || !guest_memory_) {
        return 0;
    }

    std::uint64_t steps_executed = 0;
    while (steps_executed < max_steps && state_ == BootState::running) {
        std::uint64_t slice = max_steps - steps_executed;
        CpuRunResult res = cpu_->run(slice);
        steps_executed += res.steps;
        total_steps_ += res.steps;
        last_stop_reason_ = res.describe();

        if (res.stop == CpuStop::step_limit) {
            break;
        }

        if (res.stop == CpuStop::halt) {
            state_ = BootState::halted;
            break;
        }

        if (res.stop == CpuStop::trap) {
            if (res.rip == Ps4Bootstrap::exit_trap) {
                state_ = BootState::halted;
                break;
            }
            if (res.rip >= 0x7f000000 && res.rip < 0x7f000000 + 32 * image_.imports.size()) {
                std::size_t idx = static_cast<std::size_t>((res.rip - 0x7f000000) / 32);
                const auto& imp = image_.imports[idx];
                import_call_counts_[imp.name]++;
                total_import_calls_++;
                last_import_called_ = imp.name;

                if (!config_.enable_hle || !runtime_) {
                    state_ = BootState::halted;
                    break;
                }

                std::uint64_t val = 0;
                try {
                    val = runtime_->invoke(imp.name, cpu_->state);
                } catch (const std::exception& e) {
                    state_ = BootState::error;
                    error_message_ = "HLE exception in " + imp.name + ": " + e.what();
                    break;
                }

                std::uint64_t ret_addr = 0;
                if (guest_memory_->load(cpu_->state.gpr[4], &ret_addr, 8) != MemoryError::ok) {
                    state_ = BootState::error;
                    error_message_ = "Failed to load return address during import " + imp.name;
                    break;
                }
                cpu_->state.gpr[0] = val;
                cpu_->state.gpr[4] += 8;
                cpu_->state.rip = ret_addr;
                continue;
            }
            if (res.rip >= 0x7e000000 && res.rip < 0x7e000000 + 4096 * image_.imports.size()) {
                state_ = BootState::error;
                error_message_ = "Jump into imported data trap at 0x" + hex_val(res.rip);
                break;
            }
            state_ = BootState::error;
            error_message_ = "Unhandled trap at 0x" + hex_val(res.rip);
            break;
        }

        state_ = BootState::error;
        error_message_ = "CPU error: " + res.describe();
        break;
    }
    return steps_executed;
}

BootState GameBootCoordinator::state() const {
    return state_;
}

BootDiagnostics GameBootCoordinator::diagnostics() const {
    BootDiagnostics diag;
    diag.state = state_;
    diag.state_name = boot_state_to_string(state_);
    diag.total_steps = total_steps_;
    diag.total_import_calls = total_import_calls_;
    diag.import_call_counts = import_call_counts_;
    diag.last_import_called = last_import_called_;

    diag.modules_count = image_.modules.size();
    diag.active_modules.reserve(image_.modules.size());
    for (const auto& m : image_.modules) {
        diag.active_modules.push_back(m.name);
    }

    diag.assets_valid = package_report_.valid;
    diag.package_report = package_report_;

    diag.save_initialized = save_initialized_;
    diag.save_mount_point = save_mount_point_;
    diag.open_audio_ports = AudioOutManager::instance().open_ports_count();

    diag.entry_point = image_.entry;
    diag.tcb_address = tcb_address_;
    diag.current_rip = cpu_ ? cpu_->state.rip : 0;
    diag.last_stop_reason = last_stop_reason_;
    diag.last_error = error_message_;

    return diag;
}

VirtualFileSystem& GameBootCoordinator::vfs() {
    return *vfs_;
}

const VirtualFileSystem& GameBootCoordinator::vfs() const {
    return *vfs_;
}

GamePackageManager& GameBootCoordinator::package_manager() {
    return package_mgr_;
}

const GamePackageManager& GameBootCoordinator::package_manager() const {
    return package_mgr_;
}

SaveDataManager& GameBootCoordinator::save_manager() {
    return SaveDataManager::instance();
}

const SaveDataManager& GameBootCoordinator::save_manager() const {
    return SaveDataManager::instance();
}

AudioOutManager& GameBootCoordinator::audio_out() {
    return AudioOutManager::instance();
}

GuestMemory* GameBootCoordinator::guest_memory() {
    return guest_memory_.get();
}

const GuestMemory* GameBootCoordinator::guest_memory() const {
    return guest_memory_.get();
}

X86Interpreter* GameBootCoordinator::cpu() {
    return cpu_.get();
}

const X86Interpreter* GameBootCoordinator::cpu() const {
    return cpu_.get();
}

OrbisRuntime* GameBootCoordinator::runtime() {
    return runtime_.get();
}

const OrbisRuntime* GameBootCoordinator::runtime() const {
    return runtime_.get();
}

const Ps4Image& GameBootCoordinator::image() const {
    return image_;
}

const BootConfig& GameBootCoordinator::config() const {
    return config_;
}

const PackageInspectionReport& GameBootCoordinator::package_report() const {
    return package_report_;
}

const std::string& GameBootCoordinator::last_error() const {
    return error_message_;
}

} // namespace bb
