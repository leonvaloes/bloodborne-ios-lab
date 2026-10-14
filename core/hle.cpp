#include "hle.h"
#include <stdexcept>
#include <chrono>

namespace bb {

void HleManager::register_handler(std::uint64_t nid, const std::string& name, HleHandler handler) {
    if (!handler) throw std::invalid_argument("Cannot register null HLE handler");
    handlers_[nid] = HleSymbol{name, handler};
}

bool HleManager::has_symbol(std::uint64_t nid) const {
    return handlers_.find(nid) != handlers_.end();
}

std::int64_t HleManager::invoke(std::uint64_t nid, GuestMemory& memory, std::uint64_t arg0, std::uint64_t arg1, std::uint64_t arg2) const {
    auto it = handlers_.find(nid);
    if (it == handlers_.end()) {
        throw std::runtime_error("Unresolved HLE NID: " + std::to_string(nid));
    }
    return it->second.handler(memory, arg0, arg1, arg2);
}

std::size_t HleManager::count() const {
    return handlers_.size();
}

HleManager HleManager::create_standard() {
    HleManager manager;

    // sceKernelGettimeofday (NID 0x2001)
    // Args: arg0 = guest timeval pointer (uint64_t sec, uint64_t usec)
    manager.register_handler(0x2001, "sceKernelGettimeofday", [](GuestMemory& memory, std::uint64_t arg0, std::uint64_t, std::uint64_t) -> std::int64_t {
        if (!arg0) return -1;
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(now).count();
        const auto usecs = std::chrono::duration_cast<std::chrono::microseconds>(now).count() % 1000000;
        struct { std::uint64_t s, us; } tv{static_cast<std::uint64_t>(secs), static_cast<std::uint64_t>(usecs)};
        if (memory.store(arg0, &tv, sizeof(tv)) != MemoryError::ok) return -1;
        return 0;
    });

    // sceKernelGetCurrentCpu (NID 0x2002)
    manager.register_handler(0x2002, "sceKernelGetCurrentCpu", [](GuestMemory&, std::uint64_t, std::uint64_t, std::uint64_t) -> std::int64_t {
        return 0; // Return logical CPU 0
    });

    // sceKernelAllocateDirectMemory (NID 0x2003)
    // Args: arg0 = target address, arg1 = size, arg2 = pointer to write allocated address
    manager.register_handler(0x2003, "sceKernelAllocateDirectMemory", [](GuestMemory& memory, std::uint64_t arg0, std::uint64_t arg1, std::uint64_t arg2) -> std::int64_t {
        if (memory.map(arg0, arg1, read | write) != MemoryError::ok) return -1;
        if (arg2) {
            if (memory.store(arg2, &arg0, sizeof(arg0)) != MemoryError::ok) return -1;
        }
        return 0; // Success
    });

    return manager;
}

} // namespace bb
