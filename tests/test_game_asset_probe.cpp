#include "check.h"
#include "game_asset_probe.h"
#include "vfs.h"
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

int main() { return run([] {
    const auto dir = std::filesystem::temp_directory_path() /
        ("probe_sfo_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    CHECK(bb::probe_sfo_in_documents(dir.string()).status == "not_found");

    std::vector<std::uint8_t> data(80, 0);
    data[1] = 'P'; data[2] = 'S'; data[3] = 'F';
    const std::uint32_t keys = 36, values = 48, count = 1, length = 10;
    const std::uint16_t fmt = 0x0204;
    std::memcpy(data.data() + 8, &keys, 4);
    std::memcpy(data.data() + 12, &values, 4);
    std::memcpy(data.data() + 16, &count, 4);
    std::memcpy(data.data() + 22, &fmt, 2);
    std::memcpy(data.data() + 24, &length, 4);
    std::memcpy(data.data() + 36, "TITLE_ID", 8);
    std::memcpy(data.data() + 48, "CUSA03171", 9);
    const auto file = dir / "param.sfo";
    auto write = [&](const std::vector<std::uint8_t>& bytes) {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        CHECK(out.good());
    };
    write(data);
    auto report = bb::probe_sfo_in_documents(dir.string());
    CHECK(report.status == "ok" && report.valid);
    CHECK(report.title_id == "CUSA03171");
    CHECK(report.size_bytes == data.size());
    CHECK(report.crc32 == bb::VirtualFileSystem::compute_crc32(data.data(), data.size()));

    data[1] = 'X'; write(data);
    CHECK(bb::probe_sfo_in_documents(dir.string()).status == "invalid_sfo");
    data.resize(1024 * 1024 + 1); write(data);
    CHECK(bb::probe_sfo_in_documents(dir.string()).status == "invalid_size");

    if (const char* real = std::getenv("BB_TEST_SFO_PATH")) {
        std::ifstream in(real, std::ios::binary);
        CHECK(in.good());
        const std::vector<std::uint8_t> real_bytes((std::istreambuf_iterator<char>(in)),
            std::istreambuf_iterator<char>());
        write(real_bytes);
        report = bb::probe_sfo_in_documents(dir.string());
        CHECK(report.status == "ok" && report.valid);
        CHECK(report.size_bytes == real_bytes.size());
        CHECK(!report.title_id.empty());
    }
}); }
