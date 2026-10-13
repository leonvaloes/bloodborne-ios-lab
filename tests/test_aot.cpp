#include "check.h"
#include "aot.h"
#include <random>

template<class F> bool rejects(F f) { try { f(); return false; } catch (const std::runtime_error&) { return true; } }
int main() { return run([] {
    using namespace bb;
    const auto valid = parse_hex("48 89 f8 48 01 f0 c3\n");
    CHECK(valid.size() == 7);
    CHECK(translate(valid, "fixture_add").find("add64") != std::string::npos);
    CHECK(rejects([&] { translate(valid, "injected(){}"); }));
    CHECK(rejects([] { parse_hex("0x48"); }));
    CHECK(rejects([] { parse_hex("gg"); }));
    CHECK(rejects([] { parse_hex("4"); }));
    CHECK(rejects([] { parse_hex("-1"); }));
    CHECK(rejects([] { parse_hex("48x"); }));
    CHECK(rejects([] { translate({}, "fixture_add"); }));
    for (std::size_t length = 0; length < valid.size(); ++length) {
        const std::vector<std::uint8_t> truncated(valid.begin(), valid.begin() + length);
        CHECK(rejects([&] { translate(truncated, "fixture_add"); }));
    }
    auto extra = valid; extra.push_back(0xc3);
    CHECK(rejects([&] { translate(extra, "fixture_add"); }));
    // Exhaustively mutate each byte: only the three explicitly supported ALU
    // opcodes may pass. No best-effort acceptance of unrecognized instructions.
    for (unsigned index = 0; index < valid.size(); ++index) for (unsigned value = 0; value < 256; ++value) {
        auto mutation = valid; mutation[index] = static_cast<std::uint8_t>(value);
        const bool allowed = mutation == valid || (index == 4 && (value == 0x29 || value == 0x31));
        CHECK(rejects([&] { translate(mutation, "fixture_add"); }) != allowed);
    }
    std::mt19937 rng(1234);
    for (unsigned trial = 0; trial < 10000; ++trial) {
        std::vector<std::uint8_t> bytes(rng() % 64);
        for (auto& byte : bytes) byte = static_cast<std::uint8_t>(rng());
        CHECK(rejects([&] { translate(bytes, "fixture_add"); }));
    }
    std::cout << "decoder: malformed text, truncation, 1792 byte mutations and 10000 random inputs passed\n";
}); }
