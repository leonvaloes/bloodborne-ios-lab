#include "aot.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            std::cerr << "Usage:\n"
                      << "  bb-aot <input.hex> <symbol> <output.cpp>                (Legacy fixture translation)\n"
                      << "  bb-aot --batch <elf_path> <out_dir> [max] [batch_sz]    (AOT Batch Recompilation)\n"
                      << "  bb-aot --lift <input.hex> <symbol> <output.cpp>         (Lift single function)\n"
                      << "  bb-aot --scan <elf_path> [max_funcs]                    (Scan ELF executable)\n";
            return 1;
        }

        std::string arg1 = argv[1];

        // Mode 1: Batch translation of ELF
        if (arg1 == "--batch") {
            if (argc < 4) throw std::runtime_error("Usage: bb-aot --batch <elf_path> <output_dir> [max_funcs] [batch_size]");
            std::string elf_path = argv[2];
            std::string out_dir = argv[3];
            bb::AotBatchConfig cfg{};
            if (argc >= 5) cfg.max_functions = std::stoull(argv[4]);
            if (argc >= 6) cfg.functions_per_batch = std::stoull(argv[5]);

            auto res = bb::batch_translate_elf(elf_path, out_dir, cfg);
            if (!res.success) {
                throw std::runtime_error(res.error_message);
            }
            std::cout << "Batch AOT translated " << res.total_functions << " functions ("
                      << res.total_instructions << " instructions) into "
                      << res.batch_files_count << " files in " << out_dir << "\n";
            return 0;
        }

        // Mode 2: Scan ELF
        if (arg1 == "--scan") {
            if (argc < 3) throw std::runtime_error("Usage: bb-aot --scan <elf_path>");
            std::string elf_path = argv[2];
            std::ifstream file(elf_path, std::ios::binary | std::ios::ate);
            if (!file) throw std::runtime_error("Cannot open ELF: " + elf_path);
            auto sz = static_cast<std::size_t>(file.tellg());
            file.seekg(0, std::ios::beg);
            std::vector<std::uint8_t> data(sz);
            file.read(reinterpret_cast<char*>(data.data()), sz);

            std::size_t max_funcs = (argc >= 4) ? std::stoull(argv[3]) : 0;
            auto funcs = bb::lift_elf(data.data(), data.size(), max_funcs);
            std::cout << "ELF " << elf_path << ": discovered " << funcs.size() << " functions\n";
            return 0;
        }

        // Mode 3: Lift single function
        if (arg1 == "--lift") {
            if (argc != 5) throw std::runtime_error("Usage: bb-aot --lift <input.hex> <symbol> <output.cpp>");
            std::ifstream input(argv[2], std::ios::binary);
            if (!input) throw std::runtime_error("Cannot open hex input");
            std::string source;
            char byte;
            while (input.get(byte)) source += byte;
            auto bytes = bb::parse_hex(source);
            auto fn = bb::lift_function(bytes.data(), bytes.size(), 0x1000, argv[3]);
            std::ofstream out(argv[4]);
            if (!out) throw std::runtime_error("Cannot write output file");
            out << bb::emit_function_cpp(fn);
            return 0;
        }

        // Mode 4: Legacy fixture translation (argc == 4, no flags)
        if (argc != 4) {
            throw std::runtime_error("Usage: bb-aot input.hex fixture_symbol output.cpp");
        }

        std::ifstream input(argv[1], std::ios::binary);
        if (!input) throw std::runtime_error("Cannot open fixture");
        std::string source;
        char byte;
        while (input.get(byte)) {
            if (source.size() == 16384) throw std::runtime_error("Input too large");
            source += byte;
        }
        if (!input.eof()) throw std::runtime_error("Cannot read fixture");
        const auto generated = bb::translate(bb::parse_hex(source), argv[2]);
        std::ofstream output(argv[3], std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Cannot create output");
        output << generated;
        output.close();
        if (!output) throw std::runtime_error("Cannot write output");
        return 0;

    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
