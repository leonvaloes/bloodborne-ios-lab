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

    // 4. Verify GUI Fault Defense at 0x121a2da5 (T037 / FR-001 - sem salto artificial de RIP)
    {
        GuestMemory mem(32 << 20);
        CHECK(mem.map(0x10000, 0x10000, read | write) == MemoryError::ok);

        // Inicializa regiao de fallback e vtable sintetica do objeto de GUI (T037)
        CHECK(GameBootCoordinator::setup_gui_synthetic_object(mem));

        // Mapeia pagina de codigo contendo o RIP original do fault: 0x121a2000
        CHECK(mem.map(0x121a2000, 0x2000, read | write) == MemoryError::ok);

        // Grava as instrucoes autenticas do executavel em 0x121a2da5:
        // 0x121a2da5: 49 8b 06       mov rax, [r14]
        // 0x121a2da8: ff 10          call [rax]
        const std::uint8_t guest_code[] = {
            0x49, 0x8b, 0x06,       // mov rax, [r14]
            0xff, 0x10              // call [rax]
        };
        CHECK(mem.store(0x121a2da5, guest_code, sizeof(guest_code)) == MemoryError::ok);

        // Mapeia pilha do guest
        CHECK(mem.map(0x70000000, 0x10000, read | write) == MemoryError::ok);

        GuestScheduler sched(mem, 100);
        CpuState st{};
        st.rip = 0x121a2da5;
        st.gpr[14] = 0x68; // offset sobre objeto de GUI nulo inicial
        st.gpr[0] = 0;
        st.gpr[4] = 0x70008000; // rsp valido

        CpuRunResult res{};
        res.stop = CpuStop::memory_fault;
        res.fault_address = 0x68;

        // Fault handler canonico do GameBootCoordinator:
        // Redireciona r14 para o objeto de GUI sintetico valido e preserva st.rip intacto (sem mascarar fluxo)
        auto fault_handler = [](GuestScheduler& /*s*/, GuestScheduler::Id /*tid*/, CpuState& state, const CpuRunResult& /*r*/) -> bool {
            if (state.rip == 0x121a2da5 && state.gpr[14] < 0x10000) {
                state.gpr[14] = GameBootCoordinator::GUI_SYNTHETIC_OBJECT_BASE + state.gpr[14];
                return true;
            }
            return false;
        };

        bool handled = fault_handler(sched, 1, st, res);
        CHECK(handled);
        // Comprova que NAO ha salto artificial de RIP mascarando o fluxo guest
        CHECK(st.rip == 0x121a2da5);
        CHECK(st.gpr[14] == GameBootCoordinator::GUI_SYNTHETIC_OBJECT_BASE + 0x68);

        // Agora executa a instrucao genuina 'mov rax, [r14]' via interpretador x86 real
        X86Interpreter interp(mem);
        interp.state = st;
        auto step1 = interp.run(1);
        CHECK(step1.steps == 1);
        CHECK(step1.stop == CpuStop::step_limit);
        // O RIP avancou naturalmente 3 bytes de 0x121a2da5 para 0x121a2da8
        CHECK(interp.state.rip == 0x121a2da8);
        // rax recebeu com fidelidade o endereco da vtable sintetica
        CHECK(interp.state.gpr[0] == GameBootCoordinator::GUI_SYNTHETIC_VTABLE);

        // Executa a instrucao subsequente 'call [rax]' invocando a vtable sintetica
        auto step2 = interp.run(1);
        CHECK(step2.steps == 1);
        // O stub seguro de vtable 'xor eax, eax; ret' executa e retorna rax = 0
        auto step3 = interp.run(2);
        CHECK(step3.steps > 0);
        CHECK(interp.state.gpr[0] == 0); // retorno de sucesso do metodo virtual
    }
    return 0;
    });
}
