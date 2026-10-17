#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace bb {
enum class MemoryError { ok, invalid, overlap, budget, unmapped, denied, allocation };
enum Permission : unsigned { read = 1, write = 2 };

class MemoryStorage;

class MemoryBuffer {
public:
    MemoryBuffer() noexcept = default;
    explicit MemoryBuffer(std::size_t size);
    MemoryBuffer(std::shared_ptr<MemoryStorage> storage, std::size_t offset, std::size_t size) noexcept;
    ~MemoryBuffer();
    MemoryBuffer(const MemoryBuffer&) = delete;
    MemoryBuffer& operator=(const MemoryBuffer&) = delete;
    MemoryBuffer(MemoryBuffer&& other) noexcept;
    MemoryBuffer& operator=(MemoryBuffer&& other) noexcept;

    MemoryBuffer slice(std::size_t offset, std::size_t size) const;

    std::uint8_t* data() noexcept;
    const std::uint8_t* data() const noexcept;
    std::size_t size() const noexcept { return size_; }

private:
    std::shared_ptr<MemoryStorage> storage_;
    std::size_t offset_ = 0;
    std::size_t size_ = 0;
};

// Logical guest memory only: never maps executable pages or casts guest
// addresses to host pointers. A transfer must fit a single region.
class GuestMemory {
public:
    explicit GuestMemory(std::size_t budget) : budget_(budget) {}
    MemoryError map(std::uint64_t address, std::size_t size, unsigned permissions);
    MemoryError unmap(std::uint64_t address);
    MemoryError unmap(std::uint64_t address, std::size_t size);
    MemoryError protect(std::uint64_t address, unsigned permissions);
    MemoryError protect(std::uint64_t address, std::size_t size, unsigned permissions);
    MemoryError load(std::uint64_t address, void* destination, std::size_t size) const;
    MemoryError store(std::uint64_t address, const void* source, std::size_t size);
    std::size_t used() const;
    // Pico de bytes mapeados desde a criação; não é o footprint físico do processo.
    std::size_t peak_used() const;
    std::size_t budget() const { return budget_; }
    bool query_region(std::uint64_t address, std::uint64_t& base, std::size_t& size, unsigned& perm) const;


    // F5: Atomic memory operations (equivalent to x86 LOCK prefix)
    MemoryError atomic_fetch_add_u64(std::uint64_t address, std::uint64_t delta, std::uint64_t& old_val);
    MemoryError atomic_fetch_add_u32(std::uint64_t address, std::uint32_t delta, std::uint32_t& old_val);
    MemoryError atomic_compare_exchange_u64(std::uint64_t address, std::uint64_t& expected, std::uint64_t desired, bool& success);
    MemoryError atomic_compare_exchange_u32(std::uint64_t address, std::uint32_t& expected, std::uint32_t desired, bool& success);

    bool protect_header_page() const { return protect_header_page_; }
    void set_protect_header_page(bool enable) { protect_header_page_ = enable; }

private:
    struct Region { MemoryBuffer bytes; unsigned permissions; };
    using Regions = std::map<std::uint64_t, Region>;
    Regions::const_iterator find(std::uint64_t address, std::size_t size) const;
    mutable std::mutex mutex_;
    Regions regions_;
    std::size_t budget_, used_ = 0, peak_used_ = 0;
    bool protect_header_page_ = false;
};
}
