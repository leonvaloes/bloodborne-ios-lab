#pragma once
#include <iostream>
#include <stdexcept>
#include <string>
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #condition); } while (0)
template<class F> int run(F body) {
    try { body(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
