#include "dispatcher.h"

namespace bb {

namespace {
thread_local IndirectDispatcher* g_active_dispatcher = nullptr;
}

void IndirectDispatcher::register_target(std::uint64_t guest_rip, GuestFn fn) {
    if (!fn) throw std::invalid_argument("Cannot register null target function");
    table_[guest_rip] = fn;
}

void IndirectDispatcher::register_batch(const std::vector<std::pair<std::uint64_t, GuestFn>>& batch) {
    for (const auto& entry : batch) {
        if (!entry.second) throw std::invalid_argument("Cannot register null target function in batch");
        table_[entry.first] = entry.second;
    }
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

IndirectDispatcher* IndirectDispatcher::get_active() {
    return g_active_dispatcher;
}

void IndirectDispatcher::set_active(IndirectDispatcher* d) {
    g_active_dispatcher = d;
}

} // namespace bb
