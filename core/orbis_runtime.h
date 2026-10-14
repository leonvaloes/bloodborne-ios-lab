#pragma once
#include "guest_memory.h"
#include "x86_interp.h"
#include "guest_scheduler.h"
#include "pad_hle.h"
#include "user_service.h"
#include "save_data.h"
#include "audio_out.h"
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace bb {
class GuestScheduler;
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
    // Explicit opt-in; blocking guest services must be dispatched by a scheduler-aware
    // trap driver, not through invoke() on the host thread.
    void attach_scheduler(GuestScheduler& scheduler) { scheduler_ = &scheduler; }
    void set_procparam(std::uint64_t address) { procparam_ = address; }
    std::uint64_t procparam() const { return procparam_; }
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
    GuestMemory& mem_;
    GuestScheduler* scheduler_ = nullptr;
    std::uint64_t procparam_ = 0;
    std::uint64_t thread_stack_next_ = 0x3000000000ULL;
    UserServiceManager user_mgr_;
    PadHleManager pad_mgr_;
    SaveDataManager save_mgr_;
    AudioOutManager audio_mgr_;
    struct PendingCallback { std::uint64_t key = 0, return_address = 0, return_trap = 0; };
    std::unordered_map<GuestScheduler::Id, PendingCallback> callbacks_;
    std::unordered_map<std::uint64_t, GuestScheduler::Id> guest_mutexes_, guest_conds_, guest_events_;
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
    std::uint64_t next_virtual_ = 0x1000000000ULL, next_heap_ = 0x2000000000ULL;
    std::uint64_t next_handle_ = 0x10000, physical_used_ = 0;
};
} // namespace bb
