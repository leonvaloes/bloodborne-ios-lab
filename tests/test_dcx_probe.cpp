#include "check.h"
#include "game_asset_probe.h"
#include "miniz.h"
#include "vfs.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

int main() { return run([] {
    const auto dir = std::filesystem::temp_directory_path() /
        ("probe_dcx_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    const auto file = dir / "m29_00_00_00_408010.flver.dcx";
    CHECK(bb::probe_dcx_in_documents(dir.string()).status == "not_found");
    auto write = [&](const std::vector<std::uint8_t>& bytes) {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        CHECK(out.good());
    };
    std::vector<std::uint8_t> raw(128, 0);
    std::memcpy(raw.data(), "FLVER\0", 6);
    mz_ulong compressed_size = mz_compressBound(static_cast<mz_ulong>(raw.size()));
    std::vector<std::uint8_t> compressed(compressed_size);
    CHECK(mz_compress(compressed.data(), &compressed_size, raw.data(), static_cast<mz_ulong>(raw.size())) == MZ_OK);
    compressed.resize(compressed_size);
    std::vector<std::uint8_t> dcx(76 + compressed.size(), 0);
    std::memcpy(dcx.data(), "DCX\0", 4);
    std::memcpy(dcx.data() + 24, "DCS\0", 4);
    dcx[31] = 128;
    dcx[35] = static_cast<std::uint8_t>(compressed.size());
    std::memcpy(dcx.data() + 40, "DFLT", 4);
    std::memcpy(dcx.data() + 68, "DCA\0", 4);
    std::memcpy(dcx.data() + 76, compressed.data(), compressed.size());
    write(dcx);
    auto report = bb::probe_dcx_in_documents(dir.string());
    CHECK(report.status == "ok" && report.valid);
    CHECK(report.decoded_bytes == raw.size());
    CHECK(report.decoded_crc32 == bb::VirtualFileSystem::compute_crc32(raw.data(), raw.size()));
    dcx[31] = 0;
    write(dcx);
    CHECK(bb::probe_dcx_in_documents(dir.string()).status == "invalid_decoded_size");
    dcx[0] = 'X'; write(dcx);
    CHECK(bb::probe_dcx_in_documents(dir.string()).status == "invalid_dcx");

    if (const char* real = std::getenv("BB_TEST_DCX_PATH")) {
        std::ifstream in(real, std::ios::binary);
        CHECK(in.good());
        const std::vector<std::uint8_t> real_bytes((std::istreambuf_iterator<char>(in)),
            std::istreambuf_iterator<char>());
        write(real_bytes);
        report = bb::probe_dcx_in_documents(dir.string());
        CHECK(report.status == "ok" && report.valid);
        CHECK(report.size_bytes == real_bytes.size());
    }
}); }
