#include "gnm_pm4.h"
#include <limits>

namespace bb {
GnmPm4::GnmPm4(const GuestMemory& memory, Pm4Sink& sink, Pm4Limits limits)
    : memory_(memory), sink_(sink), limits_(limits) {}
std::uint32_t GnmPm4::context(std::uint32_t i) const { return i < context_.size() ? context_[i] : 0; }
std::uint32_t GnmPm4::shader(std::uint32_t i, bool cs) const {
    const auto& regs = cs ? shader_compute_ : shader_graphics_;
    return i < regs.size() ? regs[i] : 0;
}
std::uint32_t GnmPm4::uconfig(std::uint32_t i) const { return i < uconfig_.size() ? uconfig_[i] : 0; }
bool GnmPm4::word(std::uint64_t a, std::uint32_t& v) const {
    std::uint8_t bytes[4];
    if (memory_.load(a, bytes, 4) != MemoryError::ok) return false;
    v = std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) |
        (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
    return true;
}
Pm4Status GnmPm4::process(std::uint64_t a, std::uint32_t n) {
    consumed_ = packets_ = 0;
    if (!n || a % 4 || !limits_.max_depth || limits_.max_depth > stack_address_.size()) return Pm4Status::invalid;
    return buffer(a, n, 0);
}
Pm4Status GnmPm4::buffer(std::uint64_t a, std::uint32_t n, std::uint32_t depth) {
    if (depth >= limits_.max_depth) return Pm4Status::limit;
    if (!n || a % 4 || a > UINT64_MAX - std::uint64_t(n) * 4) return Pm4Status::invalid;
    if (n > limits_.max_words - consumed_) return Pm4Status::limit;
    for (std::uint32_t i = 0; i < depth; ++i)
        if (stack_address_[i] == a && stack_words_[i] == n) return Pm4Status::cycle;
    stack_address_[depth] = a;
    stack_words_[depth] = n;
    consumed_ += n;
    // Preflight de cada DWORD: regiões separadas e lacunas não podem virar zeros silenciosos.
    std::uint32_t probe = 0;
    for (std::uint32_t i = 0; i < n; ++i)
        if (!word(a + std::uint64_t(i) * 4, probe)) return Pm4Status::unmapped;
    std::uint32_t pos = 0;
    while (pos < n) {
        if (++packets_ > limits_.max_packets) return Pm4Status::limit;
        std::uint32_t h = 0;
        if (!word(a + std::uint64_t(pos) * 4, h)) return Pm4Status::unmapped;
        const auto type = h >> 30;
        if (type == 1) return Pm4Status::unsupported;
        const std::uint32_t count = type == 2 ? 0 : ((h >> 16) & 0x3fff) + 1;
        if (count > n - pos - 1) return Pm4Status::truncated;
        const auto body = a + std::uint64_t(pos + 1) * 4;
        if (type == 0) {
            const auto base = h & 0xffff;
            if (base >= context_.size() || count > context_.size() - base) return Pm4Status::invalid;
            for (std::uint32_t i = 0; i < count; ++i) {
                if (!word(body + std::uint64_t(i) * 4, context_[base + i])) return Pm4Status::unmapped;
                sink_.on_context_reg(base + i, context_[base + i]);
            }
        } else if (type == 3) {
            const auto op = (h >> 8) & 255;
            const auto compute = (h & 2) != 0;
            const auto result = packet(op, compute, body, count, depth);
            if (result != Pm4Status::ok) return result;
        }
        pos += count + 1;
    }
    return Pm4Status::ok;
}
Pm4Status GnmPm4::packet(std::uint32_t op, bool compute, std::uint64_t body,
                          std::uint32_t count, std::uint32_t depth) {
    auto get = [&](std::uint32_t i) { std::uint32_t v = 0; word(body + std::uint64_t(i) * 4, v); return v; };
    if (op == 0x69 || op == 0x76 || op == 0x79 || op == 0x68) {
        if (count < 2) return Pm4Status::invalid;
        auto& regs = op == 0x69 ? context_ : op == 0x79 ? uconfig_ :
                     op == 0x68 ? uconfig_ : compute ? shader_compute_ : shader_graphics_;
        const auto offset = get(0) & 0xffff;
        if (offset >= regs.size() || count - 1 > regs.size() - offset) return Pm4Status::invalid;
        for (std::uint32_t i = 1; i < count; ++i) {
            const auto val = get(i);
            const auto reg_idx = offset + i - 1;
            regs[reg_idx] = val;
            if (op == 0x69) {
                sink_.on_context_reg(reg_idx, val);
            } else if (op == 0x76) {
                sink_.on_sh_reg(reg_idx, val, compute);
            } else {
                sink_.on_uconfig_reg(reg_idx, val);
            }
        }
        return Pm4Status::ok;
    }
    if (op == 0x3f || op == 0x33) {
        if (count != 3) return Pm4Status::invalid;
        const auto addr = (std::uint64_t(get(1) & 0xffff) << 32) | get(0);
        return buffer(addr, get(2) & 0xfffff, depth + 1);
    }
    Pm4Event e{};
    e.opcode = op;
    e.words = count;
    std::uint32_t minimum = 0;
    switch (op) {
    case 0x10: e.kind = Pm4Kind::nop; break;
    case 0x27: e.kind = Pm4Kind::draw_indexed; minimum = 5; break;
    case 0x2d: e.kind = Pm4Kind::draw_auto; minimum = 2; break;
    case 0x15: e.kind = Pm4Kind::dispatch; minimum = 4; break;
    case 0x46: e.kind = Pm4Kind::event_write; minimum = 1; break;
    case 0x47: e.kind = Pm4Kind::event_eop; minimum = 5; break;
    case 0x48: e.kind = Pm4Kind::event_eos; minimum = 4; break;
    case 0x49: e.kind = Pm4Kind::release_mem; minimum = 6; break;
    case 0x37: e.kind = Pm4Kind::write_data; minimum = 3; break;
    case 0x50: case 0x41: e.kind = Pm4Kind::dma_data; minimum = 6; break;
    case 0x3c: e.kind = Pm4Kind::wait_reg_mem; minimum = 6; break;
    case 0x58: e.kind = Pm4Kind::acquire_mem; minimum = 6; break;
    default: return Pm4Status::unsupported; // nunca descartar um comando desconhecido silenciosamente
    }
    if (count < minimum) return Pm4Status::invalid;
    for (std::uint32_t i = 0; i < count && i < e.args.size(); ++i) e.args[i] = get(i);
    if (op == 0x10 && count && (e.args[0] == 0x68750776 || e.args[0] == 0x68750777 ||
        e.args[0] == 0x68750778 || e.args[0] == 0x68750780 || e.args[0] == 0x68750781)) {
        e.kind = Pm4Kind::flip;
        if (count < 2) return Pm4Status::invalid;
    }
    if (op == 0x27) e.address = (std::uint64_t(get(2) & 0xffff) << 32) | get(1);
    if (op == 0x37) e.address = (std::uint64_t(get(2)) << 32) | get(1);
    if (op == 0x47 || op == 0x48) e.address = (std::uint64_t(get(2) & 0xffff) << 32) | get(1);
    sink_.on_pm4_event(e);
    return Pm4Status::ok;
}
}
