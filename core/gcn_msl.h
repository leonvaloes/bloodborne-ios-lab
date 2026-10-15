#pragma once
#include "gcn_ir.h"
#include "gnm_context.h"

namespace bb::gcn {
// Synthetic lane-local register-file contract, NOT a wavefront/graphics renderer.
// Compute reads/writes one register file per thread; vertex returns a diagnostic
// position from v0..v3; fragment returns a color from v0..v3 into [[color(0)]].
MslResult translate_msl(const ProgramIR& ir, bool vertex);
MslResult translate_msl(const ProgramIR& ir, ShaderStage stage);
} // namespace bb::gcn
