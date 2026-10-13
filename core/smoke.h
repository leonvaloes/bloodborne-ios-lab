#pragma once
#include <cstdint>
namespace bb {
struct SmokeReport { std::uint32_t checks, failures; };
SmokeReport smoke();
}
