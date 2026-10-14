#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <map>

namespace bb {

// PS4 ELF / SELF Structures
constexpr std::uint32_t SELF_MAGIC = 0x1D3D154F; // "O=" little-endian
constexpr std::uint32_t SFO_MAGIC = 0x46535000;  // "\0PSF" little-endian
constexpr std::uint16_t EM_X86_64 = 62;

struct ElfProgramHeader {
    std::uint32_t type;
    std::uint32_t flags;
    std::uint64_t offset;
    std::uint64_t vaddr;
    std::uint64_t paddr;
    std::uint64_t filesz;
    std::uint64_t memsz;
    std::uint64_t align;

    bool is_readable() const { return (flags & 4) != 0; }
    bool is_writable() const { return (flags & 2) != 0; }
    bool is_executable() const { return (flags & 1) != 0; }
};

struct GameMetadata {
    std::string title;
    std::string title_id;
    std::string app_ver;
    std::string content_id;
    std::map<std::string, std::string> entries;
};

struct ExecutableInspection {
    bool is_valid = false;
    bool is_self = false;
    std::uint16_t machine = 0;
    std::uint64_t entry_point = 0;
    std::vector<ElfProgramHeader> program_headers;
    std::uint64_t total_memory_size = 0;
    std::uint64_t code_size = 0;
    std::uint64_t data_size = 0;

    std::string to_json() const;
};

class SafeGameParser {
public:
    static GameMetadata parse_sfo(const std::uint8_t* data, std::size_t size);
    static ExecutableInspection inspect_executable(const std::uint8_t* data, std::size_t size);
};

} // namespace bb
