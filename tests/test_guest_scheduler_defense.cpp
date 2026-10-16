#include "guest_scheduler.h"
#include "check.h"
#include <cstdint>
#include <vector>

int main() {
    return run([] {
        using namespace bb;

        // 1. Setup guest memory with isolated pages
        GuestMemory mem(2 << 20); // 2 MB
        CHECK(mem.map(0x1000, 0x1000, read | write) == MemoryError::ok); // code page
        CHECK(mem.map(0x4000, 0x2000, read | write) == MemoryError::ok); // stack A
        CHECK(mem.map(0x7000, 0x2000, read | write) == MemoryError::ok); // stack B

        // Thread A code:
        // 0x1000: mov rax, [0x20]  (triggers memory_fault because 0x20 is unmapped)
        // 0x1008: add rax, 10
        // 0x100c: inc rbx
        // 0x100f: jmp 0x100c
        const std::uint8_t code_faulty[] = {
            0x48, 0x8b, 0x04, 0x25, 0x20, 0x00, 0x00, 0x00, // mov rax, [0x20] (8 bytes)
            0x48, 0x83, 0xc0, 0x0a,                         // add rax, 10      (4 bytes, at 0x1008)
            0x48, 0xff, 0xc3,                               // inc rbx          (3 bytes, at 0x100c)
            0xeb, 0xfb                                      // jmp 0x100c       (2 bytes, at 0x100f)
        };
        CHECK(mem.store(0x1000, code_faulty, sizeof(code_faulty)) == MemoryError::ok);

        GuestScheduler s(mem, 100);

        auto t1 = s.create(0x1000, 0x4000, 0x5000, 0x3000);
        CHECK(t1 != 0);
        CHECK(s.start(t1) == ORBIS_OK);

        // Test without fault handler: should stop with Stop::fault and memory_fault
        auto res_unhandled = s.run(10);
        CHECK(res_unhandled.stop == GuestScheduler::Stop::fault);
        CHECK(res_unhandled.cpu.stop == CpuStop::memory_fault);
        CHECK(res_unhandled.cpu.fault_address == 0x20);
        CHECK(s.cpu(t1).rip == 0x1000);
        CHECK(s.state(t1) == GuestScheduler::State::faulted);

        // Test with fault handler that recovers:
        // Set fault handler to redirect rip to 0x1008 and set rax = 42
        bool handler_called = false;
        s.set_fault_handler([&](GuestScheduler& /*sched*/, GuestScheduler::Id /*tid*/, CpuState& st, const CpuRunResult& r) -> bool {
            if (st.rip == 0x1000 && r.fault_address == 0x20) {
                handler_called = true;
                st.gpr[0] = 42;    // simulate recovery value
                st.rip = 0x1008;   // skip the faulting instruction
                return true;
            }
            return false;
        });

        // Create thread 2 running the same code
        auto t2 = s.create(0x1000, 0x7000, 0x8000, 0x3000);
        CHECK(t2 != 0);
        CHECK(s.start(t2) == ORBIS_OK);

        auto res_handled = s.run(20);
        CHECK(handler_called);
        // After skipping to 0x1008: add rax, 10 -> rax = 52, then loop inc rbx
        CHECK(res_handled.stop == GuestScheduler::Stop::budget);
        CHECK(s.cpu(t2).gpr[0] == 52);
        CHECK(s.cpu(t2).gpr[3] > 0); // rbx was incremented in the loop
        CHECK(res_handled.steps > 0);

        // Multi-threaded test: thread 3 advances normally while thread 2 loops
        // Thread 3 code at 0x1020: inc rcx; jmp 0x1020
        CHECK(mem.map(0x9000, 0x2000, read | write) == MemoryError::ok); // stack C
        const std::uint8_t code_t3[] = {
            0x48, 0xff, 0xc1,       // inc rcx
            0xeb, 0xfb             // jmp -5
        };
        CHECK(mem.store(0x1020, code_t3, sizeof(code_t3)) == MemoryError::ok);

        auto t3 = s.create(0x1020, 0x9000, 0xa000, 0x3000);
        CHECK(t3 != 0);
        CHECK(s.start(t3) == ORBIS_OK);

        auto res_multi = s.run(200);
        CHECK(res_multi.stop == GuestScheduler::Stop::budget);
        CHECK(s.cpu(t3).gpr[1] > 0); // rcx progressed
        CHECK(s.has_thread(t2));
        CHECK(s.has_thread(t3));
    });
}
