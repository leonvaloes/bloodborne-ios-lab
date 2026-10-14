#pragma once
#include "alu.h"
#include <cstdint>
#include <unordered_map>
#include <stdexcept>
#include <string>

namespace bb {

using GuestFn = Result (*)(std::uint64_t, std::uint64_t);

class IndirectDispatcher {
public:
    void register_target(std::uint64_t guest_rip, GuestFn fn);
    bool has_target(std::uint64_t guest_rip) const;
    Result dispatch(std::uint64_t guest_rip, std::uint64_t a, std::uint64_t b) const;
    std::size_t size() const;
    void clear();

private:
    std::unordered_map<std::uint64_t, GuestFn> table_;
};

} // namespace bb
