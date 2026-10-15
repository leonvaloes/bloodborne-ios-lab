// Diagnostico local: recebe dumps plaintext do usuario; nunca persiste bytes do jogo.
#include "../core/ps4_bootstrap.h"
#include "../core/orbis_runtime.h"
#include "../core/gnm_context.h"
#if defined(_WIN32)
#include <windows.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if (!file || file.tellg()<0 || file.tellg()>(512ll<<20)) throw std::runtime_error("arquivo ausente ou excessivo: "+path.string());
    std::vector<std::uint8_t> data(static_cast<std::size_t>(file.tellg()));
    file.seekg(0);file.read(reinterpret_cast<char*>(data.data()),static_cast<std::streamsize>(data.size()));
    if (!file) throw std::runtime_error("arquivo truncado: "+path.string());
    return data;
}
}
int main(int argc, char** argv) {
    std::vector<std::filesystem::path> args;
#if defined(_WIN32)
    int wargc = 0;
    if (auto* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc)) {
        for (int i = 0; i < wargc; ++i) args.emplace_back(wargv[i]);
        LocalFree(wargv);
    }
#endif
    if (args.empty()) {
        for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
    }
    if (args.size()<2) { std::fprintf(stderr,"uso: bb-run-eboot Image0 [max_passos] [--libc] [--hle] [--trace-hle]\n"); return 2; }
    try {
        std::uint64_t max_steps=50000000;
        bool linked=false, hle=false, trace_hle=false, budget_set=false, use_sched=false;
        std::uint64_t mem_budget = 5632ULL << 20; // 5.5 GB padrão para acomodar os pools completos do jogo (Dantelion2 + GX)
        for (std::size_t a=2;a<args.size();++a) {
            const std::string arg = args[a].string();
            if (arg=="--libc") linked=true;
            else if (arg=="--hle") hle=true;
            else if (arg=="--trace-hle") trace_hle=true;
            else if (arg=="--sched") use_sched=true;
            else if (arg=="--mem" && a+1<args.size()) {
                mem_budget = std::strtoull(args[++a].string().c_str(), nullptr, 10) << 20;
            }
            else if (!budget_set && !arg.empty() && arg.find_first_not_of("0123456789")==std::string::npos) {
                max_steps=std::strtoull(arg.c_str(),nullptr,10);budget_set=true;
            } else throw std::runtime_error("opcao desconhecida: "+arg);
        }
        bb::GuestMemory memory(static_cast<std::size_t>(mem_budget));
        const std::filesystem::path root = args[1];
        auto eboot=read_file(root/"eboot.bin");
        std::map<std::string,std::vector<std::uint8_t>> modules;
        std::uint64_t total=eboot.size();
        const auto directory=root/"sce_module";
        if (std::filesystem::is_directory(directory)) for (const auto& item:std::filesystem::directory_iterator(directory)) {
            if (!item.is_regular_file() || item.path().extension()!=".prx") continue;
            auto bytes=read_file(item.path());
            if (bytes.size()>(512ull<<20)-total) throw std::runtime_error("total de arquivos excessivo");
            total+=bytes.size();
            modules.emplace(item.path().filename().string(),std::move(bytes));
        }
        if (linked && !modules.count("libc.prx")) throw std::runtime_error("libc.prx ausente");
        auto image=linked ? bb::Ps4Loader::load(memory,eboot,modules.at("libc.prx"))
                          : bb::Ps4Loader::load(memory,eboot,modules);
        bb::X86Interpreter cpu(memory);
        cpu.add_trap_range(0x7f000000,32*(image.imports.size()+1));
        cpu.add_trap_range(0x7e000000,4096*(image.imports.size()+1));
        cpu.add_trap_range(bb::Ps4Bootstrap::exit_trap,64);
        auto tcb=bb::Ps4Bootstrap::prepare(memory,cpu.state,image);
        std::printf("entrada=0x%llx, imports=%zu, modulos=%zu, tcb=0x%llx\n",
                    static_cast<unsigned long long>(image.entry),image.imports.size(),image.modules.size(),
                    static_cast<unsigned long long>(tcb));
        bb::OrbisRuntime runtime(memory);
        bb::VideoOutManager video_out;
        runtime.attach_video_out(&video_out);
        runtime.set_procparam(image.procparam);
        runtime.set_tls_template(image.tls);
        runtime.set_app0_dir(args[1]);

        for (std::size_t i = 188; i <= 195 && i < image.imports.size(); ++i) {
            std::printf("  [IMPORT %zu] name='%s' nid='%s' resolved='%s' trap=0x%llx\n",
                        i, image.imports[i].name.c_str(), image.imports[i].nid.c_str(),
                        runtime.resolve(image.imports[i].name).c_str(),
                        (unsigned long long)image.imports[i].trap);
        }

        std::set<std::string> unique_resolved;
        for (const auto& imp : image.imports) {
            unique_resolved.insert(runtime.resolve(imp.name));
        }
        for (const auto& mod : image.modules) {
            std::printf("  [MODULE] '%s' base=0x%llx init=0x%llx fini=0x%llx\n",
                        mod.name.c_str(), (unsigned long long)mod.base,
                        (unsigned long long)mod.init, (unsigned long long)mod.fini);
        }
        std::printf("Resolved %zu unique imports from NIDs:\n", unique_resolved.size());
        for (const auto& name : unique_resolved) {
            std::printf("  [IMPORT] %s\n", name.c_str());
        }
        std::uint64_t got_val = 0;
        for (std::uint64_t g : {0x153e53b0ULL, 0x153e4858ULL, 0x153e4ed0ULL, 0x153e4988ULL, 0x153e4990ULL, 0x153e4da0ULL}) {
            if (memory.load(g, &got_val, 8) == bb::MemoryError::ok) {
                std::printf("GOT[0x%llx] = 0x%llx\n", (unsigned long long)g, (unsigned long long)got_val);
                for (std::size_t i = 0; i < image.imports.size(); ++i) {
                    if (image.imports[i].trap == got_val || image.imports[i].address == got_val) {
                        std::printf("  -> matches import[%zu]: %s (resolved: %s)\n",
                                    i, image.imports[i].name.c_str(), runtime.resolve(image.imports[i].name).c_str());
                    }
                }
            }
        }
        if (memory.load(0x153e4da0, &got_val, 8) == bb::MemoryError::ok) {
            std::printf("GOT[0x153e4da0] = 0x%llx\n", (unsigned long long)got_val);
            for (std::size_t i = 0; i < image.imports.size(); ++i) {
                if (image.imports[i].trap == got_val || image.imports[i].address == got_val) {
                    std::printf("  -> matches import[%zu]: %s (resolved: %s)\n",
                                i, image.imports[i].name.c_str(), runtime.resolve(image.imports[i].name).c_str());
                }
            }
        }
        if (memory.load(0x153e5370, &got_val, 8) == bb::MemoryError::ok) {
            std::printf("GOT[0x153e5370] = 0x%llx\n", (unsigned long long)got_val);
            for (std::size_t i = 0; i < image.imports.size(); ++i) {
                if (image.imports[i].trap == got_val || image.imports[i].address == got_val) {
                    std::printf("  -> matches import[%zu]: %s (resolved: %s)\n",
                                i, image.imports[i].name.c_str(), runtime.resolve(image.imports[i].name).c_str());
                }
            }
        }
        if (memory.load(0x153e53b8, &got_val, 8) == bb::MemoryError::ok) {
            std::printf("GOT[0x153e53b8] = 0x%llx\n", (unsigned long long)got_val);
            for (std::size_t i = 0; i < image.imports.size(); ++i) {
                if (image.imports[i].trap == got_val || image.imports[i].address == got_val) {
                    std::printf("  -> matches import[%zu]: %s (resolved: %s)\n",
                                i, image.imports[i].name.c_str(), runtime.resolve(image.imports[i].name).c_str());
                }
            }
        }
        if (memory.load(0x153e5ae8, &got_val, 8) == bb::MemoryError::ok) {
            std::printf("GOT[0x153e5ae8] = 0x%llx\n", (unsigned long long)got_val);
            for (std::size_t i = 0; i < image.imports.size(); ++i) {
                if (image.imports[i].trap == got_val || image.imports[i].address == got_val) {
                    std::printf("  -> matches import[%zu]: %s (resolved: %s)\n",
                                i, image.imports[i].name.c_str(), runtime.resolve(image.imports[i].name).c_str());
                }
            }
        }
        if (memory.load(0x153e5af0, &got_val, 8) == bb::MemoryError::ok) {
            std::printf("GOT[0x153e5af0] = 0x%llx\n", (unsigned long long)got_val);
            for (std::size_t i = 0; i < image.imports.size(); ++i) {
                if (image.imports[i].trap == got_val || image.imports[i].address == got_val) {
                    std::printf("  -> matches import[%zu]: %s (resolved: %s)\n",
                                i, image.imports[i].name.c_str(), runtime.resolve(image.imports[i].name).c_str());
                }
            }
        }
        std::uint64_t vtbl_test = 0;
        if (memory.load(0x1535fbc0, &vtbl_test, 8) == bb::MemoryError::ok) {
            std::printf("VTABLE 0x1535fbc0[0] = 0x%llx\n", (unsigned long long)vtbl_test);
        }
        if (memory.load(0x153e4778, &got_val, 8) == bb::MemoryError::ok) {
            std::printf("GOT[0x153e4778] = 0x%llx\n", (unsigned long long)got_val);
            for (std::size_t i = 0; i < image.imports.size(); ++i) {
                if (image.imports[i].trap == got_val || image.imports[i].address == got_val) {
                    std::printf("  -> matches import[%zu]: %s (resolved: %s)\n",
                                i, image.imports[i].name.c_str(), runtime.resolve(image.imports[i].name).c_str());
                }
            }
        }
        std::uint64_t fs_off = 0;
        if (memory.load(0x153e4328, &fs_off, 8) == bb::MemoryError::ok) {
            std::printf("Value in memory at 0x153e4328 = 0x%llx (%lld)\n", (unsigned long long)fs_off, (long long)fs_off);
        }
        std::uint64_t steps=0;
        bb::CpuRunResult result;
        auto execute=[&]() {
            for (;;) {
                std::uint64_t chunk = 100000;
                if (steps >= 451750000) chunk = 1;
                else if (max_steps - steps < chunk) chunk = max_steps - steps;
                result = cpu.run(chunk);
                steps += result.steps;
                if (steps >= 451750000) {
                    if (cpu.state.rip == 0x121949df) {
                        std::uint64_t vtable = 0, func = 0;
                        memory.load(cpu.state.gpr[13] + 0x68, &vtable, 8);
                        memory.load(vtable + 0x18, &func, 8);
                        std::printf("[TRACE_ALLOC] calling *0x18(rax): vtable=0x%llx func=0x%llx rdi=0x%llx rsi=0x%llx\n",
                                    (unsigned long long)vtable, (unsigned long long)func,
                                    (unsigned long long)cpu.state.gpr[7], (unsigned long long)cpu.state.gpr[6]);
                    }
                    if (cpu.state.rip == 0x121949aa || cpu.state.rip == 0x121949ad ||
                        cpu.state.rip == 0x121949ba || cpu.state.rip == 0x121949e2 ||
                        cpu.state.rip == 0x121949ec || cpu.state.rip == 0x121949f4 ||
                        cpu.state.rip == 0x12194b5f || cpu.state.rip == 0x12194b7b) {
                        std::printf("[TRACE_ALLOC] rip=0x%llx rax=0x%llx r13=0x%llx r12=0x%llx rsi=0x%llx steps=%llu\n",
                                    (unsigned long long)cpu.state.rip,
                                    (unsigned long long)cpu.state.gpr[0],
                                    (unsigned long long)cpu.state.gpr[13],
                                    (unsigned long long)cpu.state.gpr[12],
                                    (unsigned long long)cpu.state.gpr[6],
                                    (unsigned long long)steps);
                    }
                }
                if (result.stop == bb::CpuStop::step_limit) {
                    if (steps >= max_steps) break;
                    continue;
                }
                if (!hle || result.stop!=bb::CpuStop::trap || result.rip<0x7f000000 ||
                    result.rip>=0x7f000000+32*image.imports.size() || steps>=max_steps) {
                    break;
                }
                auto index=(result.rip-0x7f000000)/32;
                const auto& imp=image.imports[static_cast<std::size_t>(index)];
                if (imp.trap!=result.rip) {
                    std::printf("trap rip 0x%llx != imp.trap 0x%llx\n", (unsigned long long)result.rip, (unsigned long long)imp.trap);
                    break;
                }
                std::uint64_t value=0;
                try {
                    value=runtime.invoke(imp.name,cpu.state);
                    if (trace_hle) {
                        std::printf("HLE: %s (%s) -> 0x%llx\n", imp.name.c_str(), runtime.resolve(imp.name).c_str(), (unsigned long long)value);
                    }
                }
                catch (const std::exception& e) {
                    std::printf("HLE: %s (import: %s - %s)\n", e.what(), imp.name.c_str(), runtime.resolve(imp.name).c_str());
                    break;
                }
                std::uint64_t ret=0;
                if (memory.load(cpu.state.gpr[4],&ret,8)!=bb::MemoryError::ok) {
                    std::printf("falha ao desempilhar retorno do trap\n");
                    break;
                }
                cpu.state.gpr[0]=value;
                cpu.state.gpr[4]+=8;
                cpu.state.rip=ret;
            }
        };
        for (std::size_t i=0;i<image.modules.size();++i) {
            if (!image.modules[i].init) continue;
            bb::Ps4Bootstrap::module_call(memory,cpu.state,image.modules[i]);
            execute();
            if (result.stop!=bb::CpuStop::trap || result.rip!=bb::Ps4Bootstrap::exit_trap+32 || cpu.state.gpr[0]!=0) {
                std::printf("modulo %zu (%s) nao inicializado: %s\n",i,image.modules[i].name.c_str(),result.describe().c_str());
                return 1;
            }
            std::printf("modulo %zu (%s) inicializado apos %llu passos\n",i,image.modules[i].name.c_str(),static_cast<unsigned long long>(steps));
        }
        bb::Ps4Bootstrap::game_entry(memory,cpu.state,image);
        uint8_t probe_bytes[16] = {};
        if (memory.load(0x12167870, probe_bytes, 16) == bb::MemoryError::ok) {
            std::printf("PROBE AT 0x12167870 right after load: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                        probe_bytes[0], probe_bytes[1], probe_bytes[2], probe_bytes[3], probe_bytes[4],
                        probe_bytes[5], probe_bytes[6], probe_bytes[7], probe_bytes[8], probe_bytes[9]);
        }
        if (use_sched) {
            bb::GuestScheduler sched(memory, 25000);
            sched.add_trap_range(0x7f000000, 32 * (image.imports.size() + 1));
            sched.add_trap_range(0x7e000000, 4096 * (image.imports.size() + 1));
            sched.add_trap_range(bb::Ps4Bootstrap::exit_trap, 64);
            runtime.attach_scheduler(sched);
            const std::uint64_t return_trap = bb::Ps4Bootstrap::exit_trap + 32;

            sched.set_trap([&](bb::GuestScheduler& s, bb::GuestScheduler::Id tid, bb::CpuState& st) -> bool {
                if (st.rip == return_trap) {
                    if (runtime.has_pending_callback(tid)) {
                        runtime.complete_callback(st, tid);
                        return true;
                    }
                    std::printf("[SCHED] Thread %u exited with code 0x%llx\n", tid, (unsigned long long)st.gpr[0]);
                    s.exit(tid, st.gpr[0]);
                    return true;
                }
                if (st.rip >= 0x7f000000 && st.rip < 0x7f000000 + 32 * image.imports.size()) {
                    auto index = (st.rip - 0x7f000000) / 32;
                    const auto& imp = image.imports[static_cast<std::size_t>(index)];
                    if (trace_hle) {
                        std::printf("[TID %u] HLE: %s (%s)\n", tid, imp.name.c_str(), runtime.resolve(imp.name).c_str());
                    }
                    auto inv = runtime.invoke_trap(imp.name, st, tid, return_trap);
                    if (inv.action == bb::OrbisRuntime::InvocationResult::Action::returned) {
                        std::uint64_t ret = 0;
                        if (memory.load(st.gpr[4], &ret, 8) != bb::MemoryError::ok) {
                            std::printf("[TID %u] Falha ao desempilhar retorno do trap\n", tid);
                            return false;
                        }
                        st.gpr[4] += 8;
                        st.rip = ret;
                        st.gpr[0] = inv.value;
                    }
                    return true;
                }
                std::printf("[TID %u] Unhandled trap at 0x%llx\n", tid, (unsigned long long)st.rip);
                return false;
            });

            auto main_tid = sched.adopt(cpu.state, bb::Ps4Bootstrap::stack_base,
                                        bb::Ps4Bootstrap::stack_base + bb::Ps4Bootstrap::stack_size);
            std::printf("[SCHED] Adotada thread principal (TID %u), stack=[0x%llx..0x%llx], entry=0x%llx\n",
                        main_tid, (unsigned long long)bb::Ps4Bootstrap::stack_base,
                        (unsigned long long)(bb::Ps4Bootstrap::stack_base + bb::Ps4Bootstrap::stack_size),
                        (unsigned long long)cpu.state.rip);

            bb::GuestScheduler::Result sched_res{};
            std::uint64_t chunk = 500000;
            while (steps < max_steps) {
                if (max_steps - steps < chunk) chunk = max_steps - steps;
                sched_res = sched.run(chunk);
                steps += sched_res.steps;
                if (sched_res.stop != bb::GuestScheduler::Stop::budget) {
                    break;
                }
                if (steps % 100000000 == 0) {
                    std::printf("[SCHED PROGRESS] steps=%llu threads=%zu active_tid=%u rip=0x%llx\n",
                                (unsigned long long)steps, sched.thread_count(), sched_res.thread,
                                (unsigned long long)sched.cpu(sched_res.thread).rip);
                }
            }

            std::printf("SCHED parou: stop=%d thread=%u steps=%llu\n",
                        (int)sched_res.stop, sched_res.thread, (unsigned long long)steps);
            auto traces = sched.recent_traces();
            std::printf("=== RECENT TRACES (last %zu) ===\n", traces.size());
            for (std::size_t i = 0; i < traces.size(); ++i) {
                if (i == 0 || traces[i].rbp != traces[i-1].rbp || i >= traces.size() - 20) {
                    std::printf("  [%04zu] rip=0x%llx (image+0x%llx) rbp=0x%llx rsp=0x%llx\n",
                                i, (unsigned long long)traces[i].rip,
                                (traces[i].rip >= 0x10000000 && traces[i].rip < 0x20000000) ? (unsigned long long)(traces[i].rip - 0x10000000) : 0ULL,
                                (unsigned long long)traces[i].rbp, (unsigned long long)traces[i].rsp);
                }
            }
            std::printf("=============================\n");
            std::printf("=== SCHED THREAD DUMP (%zu threads) ===\n", sched.thread_count());
            for (bb::GuestScheduler::Id tid = 1; tid <= 3000; ++tid) {
                if (sched.has_thread(tid)) {
                    const auto& tcpu = sched.cpu(tid);
                    auto st = sched.state(tid);
                    const char* st_name = (st == bb::GuestScheduler::State::ready) ? "READY" :
                                         (st == bb::GuestScheduler::State::waiting) ? "WAIT " :
                                         (st == bb::GuestScheduler::State::exited) ? "EXIT " :
                                         (st == bb::GuestScheduler::State::created) ? "CREAT" : "FAULT";
                    std::uint64_t ret0 = 0, ret1 = 0;
                    memory.load(tcpu.gpr[4], &ret0, 8);
                    memory.load(tcpu.gpr[4] + 8, &ret1, 8);
                    std::printf("  TID %4u [%s]: rip=0x%llx rsp=0x%llx [rsp]=0x%llx [rsp+8]=0x%llx\n",
                                tid, st_name, (unsigned long long)tcpu.rip, (unsigned long long)tcpu.gpr[4],
                                (unsigned long long)ret0, (unsigned long long)ret1);
                }
            }
            std::printf("=======================================\n");
            if (sched.has_thread(1)) {
                const auto& mcpu = sched.cpu(1);
                std::printf("TID 1 STACK WALK (rsp=0x%llx, rbp=0x%llx):\n",
                            (unsigned long long)mcpu.gpr[4], (unsigned long long)mcpu.gpr[5]);
                for (int s = 0; s < 48; ++s) {
                    std::uint64_t val = 0;
                    memory.load(mcpu.gpr[4] + s * 8, &val, 8);
                    std::printf("  [rsp+%03x] = 0x%016llx (image+0x%llx)\n",
                                s * 8, (unsigned long long)val,
                                (val >= 0x10000000 && val < 0x20000000) ? (unsigned long long)(val - 0x10000000) : 0ULL);
                }
            }
            std::printf("=== VIDEO OUT & GNM STATS ===\n");
            std::printf("  Flips: %llu | Has new frame: %s\n",
                        (unsigned long long)video_out.flip_count(),
                        video_out.has_new_frame() ? "YES" : "NO");
            if (runtime.gnm_context()) {
                std::printf("  Draw calls: %zu | Last draw: %s\n",
                            runtime.gnm_context()->draw_calls().size(),
                            runtime.gnm_context()->last_draw_call() ? "recorded" : "none");
            }
            std::printf("=============================\n");
            if (sched_res.thread) {
                cpu.state = sched.cpu(sched_res.thread);
                result = sched_res.cpu;
            }
        } else {
            execute();
        }
        std::printf("parou: %s (total_steps=%llu)\n",result.describe().c_str(),static_cast<unsigned long long>(steps));
        std::printf("rax=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx\n",
                    (unsigned long long)cpu.state.gpr[0], (unsigned long long)cpu.state.gpr[3],
                    (unsigned long long)cpu.state.gpr[1], (unsigned long long)cpu.state.gpr[2]);
        std::printf("rsi=0x%llx rdi=0x%llx rbp=0x%llx rsp=0x%llx\n",
                    (unsigned long long)cpu.state.gpr[6], (unsigned long long)cpu.state.gpr[7],
                    (unsigned long long)cpu.state.gpr[5], (unsigned long long)cpu.state.gpr[4]);
        std::printf("r8=0x%llx r9=0x%llx r10=0x%llx r11=0x%llx\n",
                    (unsigned long long)cpu.state.gpr[8], (unsigned long long)cpu.state.gpr[9],
                    (unsigned long long)cpu.state.gpr[10], (unsigned long long)cpu.state.gpr[11]);
        std::printf("r12=0x%llx r13=0x%llx r14=0x%llx r15=0x%llx\n",
                    (unsigned long long)cpu.state.gpr[12], (unsigned long long)cpu.state.gpr[13],
                    (unsigned long long)cpu.state.gpr[14], (unsigned long long)cpu.state.gpr[15]);
        std::printf("rflags=0x%llx\n", (unsigned long long)cpu.state.rflags);
        std::printf("rax=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx\n",
                    (unsigned long long)cpu.state.gpr[0], (unsigned long long)cpu.state.gpr[3],
                    (unsigned long long)cpu.state.gpr[1], (unsigned long long)cpu.state.gpr[2]);
        std::printf("rsi=0x%llx rdi=0x%llx rbp=0x%llx rsp=0x%llx\n",
                    (unsigned long long)cpu.state.gpr[6], (unsigned long long)cpu.state.gpr[7],
                    (unsigned long long)cpu.state.gpr[5], (unsigned long long)cpu.state.gpr[4]);
        std::printf("r8=0x%llx r9=0x%llx r10=0x%llx r11=0x%llx\n",
                    (unsigned long long)cpu.state.gpr[8], (unsigned long long)cpu.state.gpr[9],
                    (unsigned long long)cpu.state.gpr[10], (unsigned long long)cpu.state.gpr[11]);
        std::printf("r12=0x%llx r13=0x%llx r14=0x%llx r15=0x%llx\n",
                    (unsigned long long)cpu.state.gpr[12], (unsigned long long)cpu.state.gpr[13],
                    (unsigned long long)cpu.state.gpr[14], (unsigned long long)cpu.state.gpr[15]);
        std::printf("rflags=0x%llx\n", (unsigned long long)cpu.state.rflags);
        for (const auto& imp:image.imports) if (result.rip==imp.trap)
            std::printf("import chamado: %s (%s)\n",imp.name.c_str(), runtime.resolve(imp.name).c_str());
        if (result.rip>=0x10000000 && result.rip<0x70000000)
            std::printf("rip(imagem+)=0x%llx\n",static_cast<unsigned long long>(result.rip-0x10000000));
        // Print stack around rsp
        std::printf("stack at rsp=0x%llx:\n", (unsigned long long)cpu.state.gpr[4]);
        for (int m = 0; m < 16; ++m) {
            std::uint64_t val = 0;
            if (memory.load(cpu.state.gpr[4] + m * 8, &val, 8) == bb::MemoryError::ok) {
                std::printf("  [rsp+%02x] = 0x%016llx (image+0x%llx)\n",
                            m * 8, (unsigned long long)val,
                            (val >= 0x10000000 && val < 0x70000000) ? (unsigned long long)(val - 0x10000000) : 0ULL);
            }
        }
        std::uint64_t gui_mgr = 0;
        if (memory.load(0x155400e0, &gui_mgr, 8) == bb::MemoryError::ok) {
            std::printf("gui_mgr at 0x155400e0 = 0x%llx\n", (unsigned long long)gui_mgr);
            if (gui_mgr) {
                for (int m = 0; m < 16; ++m) {
                    std::uint64_t val = 0;
                    memory.load(gui_mgr + m * 8, &val, 8);
                    std::printf("  [gui_mgr+%02x] = 0x%016llx\n", m * 8, (unsigned long long)val);
                }
            }
        }
        std::printf("=== GRAPHICS STEPS TABLE (0x1558a990) ===\n");
        for (int i = 0; i < 15; ++i) {
            std::uint64_t fn = 0, name_ptr = 0, flag = 0;
            memory.load(0x1558a990 + i * 24, &fn, 8);
            memory.load(0x1558a990 + i * 24 + 8, &name_ptr, 8);
            memory.load(0x1558a990 + i * 24 + 16, &flag, 8);
            if (!fn && !name_ptr) break;
            std::string name;
            if (name_ptr) {
                for (int c = 0; c < 120; c += 2) {
                    char16_t ch = 0;
                    if (memory.load(name_ptr + c, &ch, 2) == bb::MemoryError::ok && ch) name += static_cast<char>(ch);
                    else break;
                }
            }
            std::printf("  Step %02d: fn=0x%llx flag=%llu name='%s'\n", i, (unsigned long long)fn, (unsigned long long)flag, name.c_str());
        }
        std::printf("==========================================\n");
        std::uint64_t reg_base = 0;
        std::size_t reg_size = 0;
        unsigned reg_perm = 0;
        if (memory.query_region(cpu.state.gpr[7], reg_base, reg_size, reg_perm)) {
            std::printf("Region for rdi=0x%llx: base=0x%llx size=0x%zx (end=0x%llx) perm=%u\n",
                        (unsigned long long)cpu.state.gpr[7], (unsigned long long)reg_base, reg_size,
                        (unsigned long long)(reg_base + reg_size), reg_perm);
        } else {
            std::printf("rdi=0x%llx NOT in any region!\n", (unsigned long long)cpu.state.gpr[7]);
        }
        if (memory.query_region(result.fault_address, reg_base, reg_size, reg_perm)) {
            std::printf("Region for fault=0x%llx: base=0x%llx size=0x%zx perm=%u\n",
                        (unsigned long long)result.fault_address, (unsigned long long)reg_base, reg_size, reg_perm);
        } else {
            std::printf("fault=0x%llx NOT in any region!\n", (unsigned long long)result.fault_address);
        }
        std::printf("memory at 0x10d7b0ed20:\n");
        for (int m = 0; m < 16; ++m) {
            std::uint64_t val = 0;
            if (memory.load(0x10d7b0ed20ULL + m * 8, &val, 8) == bb::MemoryError::ok) {
                std::printf("  [0x10d7b0ed20+%02x] = 0x%016llx\n", m * 8, (unsigned long long)val);
            }
        }
        // Print memory around rdi (bin table)
        std::printf("memory at rdi=0x%llx:\n", (unsigned long long)cpu.state.gpr[7]);
        for (int m = 0; m < 70; ++m) {
            std::uint64_t val = 0;
            if (memory.load(cpu.state.gpr[7] + m * 8, &val, 8) == bb::MemoryError::ok && val != 0) {
                std::printf("  [rdi+%03d (idx %d)] = 0x%016llx\n", m * 8, m, (unsigned long long)val);
            }
        }
        // Print memory around rax if valid
        std::printf("memory at rax=0x%llx:\n", (unsigned long long)cpu.state.gpr[0]);
        for (int m = -4; m < 8; ++m) {
            std::uint64_t val = 0;
            if (memory.load(cpu.state.gpr[0] + m * 8, &val, 8) == bb::MemoryError::ok) {
                std::printf("  [rax%+03d] = 0x%016llx\n", m * 8, (unsigned long long)val);
            }
        }
        // Print frame pointer backtrace
        std::printf("callstack frame walk:\n");
        std::uint64_t cur_rbp = cpu.state.gpr[5]; // rbp
        for (int frame = 0; frame < 16 && cur_rbp >= 0x10000 && cur_rbp < 0x80000000; ++frame) {
            std::uint64_t ret_addr = 0;
            std::uint64_t prev_rbp = 0;
            if (memory.load(cur_rbp + 8, &ret_addr, 8) == bb::MemoryError::ok &&
                memory.load(cur_rbp, &prev_rbp, 8) == bb::MemoryError::ok) {
                std::printf("  frame %d: rbp=0x%llx ret=0x%llx (image+0x%llx)\n",
                            frame, (unsigned long long)cur_rbp, (unsigned long long)ret_addr,
                            (unsigned long long)(ret_addr >= 0x10000000 ? ret_addr - 0x10000000 : 0));
                cur_rbp = prev_rbp;
            } else {
                break;
            }
        }
        std::uint64_t global_r12 = 0;
        if (memory.load(0x155406c8, &global_r12, 8) == bb::MemoryError::ok) {
            std::printf("global 0x155406c8 = 0x%llx\n", (unsigned long long)global_r12);
            for (int k = 0; k < 32; ++k) {
                std::uint64_t fld = 0;
                if (memory.load(global_r12 + k * 8, &fld, 8) == bb::MemoryError::ok) {
                    std::printf("  [struct+%03x] = 0x%016llx\n", k * 8, (unsigned long long)fld);
                }
            }
        }
        // Disassemble 64 bytes around RIP
        std::uint8_t code_peek[64] = {};
        std::uint64_t peek_addr = result.rip >= 32 ? result.rip - 32 : result.rip;
        if (memory.load(peek_addr, code_peek, 64) == bb::MemoryError::ok) {
            std::printf("code at [rip-32 .. rip+32] (rip at offset +32):\n");
            for (int k = 0; k < 64; ++k) {
                if (k == 32) std::printf(" [RIP->]");
                std::printf("%02x ", code_peek[k]);
                if (k == 31 || k == 63) std::printf("\n");
            }
        }
        return result.stop==bb::CpuStop::trap && result.rip==bb::Ps4Bootstrap::exit_trap ? 0 : 1;
    } catch (const std::exception& e) { std::fprintf(stderr,"erro: %s\n",e.what()); return 2; }
}
