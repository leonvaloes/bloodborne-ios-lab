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

class MemoryStorage {
public:
    explicit MemoryStorage(std::size_t size) : size_(size) {
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

    ~MemoryStorage() {
        if (!data_) return;
#if defined(_WIN32)
        ::VirtualFree(data_, 0, MEM_RELEASE);
#else
        ::munmap(data_, size_);
#endif
    }

    MemoryStorage(const MemoryStorage&) = delete;
    MemoryStorage& operator=(const MemoryStorage&) = delete;

    std::uint8_t* data() noexcept { return data_; }
    const std::uint8_t* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }

private:
    std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
};

MemoryBuffer::MemoryBuffer(std::size_t size)
    : storage_(size > 0 ? std::make_shared<MemoryStorage>(size) : nullptr),
      offset_(0),
      size_(size) {}

MemoryBuffer::MemoryBuffer(std::shared_ptr<MemoryStorage> storage, std::size_t offset, std::size_t size) noexcept
    : storage_(std::move(storage)), offset_(offset), size_(size) {}

MemoryBuffer::~MemoryBuffer() = default;

MemoryBuffer::MemoryBuffer(MemoryBuffer&& other) noexcept
    : storage_(std::move(other.storage_)), offset_(other.offset_), size_(other.size_) {
    other.offset_ = 0;
    other.size_ = 0;
}

MemoryBuffer& MemoryBuffer::operator=(MemoryBuffer&& other) noexcept {
    if (this != &other) {
        storage_ = std::move(other.storage_);
        offset_ = other.offset_;
        size_ = other.size_;
        other.offset_ = 0;
        other.size_ = 0;
    }
    return *this;
}

MemoryBuffer MemoryBuffer::slice(std::size_t offset, std::size_t size) const {
    if (offset >= size_ || size > size_ - offset) {
        return MemoryBuffer();
    }
    return MemoryBuffer(storage_, offset_ + offset, size);
}

std::uint8_t* MemoryBuffer::data() noexcept {
    return storage_ ? storage_->data() + offset_ : nullptr;
}

const std::uint8_t* MemoryBuffer::data() const noexcept {
    return storage_ ? storage_->data() + offset_ : nullptr;
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

MemoryError GuestMemory::unmap(std::uint64_t address, std::size_t size) {
    if (size == 0 || !valid_span(address, size)) return MemoryError::invalid;
    std::lock_guard<std::mutex> guard(mutex_);

    const std::uint64_t unmap_start = address;
    const std::uint64_t unmap_end = address + size;

    std::vector<std::uint64_t> to_remove;
    std::vector<std::pair<std::uint64_t, Region>> to_add;
    bool any_overlap = false;

    for (auto& pair : regions_) {
        const std::uint64_t reg_start = pair.first;
        const std::size_t reg_size = pair.second.bytes.size();
        const std::uint64_t reg_end = reg_start + reg_size;

        if (reg_start >= unmap_end) {
            break;
        }
        if (reg_end <= unmap_start) {
            continue;
        }

        any_overlap = true;
        to_remove.push_back(reg_start);

        const unsigned perm = pair.second.permissions;

        if (unmap_start > reg_start && unmap_end < reg_end) {
            const std::size_t prefix_size = static_cast<std::size_t>(unmap_start - reg_start);
            to_add.emplace_back(reg_start, Region{pair.second.bytes.slice(0, prefix_size), perm});

            const std::size_t suffix_size = static_cast<std::size_t>(reg_end - unmap_end);
            to_add.emplace_back(unmap_end, Region{pair.second.bytes.slice(unmap_end - reg_start, suffix_size), perm});

            used_ -= size;
        } else if (unmap_start <= reg_start && unmap_end < reg_end) {
            const std::size_t suffix_size = static_cast<std::size_t>(reg_end - unmap_end);
            to_add.emplace_back(unmap_end, Region{pair.second.bytes.slice(unmap_end - reg_start, suffix_size), perm});

            used_ -= (unmap_end - reg_start);
        } else if (unmap_start > reg_start && unmap_end >= reg_end) {
            const std::size_t prefix_size = static_cast<std::size_t>(unmap_start - reg_start);
            to_add.emplace_back(reg_start, Region{pair.second.bytes.slice(0, prefix_size), perm});

            used_ -= (reg_end - unmap_start);
        } else {
            used_ -= reg_size;
        }
    }

    if (!any_overlap) {
        return MemoryError::unmapped;
    }

    for (std::uint64_t addr : to_remove) {
        regions_.erase(addr);
    }
    for (auto& new_reg : to_add) {
        regions_.emplace(new_reg.first, std::move(new_reg.second));
    }

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

MemoryError GuestMemory::protect(std::uint64_t address, std::size_t size, unsigned p) {
    if (size == 0 || !valid_span(address, size)) return MemoryError::invalid;
    if (!valid_permissions(p)) return MemoryError::invalid;
    std::lock_guard<std::mutex> guard(mutex_);

    const std::uint64_t prot_start = address;
    const std::uint64_t prot_end = address + size;

    std::uint64_t cursor = prot_start;
    while (cursor < prot_end) {
        auto it = find(cursor, 1);
        if (it == regions_.end()) return MemoryError::unmapped;
        cursor = it->first + it->second.bytes.size();
    }

    std::vector<std::uint64_t> to_remove;
    std::vector<std::pair<std::uint64_t, Region>> to_add;

    for (auto& pair : regions_) {
        const std::uint64_t reg_start = pair.first;
        const std::size_t reg_size = pair.second.bytes.size();
        const std::uint64_t reg_end = reg_start + reg_size;

        if (reg_start >= prot_end) break;
        if (reg_end <= prot_start) continue;

        to_remove.push_back(reg_start);
        const unsigned old_perm = pair.second.permissions;

        if (prot_start > reg_start && prot_end < reg_end) {
            const std::size_t prefix_size = static_cast<std::size_t>(prot_start - reg_start);
            to_add.emplace_back(reg_start, Region{pair.second.bytes.slice(0, prefix_size), old_perm});

            const std::size_t mid_size = static_cast<std::size_t>(prot_end - prot_start);
            to_add.emplace_back(prot_start, Region{pair.second.bytes.slice(prefix_size, mid_size), p});

            const std::size_t suffix_size = static_cast<std::size_t>(reg_end - prot_end);
            to_add.emplace_back(prot_end, Region{pair.second.bytes.slice(prot_end - reg_start, suffix_size), old_perm});
        } else if (prot_start <= reg_start && prot_end < reg_end) {
            const std::size_t mid_size = static_cast<std::size_t>(prot_end - reg_start);
            to_add.emplace_back(reg_start, Region{pair.second.bytes.slice(0, mid_size), p});

            const std::size_t suffix_size = static_cast<std::size_t>(reg_end - prot_end);
            to_add.emplace_back(prot_end, Region{pair.second.bytes.slice(mid_size, suffix_size), old_perm});
        } else if (prot_start > reg_start && prot_end >= reg_end) {
            const std::size_t prefix_size = static_cast<std::size_t>(prot_start - reg_start);
            to_add.emplace_back(reg_start, Region{pair.second.bytes.slice(0, prefix_size), old_perm});

            const std::size_t mid_size = static_cast<std::size_t>(reg_end - prot_start);
            to_add.emplace_back(prot_start, Region{pair.second.bytes.slice(prefix_size, mid_size), p});
        } else {
            to_add.emplace_back(reg_start, Region{std::move(pair.second.bytes), p});
        }
    }

    for (std::uint64_t addr : to_remove) {
        regions_.erase(addr);
    }
    for (auto& new_reg : to_add) {
        regions_.emplace(new_reg.first, std::move(new_reg.second));
    }

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

    const std::uint64_t end = address + size;
    std::uint64_t cur = address;
    while (cur < end) {
        auto it = regions_.upper_bound(cur);
        if (it == regions_.begin()) return MemoryError::unmapped;
        --it;
        const std::uint64_t reg_start = it->first;
        const std::size_t reg_size = it->second.bytes.size();
        const std::uint64_t reg_end = reg_start + reg_size;
        if (cur < reg_start || cur >= reg_end) {
            return MemoryError::unmapped;
        }
        if (!(it->second.permissions & read)) {
            return MemoryError::denied;
        }
        cur = reg_end;
    }

    cur = address;
    std::size_t dest_offset = 0;
    while (cur < end) {
        auto it = regions_.upper_bound(cur);
        --it;
        const std::uint64_t reg_start = it->first;
        const std::size_t reg_size = it->second.bytes.size();
        const std::uint64_t reg_end = reg_start + reg_size;
        const std::size_t step = static_cast<std::size_t>(std::min<std::uint64_t>(end - cur, reg_end - cur));
        const std::size_t src_offset = static_cast<std::size_t>(cur - reg_start);
        std::memcpy(static_cast<std::uint8_t*>(destination) + dest_offset,
                    it->second.bytes.data() + src_offset,
                    step);
        dest_offset += step;
        cur += step;
    }
    return MemoryError::ok;
}

MemoryError GuestMemory::store(std::uint64_t address, const void* source, std::size_t size) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!source || !valid_span(address, size)) return MemoryError::invalid;

    const std::uint64_t end = address + size;
    std::uint64_t cur = address;
    while (cur < end) {
        auto it = regions_.upper_bound(cur);
        if (it == regions_.begin()) return MemoryError::unmapped;
        --it;
        const std::uint64_t reg_start = it->first;
        const std::size_t reg_size = it->second.bytes.size();
        const std::uint64_t reg_end = reg_start + reg_size;
        if (cur < reg_start || cur >= reg_end) {
            return MemoryError::unmapped;
        }
        if (!(it->second.permissions & write)) {
            return MemoryError::denied;
        }
        cur = reg_end;
    }

    cur = address;
    std::size_t src_offset = 0;
    while (cur < end) {
        auto it = regions_.upper_bound(cur);
        --it;
        const std::uint64_t reg_start = it->first;
        const std::size_t reg_size = it->second.bytes.size();
        const std::uint64_t reg_end = reg_start + reg_size;
        const std::size_t step = static_cast<std::size_t>(std::min<std::uint64_t>(end - cur, reg_end - cur));
        const std::size_t dst_offset = static_cast<std::size_t>(cur - reg_start);
        std::memcpy(it->second.bytes.data() + dst_offset,
                    static_cast<const std::uint8_t*>(source) + src_offset,
                    step);
        src_offset += step;
        cur += step;
    }
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
