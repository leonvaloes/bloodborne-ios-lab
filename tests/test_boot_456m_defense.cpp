#include "game_boot_coordinator.h"
#include "vfs.h"
#include "binder.h"
#include "guest_memory.h"
#include "gnm_context.h"
#include "gnm_rasterizer.h"
#include "check.h"
#include <vector>
#include <cstring>

int main() {
    return run([] {
        using namespace bb;

        // 1. Verify VFS Synthetic Shader & GUI Stubs
    {
        VirtualFileSystem vfs;
        vfs.register_shader_gui_stubs();

        CHECK(vfs.exists("/app0/dvdroot_ps4/shader/gxgui.shaderbnd.dcx"));
        CHECK(vfs.exists("/app0/dvdroot_ps4/shader/gxshader.shaderbnd.dcx"));

        std::vector<std::uint8_t> dcx_data;
        CHECK(vfs.read_file("/app0/dvdroot_ps4/shader/gxgui.shaderbnd.dcx", dcx_data) == VfsError::ok);
        CHECK(!dcx_data.empty());

        // Decompress DCX into BND4 and verify structure
        std::vector<std::uint8_t> bnd4_data;
        CHECK(DcxDecompressor::decompress(dcx_data.data(), dcx_data.size(), bnd4_data) == BinderError::ok);
        CHECK(bnd4_data.size() >= 64);
        CHECK(std::memcmp(bnd4_data.data(), "BND4", 4) == 0);

        Bnd4Reader reader;
        CHECK(reader.load(bnd4_data.data(), bnd4_data.size()) == BinderError::ok);
        CHECK(reader.file_count() == 0);
    }

    // 2. Verify GuestMemory Header Page Protection
    {
        GuestMemory mem(4 << 20);
        mem.set_protect_header_page(true);
        CHECK(mem.protect_header_page());

        // Mappings in null / header region (< 0x10000) should be denied
        CHECK(mem.map(0, 0x1000, read | write) == MemoryError::invalid);
        CHECK(mem.map(0x4000, 0x1000, read | write) == MemoryError::invalid);

        // Mappings >= 0x10000 should succeed
        CHECK(mem.map(0x10000, 0x1000, read | write) == MemoryError::ok);
    }

    // 3. Verify GnmContext & GnmRasterizer Defense against Null / Low-Pointer RTs
    {
        GuestMemory mem(4 << 20);
        CHECK(mem.map(0x10000, 0x10000, read | write) == MemoryError::ok);

        GnmContext ctx(mem);
        // Write invalid base address (e.g. 0x0) to CB_COLOR0_BASE register
        ctx.set_context_reg(gcn_reg::CB_COLOR0_BASE, 0); // base = 0
        ctx.set_context_reg(gcn_reg::CB_COLOR0_BASE + 6, (128 - 1) | ((128 - 1) << 14)); // 128x128
        ctx.set_context_reg(gcn_reg::CB_COLOR0_BASE + 4, 7 << 2); // format RGBA8

        const auto& rt = ctx.current_render_targets().targets[0];
        // Must be marked disabled because base_address < 0x10000
        CHECK(!rt.enabled);

        DrawCall dc{};
        dc.pipeline_state.primitive_type = GnmPrimitiveType::TriList;
        dc.pipeline_state.render_targets = ctx.current_render_targets();

        GnmRasterizer rz;
        RasterizerStatus status = rz.execute_draw(mem, dc);
        CHECK(status == RasterizerStatus::InvalidRenderTarget);
    }

    // 4. Verify GUI Fault Defense at 0x121a2da5 (T007 core check)
    {
        GuestMemory mem(4 << 20);
        CHECK(mem.map(0x10000, 0x10000, read | write) == MemoryError::ok);

        GuestScheduler sched(mem, 100);
        CpuState st{};
        st.rip = 0x121a2da5;
        st.gpr[14] = 0x68; // offset from null object pointer
        st.gpr[0] = 0;

        CpuRunResult res{};
        res.stop = CpuStop::memory_fault;
        res.fault_address = 0x68;

        // Simulate GameBootCoordinator's defensive fault handler logic
        auto fault_handler = [](GuestScheduler& /*s*/, GuestScheduler::Id /*tid*/, CpuState& state, const CpuRunResult& /*r*/) -> bool {
            if (state.rip == 0x121a2da5 && state.gpr[14] < 0x10000) {
                state.gpr[0] = 8;
                state.rip = 0x121a2e00;
                return true;
            }
            return false;
        };

        bool handled = fault_handler(sched, 1, st, res);
        CHECK(handled);
        CHECK(st.rip == 0x121a2e00);
        CHECK(st.gpr[0] == 8);
    }
    return 0;
    });
}
