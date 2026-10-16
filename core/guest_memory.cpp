#include "guest_memory.h"
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace bb {

MemoryBuffer::MemoryBuffer(std::size_t size) : size_(size) {
    if (size == 0) return;
#if defined(_WIN32)
    data_ = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!data_) throw std::bad_alloc();
#else
    void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) throw std::bad_alloc();
    data_ = static_cast<std::uint8_t*>(p);
#endif
}

MemoryBuffer::~MemoryBuffer() {
    if (!data_) return;
#if defined(_WIN32)
    ::VirtualFree(data_, 0, MEM_RELEASE);
#else
    ::munmap(data_, size_);
#endif
}

MemoryBuffer::MemoryBuffer(MemoryBuffer&& other) noexcept : data_(other.data_), size_(other.size_) {
    other.data_ = nullptr;
    other.size_ = 0;
}

MemoryBuffer& MemoryBuffer::operator=(MemoryBuffer&& other) noexcept {
    if (this != &other) {
        if (data_) {
#if defined(_WIN32)
            ::VirtualFree(data_, 0, MEM_RELEASE);
#else
            ::munmap(data_, size_);
#endif
        }
        data_ = other.data_;
        size_ = other.size_;
        other.data_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

namespace {
bool valid_permissions(unsigned p) { return (p & ~(read | write)) == 0; }
bool valid_span(std::uint64_t address, std::size_t size) {
    return size != 0 && size <= std::numeric_limits<std::uint64_t>::max() - address;
}
}

MemoryError GuestMemory::map(std::uint64_t address, std::size_t size, unsigned p) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!valid_span(address, size) || !valid_permissions(p)) return MemoryError::invalid;
    if (protect_header_page_ && address < 0x10000) return MemoryError::invalid;
    auto next = regions_.lower_bound(address);
    if (next != regions_.end() && next->first < address + size) return MemoryError::overlap;
    if (next != regions_.begin()) {
        const auto& previous = *std::prev(next);
        if (address - previous.first < previous.second.bytes.size()) return MemoryError::overlap;
    }
    if (size > budget_ - used_) return MemoryError::budget;
    try {
        regions_.emplace(address, Region{MemoryBuffer(size), p});
    } catch (const std::bad_alloc&) { return MemoryError::allocation;
    } catch (const std::length_error&) { return MemoryError::allocation; }
    used_ += size;
    if (used_ > peak_used_) peak_used_ = used_;
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

std::size_t GuestMemory::peak_used() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return peak_used_;
}

bool GuestMemory::query_region(std::uint64_t address, std::uint64_t& base, std::size_t& size, unsigned& perm) const {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = regions_.upper_bound(address);
    if (it == regions_.begin()) return false;
    --it;
    if (address < it->first || address >= it->first + it->second.bytes.size()) return false;
    base = it->first;
    size = it->second.bytes.size();
    perm = it->second.permissions;
    return true;
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
