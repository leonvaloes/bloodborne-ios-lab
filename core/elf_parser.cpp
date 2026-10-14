#include "elf_parser.h"
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <limits>

namespace bb {

namespace {
inline bool check_span(std::size_t offset, std::size_t length, std::size_t total) {
    return offset <= total && length <= total - offset;
}
}

GameMetadata SafeGameParser::parse_sfo(const std::uint8_t* data, std::size_t size) {
    if (!data || size < 20) throw std::runtime_error("SFO data too small");
    std::uint32_t magic = 0;
    std::memcpy(&magic, data, 4);
    if (magic != SFO_MAGIC) throw std::runtime_error("Invalid SFO magic header");

    std::uint32_t key_table_offset = 0, val_table_offset = 0, count = 0;
    std::memcpy(&key_table_offset, data + 8, 4);
    std::memcpy(&val_table_offset, data + 12, 4);
    std::memcpy(&count, data + 16, 4);

    if (key_table_offset >= size || val_table_offset >= size)
        throw std::runtime_error("Corrupted SFO table offsets");

    GameMetadata meta;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::size_t entry_offset = 20 + i * 16;
        if (!check_span(entry_offset, 16, size)) break;

        std::uint16_t key_off = 0, fmt = 0;
        std::uint32_t data_len = 0, data_max = 0, data_off = 0;
        std::memcpy(&key_off, data + entry_offset, 2);
        std::memcpy(&fmt, data + entry_offset + 2, 2);
        std::memcpy(&data_len, data + entry_offset + 4, 4);
        std::memcpy(&data_max, data + entry_offset + 8, 4);
        std::memcpy(&data_off, data + entry_offset + 12, 4);

        std::size_t k_pos = key_table_offset + key_off;
        if (k_pos >= size) continue;
        std::string key;
        while (k_pos < size && data[k_pos] != 0) {
            key += static_cast<char>(data[k_pos++]);
        }

        std::size_t v_pos = val_table_offset + data_off;
        if (!check_span(v_pos, data_len, size)) continue;

        std::string val;
        if (fmt == 0x0404 && data_len == 4) { // uint32
            std::uint32_t num = 0;
            std::memcpy(&num, data + v_pos, 4);
            val = std::to_string(num);
        } else {
            val = std::string(reinterpret_cast<const char*>(data + v_pos), data_len);
            while (!val.empty() && val.back() == 0) val.pop_back();
        }

        meta.entries[key] = val;
        if (key == "TITLE") meta.title = val;
        if (key == "TITLE_ID") meta.title_id = val;
        if (key == "APP_VER") meta.app_ver = val;
        if (key == "CONTENT_ID") meta.content_id = val;
    }
    return meta;
}

ExecutableInspection SafeGameParser::inspect_executable(const std::uint8_t* data, std::size_t size) {
    ExecutableInspection insp{};
    if (!data || size < 64) return insp;

    std::size_t elf_offset = 0;
    std::uint32_t magic = 0;
    std::memcpy(&magic, data, 4);

    if (magic == SELF_MAGIC) {
        insp.is_self = true;
        if (size < 32) return insp;
        std::uint16_t segment_count = 0;
        std::memcpy(&segment_count, data + 24, 2);
        elf_offset = 32 + static_cast<std::size_t>(segment_count) * 32;
        if (!check_span(elf_offset, 64, size)) return insp;
    }

    const std::uint8_t* elf = data + elf_offset;
    std::size_t elf_avail = size - elf_offset;

    if (elf[0] != 0x7f || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F') {
        return insp;
    }

    std::uint16_t machine = 0;
    std::memcpy(&machine, elf + 18, 2);
    insp.machine = machine;

    std::uint64_t entry_point = 0, phoff = 0;
    std::uint16_t phentsize = 0, phnum = 0;
    std::memcpy(&entry_point, elf + 24, 8);
    std::memcpy(&phoff, elf + 32, 8);
    std::memcpy(&phentsize, elf + 54, 2);
    std::memcpy(&phnum, elf + 56, 2);

    insp.entry_point = entry_point;

    if (phentsize < 56 || phnum == 0 || phnum > 256) return insp;
    if (!check_span(phoff, static_cast<std::size_t>(phnum) * phentsize, elf_avail)) return insp;

    for (std::uint16_t i = 0; i < phnum; ++i) {
        std::size_t pos = phoff + i * phentsize;
        ElfProgramHeader ph{};
        std::memcpy(&ph.type, elf + pos, 4);
        std::memcpy(&ph.flags, elf + pos + 4, 4);
        std::memcpy(&ph.offset, elf + pos + 8, 8);
        std::memcpy(&ph.vaddr, elf + pos + 16, 8);
        std::memcpy(&ph.paddr, elf + pos + 24, 8);
        std::memcpy(&ph.filesz, elf + pos + 32, 8);
        std::memcpy(&ph.memsz, elf + pos + 40, 8);
        std::memcpy(&ph.align, elf + pos + 48, 8);

        insp.program_headers.push_back(ph);

        if (ph.type == 1) { // PT_LOAD
            insp.total_memory_size += ph.memsz;
            if (ph.is_executable()) insp.code_size += ph.memsz;
            else insp.data_size += ph.memsz;
        }
    }

    insp.is_valid = true;
    return insp;
}

std::string ExecutableInspection::to_json() const {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"is_valid\": " << (is_valid ? "true" : "false") << ",\n";
    ss << "  \"is_self\": " << (is_self ? "true" : "false") << ",\n";
    ss << "  \"machine\": " << machine << ",\n";
    ss << "  \"entry_point\": " << entry_point << ",\n";
    ss << "  \"segments_count\": " << program_headers.size() << ",\n";
    ss << "  \"total_memory_size\": " << total_memory_size << ",\n";
    ss << "  \"code_size\": " << code_size << ",\n";
    ss << "  \"data_size\": " << data_size << "\n";
    ss << "}\n";
    return ss.str();
}

} // namespace bb
