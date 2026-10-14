#pragma once
#include "gcn_ir.h"

namespace bb::gcn {
// Synthetic lane-local register-file contract, NOT a wavefront/graphics renderer.
// Compute reads/writes one register file per thread; vertex returns a diagnostic
// position from v0..v3. No PS4 stage IO, resources, exports or EXEC masking.
MslResult translate_msl(const ProgramIR& ir, bool vertex);
} // namespace bb::gcn
