#pragma once
#include "guest_memory.h"
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace bb {

using HleHandler = std::function<std::int64_t(GuestMemory& memory, std::uint64_t arg0, std::uint64_t arg1, std::uint64_t arg2)>;

struct HleSymbol {
    std::string name;
    HleHandler handler;
};

class HleManager {
public:
    void register_handler(std::uint64_t nid, const std::string& name, HleHandler handler);
    bool has_symbol(std::uint64_t nid) const;
    std::int64_t invoke(std::uint64_t nid, GuestMemory& memory, std::uint64_t arg0, std::uint64_t arg1, std::uint64_t arg2) const;
    std::size_t count() const;

    // Registers standard baseline PS4 kernel imports
    static HleManager create_standard();

private:
    std::unordered_map<std::uint64_t, HleSymbol> handlers_;
};

} // namespace bb
