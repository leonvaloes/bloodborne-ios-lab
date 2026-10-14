#include "dispatcher.h"

namespace bb {

void IndirectDispatcher::register_target(std::uint64_t guest_rip, GuestFn fn) {
    if (!fn) throw std::invalid_argument("Cannot register null target function");
    table_[guest_rip] = fn;
}

bool IndirectDispatcher::has_target(std::uint64_t guest_rip) const {
    return table_.find(guest_rip) != table_.end();
}

Result IndirectDispatcher::dispatch(std::uint64_t guest_rip, std::uint64_t a, std::uint64_t b) const {
    auto it = table_.find(guest_rip);
    if (it == table_.end()) {
        throw std::runtime_error("Unresolved indirect target: " + std::to_string(guest_rip));
    }
    return it->second(a, b);
}

std::size_t IndirectDispatcher::size() const {
    return table_.size();
}

void IndirectDispatcher::clear() {
    table_.clear();
}

} // namespace bb
