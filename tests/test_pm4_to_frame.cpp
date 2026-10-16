#include "check.h"
#include "gnm_rasterizer.h"
#include "orbis_runtime.h"
#include "video_out.h"

#include <array>
#include <cstdint>
#include <cstring>

using namespace bb;

int main() { return run([] {
    GuestMemory memory(128 * 1024);
    CHECK(memory.map(0x10000, 64 * 64 * 4, Permission::read | Permission::write) == MemoryError::ok);
    CHECK(memory.map(0x20000, 16 * 1024, Permission::read | Permission::write) == MemoryError::ok);
    CHECK(memory.map(0x50000, 4096, Permission::read | Permission::write) == MemoryError::ok);

    struct Vertex { float x, y, z, r, g, b, a; };
    const Vertex vertices[] = {
        {-0.6f, -0.6f, 0, 1, 0, 0, 1},
        { 0.6f, -0.6f, 0, 1, 0, 0, 1},
        { 0.0f,  0.6f, 0, 1, 0, 0, 1}
    };
    CHECK(memory.store(0x50000, vertices, sizeof(vertices)) == MemoryError::ok);
    RenderTarget target{};
    target.base_address = 0x10000;
    target.width = target.height = target.pitch = 64;
    target.format = GnmSurfaceFormat::B8G8R8A8_Unorm;
    GnmRasterizer rasterizer;
    CHECK(rasterizer.clear_render_target(memory, target, 0.2f, 0.4f, 0.6f, 1) == RasterizerStatus::Ok);

    BufferDescriptor descriptor{};
    descriptor.base_address = 0x50000;
    descriptor.stride = sizeof(Vertex);
    descriptor.num_records = 3;
    const auto raw = GnmContext::encode_buffer_descriptor(descriptor);
    std::array<std::uint32_t, 4> words{};
    std::memcpy(words.data(), raw.data(), raw.size());
    const std::uint32_t pm4[] = {
        0xC0016900, gcn_reg::CB_COLOR0_BASE, 0x100,
        0xC0016900, gcn_reg::CB_COLOR0_PITCH, 64,
        0xC0016900, gcn_reg::CB_COLOR0_INFO, 0x5C,
        0xC0016900, gcn_reg::CB_COLOR0_DIM, 0x000FC03F,
        0xC0047600, gcn_reg::SPI_SHADER_USER_DATA_VS_0,
        words[0], words[1], words[2], words[3],
        0xC0012D00, 3, 0
    };
    constexpr std::uint64_t command = 0x20000, address_table = 0x21000, size_table = 0x21040;
    CHECK(memory.store(command, pm4, sizeof(pm4)) == MemoryError::ok);
    CHECK(memory.store(address_table, &command, sizeof(command)) == MemoryError::ok);
    const std::uint32_t byte_count = sizeof(pm4);
    CHECK(memory.store(size_table, &byte_count, sizeof(byte_count)) == MemoryError::ok);

    struct Sink : VideoOutSink { void on_flip(const FlipEvent&) override {} } sink;
    VideoOutManager video;
    video.attach_guest_memory(&memory, &sink);
    const VideoBuffer output{0x10000, 64, 64, 64, 4, 0x80000000, 1};
    CHECK(video.register_buffer(0, output) == VideoStatus::ok);
    OrbisRuntime runtime(memory);
    runtime.attach_video_out(&video);
    CpuState cpu{};
    cpu.gpr[4] = 0x22000;
    cpu.gpr[7] = 1;
    cpu.gpr[6] = address_table;
    cpu.gpr[2] = size_table;
    CHECK(runtime.invoke("sceGnmSubmitCommandBuffers", cpu) == 0);
    CHECK(runtime.submit_count() == 1);
    CHECK(runtime.last_rasterizer_status() == RasterizerStatus::Ok);
    CHECK(runtime.rasterizer_failure_count() == 0);
    CHECK(runtime.gnm_context()->draw_calls().size() == 1);
    CHECK(runtime.rasterizer_stats().triangles_drawn == 1);
    CHECK(runtime.rasterizer_stats().pixels_written > 100);
    cpu.gpr[7] = 1; // handle; esta implementação usa índice 0 no segundo argumento
    cpu.gpr[6] = 0;
    cpu.gpr[2] = 0;
    cpu.gpr[1] = 42;
    CHECK(runtime.invoke("sceVideoOutSubmitFlip", cpu) == 0);
    CHECK(video.flip_count() == 1);
    HostFrameBuffer frame;
    CHECK(video.read_current_frame_pixels(frame));
    CHECK(frame.valid && frame.pixel_data.size() == 64 * 64 * 4);
    const auto center = (32 * 64 + 32) * 4;
    CHECK(frame.pixel_data[center] == 0 && frame.pixel_data[center + 1] == 0 &&
          frame.pixel_data[center + 2] == 255 && frame.pixel_data[center + 3] == 255);
    const auto corner = (2 * 64 + 2) * 4;
    CHECK(frame.pixel_data[corner] == 153 && frame.pixel_data[corner + 1] == 102 &&
          frame.pixel_data[corner + 2] == 51 && frame.pixel_data[corner + 3] == 255);
}); }
