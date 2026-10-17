#include "check.h"
#include "input.h"
#include "guest_memory.h"
#include <cmath>

int main() { return run([] {
    using namespace bb;
    InputManager input;

    // Estado inicial
    auto state = input.poll();
    CHECK(state.connected);
    CHECK(state.buttons == 0);
    CHECK(std::abs(state.left_stick_x) < 0.001f);
    CHECK(std::abs(state.left_stick_y) < 0.001f);
    CHECK(std::abs(state.right_stick_x) < 0.001f);
    CHECK(std::abs(state.right_stick_y) < 0.001f);
    CHECK(std::abs(state.l2_analog) < 0.001f);
    CHECK(std::abs(state.r2_analog) < 0.001f);

    // Botões digitais
    input.set_button(PAD_CROSS, true);
    input.set_button(PAD_R1, true);
    state = input.poll();
    CHECK((state.buttons & PAD_CROSS) != 0);
    CHECK((state.buttons & PAD_R1) != 0);
    CHECK((state.buttons & PAD_CIRCLE) == 0);

    input.set_button(PAD_CROSS, false);
    state = input.poll();
    CHECK((state.buttons & PAD_CROSS) == 0);
    CHECK((state.buttons & PAD_R1) != 0);

    // Máscara direta
    input.set_buttons_mask(PAD_OPTIONS | PAD_UP | PAD_TRIANGLE);
    state = input.poll();
    CHECK(state.buttons == (PAD_OPTIONS | PAD_UP | PAD_TRIANGLE));

    // Deadzone analógica estática
    CHECK(InputManager::apply_deadzone(0.05f, 0.15f) == 0.0f);
    CHECK(InputManager::apply_deadzone(-0.10f, 0.15f) == 0.0f);
    CHECK(InputManager::apply_deadzone(1.0f, 0.15f) == 1.0f);
    CHECK(InputManager::apply_deadzone(-1.0f, 0.15f) == -1.0f);

    float rx = 0, ry = 0;
    InputManager::apply_radial_deadzone(0.05f, 0.05f, 0.15f, rx, ry);
    CHECK(rx == 0.0f && ry == 0.0f);

    InputManager::apply_radial_deadzone(0.6f, 0.8f, 0.2f, rx, ry);
    CHECK(std::abs(std::sqrt(rx * rx + ry * ry) - 1.0f) < 0.001f);

    // Sticks com deadzone configurada no manager
    input.reset();
    input.set_deadzone(0.2f);
    input.set_left_stick(0.1f, -0.1f); // dentro da deadzone
    state = input.poll();
    CHECK(std::abs(state.left_stick_x) < 0.001f);
    CHECK(std::abs(state.left_stick_y) < 0.001f);

    input.set_left_stick(0.0f, 1.0f); // fora da deadzone
    state = input.poll();
    CHECK(std::abs(state.left_stick_y - 1.0f) < 0.001f);

    // Gatilhos analógicos
    input.set_triggers(0.85f, 0.10f);
    state = input.poll();
    CHECK(std::abs(state.l2_analog - 0.85f) < 0.001f);
    CHECK(std::abs(state.r2_analog - 0.10f) < 0.001f);
    CHECK((state.buttons & PAD_L2) != 0); // Ativado pelo limiar > 0.2
    CHECK((state.buttons & PAD_R2) == 0);

    // Serialização HLE na memória do guest
    GuestMemory mem(64);
    CHECK(mem.map(0x1000, 64, read | write) == MemoryError::ok);
    CHECK(input.write_hle_pad_data(mem, 0x1000) == MemoryError::ok);

    std::uint32_t read_buttons = 0;
    CHECK(mem.load(0x1000, &read_buttons, sizeof(read_buttons)) == MemoryError::ok);
    CHECK((read_buttons & PAD_L2) != 0);

    std::uint8_t stick_y = 0;
    CHECK(mem.load(0x1005, &stick_y, sizeof(stick_y)) == MemoryError::ok);
    CHECK(stick_y == 255); // 1.0 mapeado para 255

    std::uint32_t connected = 0;
    CHECK(mem.load(0x100C, &connected, sizeof(connected)) == MemoryError::ok);
    CHECK(connected == 1);

    // ========================================================================
    // Arbitracao de Entradas Simultaneas Toque e Gamepad por Origem (T056)
    // ========================================================================
    input.reset();

    // 1. Pressao e liberacao sobreposta do mesmo botao (Cross)
    input.set_button(PAD_CROSS, true, InputSource::Touch);
    CHECK((input.poll().buttons & PAD_CROSS) != 0);

    input.set_button(PAD_CROSS, true, InputSource::Gamepad);
    CHECK((input.poll().buttons & PAD_CROSS) != 0);

    // Liberacao pelo Toque NAO deve apagar a pressao mantida pelo Gamepad
    input.set_button(PAD_CROSS, false, InputSource::Touch);
    CHECK((input.poll().buttons & PAD_CROSS) != 0);

    // Apenas quando ambas as fontes liberarem o botao e desativado
    input.set_button(PAD_CROSS, false, InputSource::Gamepad);
    CHECK((input.poll().buttons & PAD_CROSS) == 0);

    // Ordem inversa: Gamepad aperta primeiro, Toque sobrepoe e Gamepad solta primeiro
    input.set_button(PAD_SQUARE, true, InputSource::Gamepad);
    CHECK((input.poll().buttons & PAD_SQUARE) != 0);

    input.set_button(PAD_SQUARE, true, InputSource::Touch);
    CHECK((input.poll().buttons & PAD_SQUARE) != 0);

    input.set_button(PAD_SQUARE, false, InputSource::Gamepad);
    CHECK((input.poll().buttons & PAD_SQUARE) != 0); // Mantido pelo Toque!

    input.set_button(PAD_SQUARE, false, InputSource::Touch);
    CHECK((input.poll().buttons & PAD_SQUARE) == 0);

    // 2. Botoes distintos simultaneos (Ataque R1 via Toque + Esquiva Circle via Gamepad)
    input.set_button(PAD_R1, true, InputSource::Touch);
    input.set_button(PAD_CIRCLE, true, InputSource::Gamepad);
    state = input.poll();
    CHECK((state.buttons & PAD_R1) != 0);
    CHECK((state.buttons & PAD_CIRCLE) != 0);

    // Toque solta R1: Circle do Gamepad continua intacto
    input.set_button(PAD_R1, false, InputSource::Touch);
    state = input.poll();
    CHECK((state.buttons & PAD_R1) == 0);
    CHECK((state.buttons & PAD_CIRCLE) != 0);

    input.set_button(PAD_CIRCLE, false, InputSource::Gamepad);
    CHECK(input.poll().buttons == 0);

    // 3. Arbitracao de analogicos por magnitude dominante
    input.set_deadzone(0.0f);
    input.set_left_stick(0.3f, 0.4f, InputSource::Touch);     // mag = 0.5
    input.set_left_stick(0.0f, -0.9f, InputSource::Gamepad);   // mag = 0.9 (dominante)
    state = input.poll();
    CHECK(std::abs(state.left_stick_x) < 0.001f);
    CHECK(std::abs(state.left_stick_y - (-0.9f)) < 0.001f);

    // Liberacao do toque na tela nao zera o direcional do gamepad fisico
    input.set_left_stick(0.0f, 0.0f, InputSource::Touch);
    state = input.poll();
    CHECK(std::abs(state.left_stick_y - (-0.9f)) < 0.001f);

    input.set_left_stick(0.0f, 0.0f, InputSource::Gamepad);
    state = input.poll();
    CHECK(std::abs(state.left_stick_y) < 0.001f);

    // 4. Gatilhos analogicos com pressao sobreposta
    input.set_triggers(0.80f, 0.0f, InputSource::Touch);
    input.set_triggers(0.35f, 0.0f, InputSource::Gamepad);
    state = input.poll();
    CHECK(std::abs(state.l2_analog - 0.80f) < 0.001f);
    CHECK((state.buttons & PAD_L2) != 0);

    // Toque solta gatilho: pressao do Gamepad (0.35f) permanece ativa
    input.set_triggers(0.0f, 0.0f, InputSource::Touch);
    state = input.poll();
    CHECK(std::abs(state.l2_analog - 0.35f) < 0.001f);
    CHECK((state.buttons & PAD_L2) != 0);

    input.set_triggers(0.0f, 0.0f, InputSource::Gamepad);
    state = input.poll();
    CHECK(std::abs(state.l2_analog) < 0.001f);
    CHECK((state.buttons & PAD_L2) == 0);

    // 5. Nao reter botoes na desconexao ou troca de controle
    input.set_button(PAD_R1 | PAD_OPTIONS, true, InputSource::Gamepad);
    input.set_left_stick(0.7f, -0.7f, InputSource::Gamepad);
    CHECK((input.poll().buttons & PAD_R1) != 0);

    // Desconecta Gamepad: entradas do gamepad sao zeradas imediatamente
    input.set_connected(false, InputSource::Gamepad);
    state = input.poll();
    CHECK(state.buttons == 0);
    CHECK(std::abs(state.left_stick_x) < 0.001f);
    CHECK(std::abs(state.left_stick_y) < 0.001f);

    // ========================================================================
    // T149: Ignorar botoes/eixos de fontes desconectadas e estados de set_pad_state
    // ========================================================================
    input.reset();

    // 1. set_pad_state com connected=false e dados residuais: deve sanitizar na fonte
    PadData dirty_state{};
    dirty_state.connected = false;
    dirty_state.buttons = PAD_TRIANGLE | PAD_L1 | PAD_R2;
    dirty_state.left_stick_x = 0.75f;
    dirty_state.left_stick_y = -0.85f;
    dirty_state.right_stick_x = 0.50f;
    dirty_state.right_stick_y = -0.50f;
    dirty_state.l2_analog = 0.90f;
    dirty_state.r2_analog = 0.90f;
    input.set_pad_state(dirty_state, InputSource::Gamepad);

    auto poll_gp = input.poll_source(InputSource::Gamepad);
    CHECK(!poll_gp.connected);
    CHECK(poll_gp.buttons == 0);
    CHECK(std::abs(poll_gp.left_stick_x) < 0.001f);
    CHECK(std::abs(poll_gp.left_stick_y) < 0.001f);
    CHECK(std::abs(poll_gp.l2_analog) < 0.001f);
    CHECK(std::abs(poll_gp.r2_analog) < 0.001f);

    // 2. Desconexao com entradas retidas enquanto outra fonte (Toque) permanece ativa
    // Configura fonte Toque com entradas ativas
    input.set_connected(true, InputSource::Touch);
    input.set_button(PAD_CROSS | PAD_CIRCLE, true, InputSource::Touch);
    input.set_left_stick(0.6f, 0.0f, InputSource::Touch);
    input.set_triggers(0.7f, 0.0f, InputSource::Touch);

    // Configura gamepad com entradas concorrentes e depois desconecta
    input.set_connected(true, InputSource::Gamepad);
    input.set_button(PAD_SQUARE | PAD_R1, true, InputSource::Gamepad);
    input.set_left_stick(0.0f, -0.9f, InputSource::Gamepad);
    input.set_triggers(0.0f, 0.85f, InputSource::Gamepad);

    // Desconecta a fonte Gamepad com botoes retidos
    input.set_connected(false, InputSource::Gamepad);

    // poll() agregado deve refletir exclusivamente a fonte de toque ativa
    auto agg = input.poll();
    CHECK(agg.connected);
    CHECK((agg.buttons & PAD_CROSS) != 0);
    CHECK((agg.buttons & PAD_CIRCLE) != 0);
    CHECK((agg.buttons & PAD_SQUARE) == 0); // Entrada retida do gamepad DEVE ser ignorada
    CHECK((agg.buttons & PAD_R1) == 0);
    CHECK(std::abs(agg.left_stick_y) < 0.001f); // Eixo retido do gamepad ignorado
    CHECK(agg.left_stick_x > 0.3f);             // Eixo do toque mantido
    CHECK(agg.l2_analog > 0.6f);                // Gatilho do toque mantido
    CHECK(std::abs(agg.r2_analog) < 0.001f);    // Gatilho retido do gamepad ignorado

    // Se a fonte restante de toque tambem desconectar, poll() resulta completamente inativo
    input.set_connected(false, InputSource::Touch);
    auto all_dc = input.poll();
    CHECK(!all_dc.connected);
    CHECK(all_dc.buttons == 0);
    CHECK(std::abs(all_dc.left_stick_x) < 0.001f);
    CHECK(std::abs(all_dc.l2_analog) < 0.001f);
}); }
