#pragma once
#include "guest_memory.h"
#include "x86_interp.h"
#include "guest_scheduler.h"
#include "pad_hle.h"
#include "user_service.h"
#include "save_data.h"
#include "audio_out.h"
#include "video_out.h"
#include "gnm_rasterizer.h"
#include "ps4_loader.h"
#include <cstdio>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include <filesystem>

namespace bb {
class GuestScheduler;
class GnmContext;
// NID textual Orbis (11 caracteres base64, sem padding). Implementação local
// independente de plataforma; a resolução pode ser substituída pelo loader.
std::string orbis_nid(const std::string& symbol);

class OrbisRuntime {
public:
    struct InvocationResult {
        enum class Action {
            returned,
            blocked,
            exited,
            callback
        } action = Action::returned;
        std::uint64_t value = 0;
        std::uint64_t callback = 0;
        std::uint64_t argument = 0;
    };

    explicit OrbisRuntime(GuestMemory& memory);
    ~OrbisRuntime();
    // Explicit opt-in; blocking guest services must be dispatched by a scheduler-aware
    // trap driver, not through invoke() on the host thread.
    void attach_scheduler(GuestScheduler& scheduler) { scheduler_ = &scheduler; }
    void attach_video_out(VideoOutManager* video_out) { video_out_ = video_out; }
    VideoOutManager* video_out() { return video_out_; }
    const VideoOutManager* video_out() const { return video_out_; }
    GnmContext* gnm_context() { return gnm_context_.get(); }
    const GnmContext* gnm_context() const { return gnm_context_.get(); }
    GnmRasterizer& rasterizer() { return rasterizer_; }
    const GnmRasterizer& rasterizer() const { return rasterizer_; }
    std::uint64_t submit_count() const { return submit_count_; }
    const RasterizerStats& rasterizer_stats() const { return rasterizer_stats_; }
    RasterizerStatus last_rasterizer_status() const { return last_rasterizer_status_; }
    std::uint64_t rasterizer_failure_count() const { return rasterizer_failure_count_; }
    void set_procparam(std::uint64_t address) { procparam_ = address; }
    std::uint64_t procparam() const { return procparam_; }
    void set_tls_template(const Ps4Tls& t) { tls_template_ = t; }
    const Ps4Tls& tls_template() const { return tls_template_; }
    std::string resolve(const std::string& import) const;
    // Trap SysV: não modifica RIP/RSP; o chamador executa o retorno somente em sucesso.
    // Lança runtime_error para imports/argumentos que não podem ser emulados.
    std::uint64_t invoke(const std::string& import, const CpuState& cpu);
    std::uint64_t invoke(const std::string& import, CpuState& cpu);

    PadHleManager& pad_manager() { return pad_mgr_; }
    const PadHleManager& pad_manager() const { return pad_mgr_; }

    UserServiceManager& user_service_manager() { return user_mgr_; }
    const UserServiceManager& user_service_manager() const { return user_mgr_; }

    SaveDataManager& save_data_manager() { return save_mgr_; }
    const SaveDataManager& save_data_manager() const { return save_mgr_; }

    AudioOutManager& audio_out_manager() { return audio_mgr_; }
    const AudioOutManager& audio_out_manager() const { return audio_mgr_; }

    void set_app0_dir(const std::filesystem::path& dir) { app0_dir_ = dir; }
    const std::filesystem::path& app0_dir() const { return app0_dir_; }
    std::string resolve_guest_path(const std::string& guest_path) const;

    // Adaptador scheduler-traps:
    InvocationResult invoke_trap(const std::string& import, CpuState& cpu,
                                 GuestScheduler::Id thread, std::uint64_t return_trap = 0);
    InvocationResult invoke(const std::string& import, CpuState& cpu,
                            GuestScheduler::Id thread, std::uint64_t return_trap = 0) {
        return invoke_trap(import, cpu, thread, return_trap);
    }
    InvocationResult complete_callback(CpuState& cpu, GuestScheduler::Id thread);
    bool has_pending_callback(GuestScheduler::Id thread) const {
        return callbacks_.count(thread) != 0;
    }
private:
    std::uint64_t arg(const CpuState& cpu, unsigned index) const;
    std::uint64_t handle(const std::string& name, CpuState& cpu);
    std::uint64_t get_errno_addr(GuestScheduler::Id tid = 1);
    GuestMemory& mem_;
    GuestScheduler* scheduler_ = nullptr;
    std::uint64_t procparam_ = 0;
    std::uint64_t thread_stack_next_ = 0x3000000000ULL;
    std::uint64_t errno_page_ = 0;
    int exit_code_ = 0;
    
    struct PthreadAttrDefaults {
        std::uint64_t stack_size = 0;
        std::uint32_t detach_state = 0;
        std::uint32_t sched_policy = 0;
        std::uint32_t sched_param = 0;
        std::uint64_t affinity = 0;
        std::uint32_t inherit_sched = 0;
    };
    PthreadAttrDefaults* find_attr(std::uint64_t slot_addr);
    std::unordered_map<std::uint64_t, PthreadAttrDefaults> attr_defaults_;
    
    UserServiceManager user_mgr_;
    PadHleManager pad_mgr_;
    SaveDataManager save_mgr_;
    AudioOutManager audio_mgr_;
    VideoOutManager* video_out_ = nullptr;
    std::unique_ptr<GnmContext> gnm_context_;
    GnmRasterizer rasterizer_;
    std::uint64_t submit_count_ = 0;
    RasterizerStats rasterizer_stats_{};
    RasterizerStatus last_rasterizer_status_ = RasterizerStatus::Ok;
    std::uint64_t rasterizer_failure_count_ = 0;
    std::filesystem::path app0_dir_;
    struct KernelFileEntry {
        FILE* fp = nullptr;
        std::string path;
        int flags = 0;
        std::uint64_t size = 0;
    };
    std::unordered_map<int, KernelFileEntry> kernel_files_;
    int next_kernel_fd_ = 3;
    std::unordered_map<std::uint64_t, FILE*> open_files_;
    struct PendingCallback { std::uint64_t key = 0, return_address = 0, return_trap = 0; };
    std::unordered_map<GuestScheduler::Id, PendingCallback> callbacks_;
    std::unordered_map<std::uint64_t, GuestScheduler::Id> guest_mutexes_, guest_conds_;
    std::unordered_map<std::uint32_t, GuestScheduler::Id> guest_events_, guest_semas_;
    struct SemaRecord {
        std::string name;
        std::uint32_t attr = 0;
        std::int32_t init_count = 0;
        std::int32_t max_count = 0;
    };
    std::unordered_map<std::uint32_t, SemaRecord> sema_records_;
    std::unordered_map<std::string, std::string> names_;
    std::map<std::uint64_t, std::uint64_t> physical_, mappings_, heap_;
    std::map<std::uint64_t, std::uint64_t> direct_mappings_; // virtual -> physical offset
    std::unordered_map<std::uint64_t, unsigned> mutexes_, attrs_;
    std::unordered_map<std::uint64_t, unsigned> conds_;
    std::unordered_map<std::uint64_t, std::uint64_t> keys_, key_values_;
    std::unordered_map<std::uint64_t, std::uint64_t> event_flags_;
    std::unordered_map<std::uint64_t, bool> guards_;
    std::vector<std::uint64_t> exit_callbacks_;
    std::unordered_map<std::uint64_t, int> rwlocks_; // positivo: leitores, -1: escritor
    bool user_login_event_pending_ = true;
    std::uint64_t next_virtual_ = 0x1000000000ULL, next_heap_ = 0x2000000000ULL;
    struct EqueueEvent {
        std::uint64_t ident = 0;
        std::int16_t filter = 0;
        std::uint16_t flags = 0;
        std::uint32_t fflags = 0;
        std::uint64_t data = 0;
        std::uint64_t udata = 0;
    };
    struct EqueueRecord {
        std::string name;
        std::vector<EqueueEvent> pending_events;
        std::uint64_t flip_udata = 0;
        bool has_flip_event = false;
    };
    std::unordered_map<std::uint32_t, EqueueRecord> equeues_;
    std::uint32_t next_equeue_id_ = 100;
    Ps4Tls tls_template_{};
    std::uint64_t tessellation_ring_addr_ = 0;
    std::uint64_t next_handle_ = 0x10000, physical_used_ = 0;
};
} // namespace bb
