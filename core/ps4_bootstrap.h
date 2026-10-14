#pragma once
#include "ps4_loader.h"
#include "x86_interp.h"
#include <cstdint>

namespace bb {
// Guest-only main thread layout; never changes the host FS/GS registers.
class Ps4Bootstrap {
public:
    static constexpr std::uint64_t stack_base = 0x70000000, stack_size = 8u << 20;
    static constexpr std::uint64_t param_base = 0x6f000000, tls_base = 0x6e000000;
    static constexpr std::uint64_t exit_trap = 0x7d000000;
    // Returns the guest TCB address. Image must already be mapped and relocated.
    static std::uint64_t prepare(GuestMemory& memory, CpuState& cpu, const Ps4Image& image);
    // Set up SysV call to a module initializer (0, NULL, NULL). Resume at entry
    // only after its return trap has been reached with a zero return value.
    static void module_call(GuestMemory& memory, CpuState& cpu, const Ps4Module& module);
    static void game_entry(GuestMemory& memory, CpuState& cpu, const Ps4Image& image);
    // Dynamic module TLS registration in the guest TCB / DTV.
    static std::uint64_t register_dynamic_tls(GuestMemory& memory, const Ps4Image& image, const Ps4Tls& tls);
};
} // namespace bb
