#pragma once
#include "guest_memory.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace bb {
// PM4 usa offsets em DWORD a partir das bases MMIO GCN (0xA000/0x2C00/0xC000).
enum class Pm4Status { ok, invalid, truncated, unmapped, limit, cycle, unsupported };
enum class Pm4Kind { draw_indexed, draw_auto, dispatch, flip, event_write, event_eop,
                     event_eos, release_mem, write_data, dma_data, wait_reg_mem, acquire_mem, nop };
struct Pm4Event {
    Pm4Kind kind;
    std::uint32_t opcode = 0;
    std::uint64_t address = 0;
    std::array<std::uint32_t, 8> args{};
    std::uint32_t words = 0; // payload original, não copiado para o host
};
class Pm4Sink {
public:
    virtual ~Pm4Sink() = default;
    virtual void on_pm4_event(const Pm4Event& event) = 0;
    virtual void on_context_reg(std::uint32_t /*offset*/, std::uint32_t /*value*/) {}
    virtual void on_sh_reg(std::uint32_t /*offset*/, std::uint32_t /*value*/, bool /*compute*/) {}
    virtual void on_uconfig_reg(std::uint32_t /*offset*/, std::uint32_t /*value*/) {}
};
struct Pm4Limits {
    std::uint32_t max_words = 1'048'576; // soma de todos os IBs
    std::uint32_t max_packets = 65'536;
    std::uint32_t max_depth = 8;
};
class GnmPm4 {
public:
    explicit GnmPm4(const GuestMemory& memory, Pm4Sink& sink, Pm4Limits limits = {});
    Pm4Status process(std::uint64_t address, std::uint32_t words);
    // Offsets em DWORD (não endereços de bytes). UCONFIG e SH de computação separados.
    std::uint32_t context(std::uint32_t offset) const;
    std::uint32_t shader(std::uint32_t offset, bool compute = false) const;
    std::uint32_t uconfig(std::uint32_t offset) const;
private:
    Pm4Status buffer(std::uint64_t address, std::uint32_t words, std::uint32_t depth);
    Pm4Status packet(std::uint32_t opcode, bool compute, std::uint64_t body,
                     std::uint32_t count, std::uint32_t depth);
    bool word(std::uint64_t address, std::uint32_t& value) const;
    const GuestMemory& memory_;
    Pm4Sink& sink_;
    Pm4Limits limits_;
    std::array<std::uint32_t, 0x4000> context_{};
    std::array<std::uint32_t, 0x4000> shader_graphics_{};
    std::array<std::uint32_t, 0x4000> shader_compute_{};
    std::array<std::uint32_t, 0x4000> uconfig_{};
    std::uint32_t consumed_ = 0, packets_ = 0;
    std::array<std::uint64_t, 64> stack_address_{};
    std::array<std::uint32_t, 64> stack_words_{};
};
}
