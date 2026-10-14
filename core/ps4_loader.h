#pragma once
#include "guest_memory.h"
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bb {

struct Ps4Tls {
    std::uint64_t address = 0, filesz = 0, memsz = 0, align = 0;
    std::uint32_t module_id = 0; // eboot=1, libc=2
};
struct Ps4Module {
    std::string name;
    std::uint64_t base = 0, size = 0, init = 0;
    Ps4Tls tls;
};
struct Ps4Import {
    std::string name;     // NID#biblioteca#modulo, grafia original do eboot
    std::string nid;
    std::uint64_t trap = 0; // destino fallback, inclusive quando vinculado à libc
    std::uint64_t address = 0; // export vinculado ou trap
    bool data = false;
};
struct Ps4Image {
    std::uint64_t entry = 0, procparam = 0;
    Ps4Tls tls;
    std::vector<Ps4Module> modules; // ordem de inicialização antes do eboot
    std::vector<Ps4Import> imports;
    std::map<std::string, std::uint64_t> import_traps; // nome -> endereço do trap
};

// Não lê arquivos nem executa código: recebe bytes do app e mapeia apenas memória lógica.
// Lança std::runtime_error em entradas inválidas. A memória pode ficar parcialmente mapeada
// se houver erro; use uma instância nova de GuestMemory em cada tentativa.
class Ps4Loader {
public:
    static Ps4Image load(GuestMemory& memory,
                         const std::vector<std::uint8_t>& eboot,
                         const std::vector<std::uint8_t>& libc = {},
                         std::uint64_t image_base = 0x10000000,
                         std::uint64_t code_traps = 0x7f000000,
                         std::uint64_t data_traps = 0x7e000000);
    // Only DT_NEEDED dependencies are loaded; keys are exact sce_module filenames.
    // Missing dependencies and cycles fail closed. Legacy overload retains optional libc.
    static Ps4Image load(GuestMemory& memory,
                         const std::vector<std::uint8_t>& eboot,
                         const std::map<std::string, std::vector<std::uint8_t>>& modules,
                         std::uint64_t image_base = 0x10000000,
                         std::uint64_t code_traps = 0x7f000000,
                         std::uint64_t data_traps = 0x7e000000);
    static std::string nid(const std::string& ascii_name);
};
} // namespace bb
