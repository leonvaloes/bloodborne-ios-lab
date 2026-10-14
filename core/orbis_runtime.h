#pragma once
#include "guest_memory.h"
#include "x86_interp.h"
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
    explicit OrbisRuntime(GuestMemory& memory);
    // Explicit opt-in; blocking guest services must be dispatched by a scheduler-aware
    // trap driver, not through invoke() on the host thread.
    void attach_scheduler(GuestScheduler& scheduler) { scheduler_ = &scheduler; }
    std::string resolve(const std::string& import) const;
    // Trap SysV: não modifica RIP/RSP; o chamador executa o retorno somente em sucesso.
    // Lança runtime_error para imports/argumentos que não podem ser emulados.
    std::uint64_t invoke(const std::string& import, const CpuState& cpu);
private:
    std::uint64_t arg(const CpuState& cpu, unsigned index) const;
    std::uint64_t handle(const std::string& name, const CpuState& cpu);
    GuestMemory& mem_;
    GuestScheduler* scheduler_ = nullptr;
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
