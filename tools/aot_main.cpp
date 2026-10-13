#include "aot.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        if (argc != 4) throw std::runtime_error("Usage: bb-aot input.hex fixture_symbol output.cpp");
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
