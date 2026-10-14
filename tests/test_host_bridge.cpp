#include "check.h"
#include "host_bridge.h"

#include <cstdint>
#include <string>

int main() { return run([] {
    bb::HostBridge bridge;
    CHECK(!bridge.is_initialized());
    CHECK(!bridge.is_running());
    CHECK(bridge.state() == bb::BootState::not_started);
    CHECK(bridge.update(1.0f / 60.0f) == 0);
    CHECK(bridge.update(0.0f) == 0);

    bb::HostFrameBuffer frame;
    CHECK(!bridge.render_frame(frame));
    CHECK(!bridge.render_frame());
    CHECK(!bridge.on_touch(1, 0.5f, 0.5f, bb::TouchPhase::Began));
    CHECK(!bridge.on_touch(std::uint64_t{2}, 0.5f, 0.5f, bb::TouchPhase::Began));
    CHECK(bridge.diagnostics().active_touches == 0);

    const auto initial = bridge.diagnostics();
    CHECK(initial.boot_state == bb::BootState::not_started);
    CHECK(initial.boot_state_name == "not_started");
    CHECK(initial.total_steps == 0);
    CHECK(initial.total_import_calls == 0);
    CHECK(initial.modules_count == 0);
    CHECK(initial.video_flip_count == 0);
    CHECK(initial.summary().find("HostBridge Diagnostics") != std::string::npos);

    // Sem Documents, init rejeita antes de tocar em arquivos ou executar guest.
    CHECK(!bridge.init(std::string{}));
    CHECK(!bridge.is_initialized());
    CHECK(!bridge.is_running());
    CHECK(bridge.state() == bb::BootState::not_started);
    CHECK(bridge.update(1.0f / 60.0f) == 0);
    CHECK(!bridge.on_touch(3, 0.2f, 0.8f, bb::TouchPhase::Began));
    CHECK(bridge.diagnostics().total_steps == 0);

    bb::HostBridgeConfig invalid;
    CHECK(!bridge.init(invalid));
    bridge.reset();
    bridge.shutdown(); // Repetir cleanup sem init deve ser seguro.
    CHECK(!bridge.is_initialized());
    CHECK(bridge.state() == bb::BootState::not_started);
    CHECK(bridge.diagnostics().active_touches == 0);
    CHECK(bridge.diagnostics().total_steps == 0);
}); }
