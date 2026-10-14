#include "host_bridge.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <limits>

namespace bb {

// InternalVideoSink implementation
void HostBridge::InternalVideoSink::on_flip(const FlipEvent& /*event*/) {
    // Flips são contados exclusivamente pelo VideoOutManager; frame_count_ conta ticks de update().
}

HostBridge::HostBridge(const HostBridgeConfig& config)
    : config_(config),
      boot_coordinator_{},
      input_{},
      touch_controls_(&input_),
      audio_out_{},
      video_out_{},
      pad_hle_(&input_, nullptr),
      video_sink_{this} {
    audio_out_.set_time_provider([]() -> std::uint64_t {
        return 0;
    });
}

HostBridge::~HostBridge() {
    shutdown();
}

bool HostBridge::init(const std::string& documents_dir) {
    HostBridgeConfig cfg = config_;
    cfg.documents_dir = documents_dir;
    return init(cfg);
}

bool HostBridge::init(const HostBridgeConfig& config) {
    config_ = config;
    reset();

    if (config_.documents_dir.empty()) {
        return false;
    }

    BootConfig boot_config;
    boot_config.base_dir = config_.documents_dir;
    boot_config.dvdroot_dir = config_.dvdroot_dir;
    boot_config.save_dir = config_.save_dir;
    boot_config.title_id = config_.title_id;
    boot_config.save_dir_name = config_.save_dir_name;
    boot_config.user_id = config_.user_id;
    boot_config.save_quota_bytes = config_.save_quota_bytes;
    boot_config.guest_memory_size = config_.guest_memory_size;
    boot_config.module_init_budget = config_.module_init_budget;
    boot_config.essential_asset_paths = config_.essential_asset_paths;
    boot_config.require_essential_assets = config_.require_essential_assets;
    boot_config.enable_hle = config_.enable_hle;

    boot_coordinator_.attach_video_out(&video_out_);

    if (!boot_coordinator_.initialize(boot_config)) {
        // Preserva o diagnóstico de falha; reset() limpa recursos parciais mesmo sem initialized_.
        return false;
    }

    video_out_.attach_guest_memory(boot_coordinator_.guest_memory(), &video_sink_);
    if (boot_coordinator_.runtime()) {
        boot_coordinator_.runtime()->attach_video_out(&video_out_);
    }
    touch_controls_.bind_input_manager(&input_);
    pad_hle_.set_default_input(&input_);

    initialized_ = true;
    frame_count_ = 0;
    return true;
}

std::uint64_t HostBridge::update(float dt) {
    if (!initialized_ || !boot_coordinator_.guest_memory() || !boot_coordinator_.cpu()) {
        return 0;
    }

    std::uint64_t max_steps = calculate_steps_for_dt(dt);
    if (max_steps == 0) max_steps = 1;

    std::uint64_t steps_executed = boot_coordinator_.step(max_steps);
    total_steps_ += steps_executed;

    update_audio(dt);
    update_video();
    update_haptics();

    if (frame_callback_ && video_out_.consume_new_frame()) {
        HostFrameBuffer frame;
        if (render_frame(frame)) {
            frame_callback_(frame);
        }
    }

    frame_count_++;
    return steps_executed;
}

bool HostBridge::render_frame(HostFrameBuffer& out_frame) {
    if (!initialized_) {
        out_frame = {};
        return false;
    }

    return video_out_.read_current_frame_pixels(out_frame);
}

bool HostBridge::render_frame() {
    HostFrameBuffer frame;
    return render_frame(frame);
}

bool HostBridge::on_touch(int id, float x, float y, TouchPhase phase) {
    return on_touch(static_cast<std::uint64_t>(id), x, y, phase);
}

bool HostBridge::on_touch(std::uint64_t id, float x, float y, TouchPhase phase) {
    if (!initialized_) return false;
    return touch_controls_.on_touch(id, x, y, phase);
}

bool HostBridge::is_running() const {
    return initialized_ && (boot_coordinator_.state() == BootState::running ||
                            boot_coordinator_.state() == BootState::modules_initialized);
}

bool HostBridge::is_initialized() const {
    return initialized_;
}

BootState HostBridge::state() const {
    return boot_coordinator_.state();
}

HostBridgeDiagnostics HostBridge::diagnostics() const {
    HostBridgeDiagnostics diag;
    auto boot_diag = boot_coordinator_.diagnostics();

    diag.boot_state = boot_diag.state;
    diag.boot_state_name = boot_diag.state_name;
    diag.total_steps = boot_diag.total_steps;
    diag.total_import_calls = boot_diag.total_import_calls;
    diag.import_call_counts = boot_diag.import_call_counts;
    diag.last_import_called = boot_diag.last_import_called;
    diag.active_modules = boot_diag.active_modules;
    diag.modules_count = boot_diag.modules_count;
    diag.assets_valid = boot_diag.assets_valid;
    diag.package_report = boot_diag.package_report;
    diag.save_initialized = boot_diag.save_initialized;
    diag.save_mount_point = boot_diag.save_mount_point;
    diag.open_audio_ports = boot_diag.open_audio_ports;
    diag.entry_point = boot_diag.entry_point;
    diag.tcb_address = boot_diag.tcb_address;
    diag.current_rip = boot_diag.current_rip;
    diag.last_stop_reason = boot_diag.last_stop_reason;
    diag.last_error = boot_diag.last_error;

    diag.video_flip_count = video_out_.flip_count();
    diag.video_has_new_frame = video_out_.has_new_frame();
    auto last_flip = video_out_.last_flip();
    diag.video_current_buffer_index = last_flip.buffer_index;
    const VideoBuffer* vbuf = video_out_.buffer(last_flip.buffer_index);
    if (vbuf) {
        diag.video_width = vbuf->width;
        diag.video_height = vbuf->height;
    }

    diag.audio_frames_drained = 0;
    diag.audio_underflows = 0;
    diag.active_touches = touch_controls_.active_touch_count();

    return diag;
}

std::string HostBridge::last_error() const {
    return boot_coordinator_.last_error();
}

void HostBridge::reset() {
    video_out_.detach_guest_memory();
    boot_coordinator_.reset();
    touch_controls_.reset();
    audio_out_.close_all_ports();
    input_.reset();
    touch_controls_.bind_input_manager(&input_);
    pad_hle_.set_default_input(&input_);
    initialized_ = false;
    frame_count_ = 0;
    total_steps_ = 0;
}

void HostBridge::shutdown() {
    reset();
}

void HostBridge::update_audio(float dt) {
    const std::uint32_t sample_rate = 48000;
    std::size_t expected_frames = static_cast<std::size_t>(sample_rate * dt);
    if (expected_frames > 4096) expected_frames = 4096;

    std::vector<std::int16_t> scratch(expected_frames * 2);
    audio_out_.drain_main_port_pcm16(scratch.data(), expected_frames);
}

void HostBridge::update_video() {
    // Flips são processados via sink
}

void HostBridge::update_haptics() {
    touch_controls_.update_haptics_from_hle(pad_hle_, 1);
}

std::uint64_t HostBridge::calculate_steps_for_dt(float dt) const {
    if (dt <= 0.0f) return 1;
    double steps = steps_per_second_ * dt;
    if (steps < 1.0) return 1;
    if (steps > static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(steps);
}

std::string HostBridgeDiagnostics::summary() const {
    std::ostringstream ss;
    ss << "=== HostBridge Diagnostics ===\n";
    ss << "Boot State: " << boot_state_name << " (" << static_cast<int>(boot_state) << ")\n";
    ss << "Initialized: " << (boot_state != BootState::not_started && boot_state != BootState::error ? "YES" : "NO") << "\n";
    ss << "Total Steps: " << total_steps << "\n";
    ss << "Total Import Calls: " << total_import_calls << "\n";
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
    ss << "Packages: " << (assets_valid ? "VALID" : "INVALID")
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
    ss << "Video Flips: " << video_flip_count;
    if (video_has_new_frame) ss << " (new frame)";
    ss << " [buf=" << video_current_buffer_index << ", " << video_width << "x" << video_height << "]\n";
    ss << "Active Touches: " << active_touches << "\n";
    if (entry_point != 0) ss << "Entry Point: 0x" << std::hex << entry_point << std::dec << "\n";
    if (tcb_address != 0) ss << "TCB Address: 0x" << std::hex << tcb_address << std::dec << "\n";
    if (current_rip != 0) ss << "Current RIP: 0x" << std::hex << current_rip << std::dec << "\n";
    if (!last_stop_reason.empty()) ss << "Last Stop: " << last_stop_reason << "\n";
    if (!last_error.empty()) ss << "Error: " << last_error << "\n";
    return ss.str();
}

} // namespace bb
