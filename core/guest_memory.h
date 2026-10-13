#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace bb {
enum class MemoryError { ok, invalid, overlap, budget, unmapped, denied, allocation };
enum Permission : unsigned { read = 1, write = 2 };
// Logical guest memory only: never maps executable pages or casts guest
// addresses to host pointers. A transfer must fit a single region.
class GuestMemory {
public:
    explicit GuestMemory(std::size_t budget) : budget_(budget) {}
    MemoryError map(std::uint64_t address, std::size_t size, unsigned permissions);
    MemoryError unmap(std::uint64_t address);
    MemoryError protect(std::uint64_t address, unsigned permissions);
    MemoryError load(std::uint64_t address, void* destination, std::size_t size) const;
    MemoryError store(std::uint64_t address, const void* source, std::size_t size);
    std::size_t used() const;
private:
    struct Region { std::vector<std::uint8_t> bytes; unsigned permissions; };
    using Regions = std::map<std::uint64_t, Region>;
    Regions::const_iterator find(std::uint64_t address, std::size_t size) const;
    mutable std::mutex mutex_;
    Regions regions_;
    std::size_t budget_, used_ = 0;
};
}
