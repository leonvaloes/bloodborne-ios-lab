#include "guest_memory.h"
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace bb {
namespace {
bool valid_permissions(unsigned p) { return (p & ~(read | write)) == 0; }
bool valid_span(std::uint64_t address, std::size_t size) {
    return size != 0 && size <= std::numeric_limits<std::uint64_t>::max() - address;
}
}

MemoryError GuestMemory::map(std::uint64_t address, std::size_t size, unsigned p) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!valid_span(address, size) || !valid_permissions(p)) return MemoryError::invalid;
    auto next = regions_.lower_bound(address);
    if (next != regions_.end() && next->first < address + size) return MemoryError::overlap;
    if (next != regions_.begin()) {
        const auto& previous = *std::prev(next);
        if (address - previous.first < previous.second.bytes.size()) return MemoryError::overlap;
    }
    if (size > budget_ - used_) return MemoryError::budget;
    try {
        regions_.emplace(address, Region{std::vector<std::uint8_t>(size, 0), p});
    } catch (const std::bad_alloc&) { return MemoryError::allocation;
    } catch (const std::length_error&) { return MemoryError::allocation; }
    used_ += size;
    return MemoryError::ok;
}

MemoryError GuestMemory::unmap(std::uint64_t address) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = regions_.find(address);
    if (it == regions_.end()) return MemoryError::unmapped;
    used_ -= it->second.bytes.size();
    regions_.erase(it);
    return MemoryError::ok;
}

MemoryError GuestMemory::protect(std::uint64_t address, unsigned p) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!valid_permissions(p)) return MemoryError::invalid;
    auto it = regions_.find(address);
    if (it == regions_.end()) return MemoryError::unmapped;
    it->second.permissions = p;
    return MemoryError::ok;
}

GuestMemory::Regions::const_iterator GuestMemory::find(std::uint64_t address, std::size_t size) const {
    auto it = regions_.upper_bound(address);
    if (it == regions_.begin()) return regions_.end();
    --it;
    const auto offset = address - it->first;
    if (offset >= it->second.bytes.size() || size > it->second.bytes.size() - offset)
        return regions_.end();
    return it;
}

MemoryError GuestMemory::load(std::uint64_t address, void* destination, std::size_t size) const {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!destination || !valid_span(address, size)) return MemoryError::invalid;
    auto it = find(address, size);
    if (it == regions_.end()) return MemoryError::unmapped;
    if (!(it->second.permissions & read)) return MemoryError::denied;
    std::memcpy(destination, it->second.bytes.data() + (address - it->first), size);
    return MemoryError::ok;
}

MemoryError GuestMemory::store(std::uint64_t address, const void* source, std::size_t size) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!source || !valid_span(address, size)) return MemoryError::invalid;
    auto found = find(address, size);
    if (found == regions_.end()) return MemoryError::unmapped;
    if (!(found->second.permissions & write)) return MemoryError::denied;
    auto it = regions_.find(found->first);
    std::memcpy(it->second.bytes.data() + (address - it->first), source, size);
    return MemoryError::ok;
}

std::size_t GuestMemory::used() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return used_;
}

MemoryError GuestMemory::atomic_fetch_add_u64(std::uint64_t address, std::uint64_t delta, std::uint64_t& old_val) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!valid_span(address, sizeof(std::uint64_t))) return MemoryError::invalid;
    auto found = find(address, sizeof(std::uint64_t));
    if (found == regions_.end()) return MemoryError::unmapped;
    if ((found->second.permissions & (read | write)) != (read | write)) return MemoryError::denied;
    auto it = regions_.find(found->first);
    std::uint8_t* ptr = it->second.bytes.data() + (address - it->first);
    std::memcpy(&old_val, ptr, sizeof(std::uint64_t));
    const std::uint64_t new_val = old_val + delta;
    std::memcpy(ptr, &new_val, sizeof(std::uint64_t));
    return MemoryError::ok;
}

MemoryError GuestMemory::atomic_fetch_add_u32(std::uint64_t address, std::uint32_t delta, std::uint32_t& old_val) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!valid_span(address, sizeof(std::uint32_t))) return MemoryError::invalid;
    auto found = find(address, sizeof(std::uint32_t));
    if (found == regions_.end()) return MemoryError::unmapped;
    if ((found->second.permissions & (read | write)) != (read | write)) return MemoryError::denied;
    auto it = regions_.find(found->first);
    std::uint8_t* ptr = it->second.bytes.data() + (address - it->first);
    std::memcpy(&old_val, ptr, sizeof(std::uint32_t));
    const std::uint32_t new_val = old_val + delta;
    std::memcpy(ptr, &new_val, sizeof(std::uint32_t));
    return MemoryError::ok;
}

MemoryError GuestMemory::atomic_compare_exchange_u64(std::uint64_t address, std::uint64_t& expected, std::uint64_t desired, bool& success) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!valid_span(address, sizeof(std::uint64_t))) return MemoryError::invalid;
    auto found = find(address, sizeof(std::uint64_t));
    if (found == regions_.end()) return MemoryError::unmapped;
    if ((found->second.permissions & (read | write)) != (read | write)) return MemoryError::denied;
    auto it = regions_.find(found->first);
    std::uint8_t* ptr = it->second.bytes.data() + (address - it->first);
    std::uint64_t current = 0;
    std::memcpy(&current, ptr, sizeof(std::uint64_t));
    if (current == expected) {
        std::memcpy(ptr, &desired, sizeof(std::uint64_t));
        success = true;
    } else {
        expected = current;
        success = false;
    }
    return MemoryError::ok;
}

MemoryError GuestMemory::atomic_compare_exchange_u32(std::uint64_t address, std::uint32_t& expected, std::uint32_t desired, bool& success) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!valid_span(address, sizeof(std::uint32_t))) return MemoryError::invalid;
    auto found = find(address, sizeof(std::uint32_t));
    if (found == regions_.end()) return MemoryError::unmapped;
    if ((found->second.permissions & (read | write)) != (read | write)) return MemoryError::denied;
    auto it = regions_.find(found->first);
    std::uint8_t* ptr = it->second.bytes.data() + (address - it->first);
    std::uint32_t current = 0;
    std::memcpy(&current, ptr, sizeof(std::uint32_t));
    if (current == expected) {
        std::memcpy(ptr, &desired, sizeof(std::uint32_t));
        success = true;
    } else {
        expected = current;
        success = false;
    }
    return MemoryError::ok;
}

} // namespace bb
