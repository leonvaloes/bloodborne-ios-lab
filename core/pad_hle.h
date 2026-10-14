#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include "guest_memory.h"
#include "input.h"
#include "user_service.h"

namespace bb {

// Constantes PS4 de libScePad
constexpr std::int32_t SCE_PAD_PORT_TYPE_STANDARD = 0;
constexpr std::size_t SCE_PAD_MAX_PORTS = 4;

// Códigos de erro PS4 libScePad
constexpr std::int32_t SCE_PAD_ERROR_INVALID_ARG       = static_cast<std::int32_t>(0x80FF0001);
constexpr std::int32_t SCE_PAD_ERROR_INVALID_HANDLE    = static_cast<std::int32_t>(0x80FF0002);
constexpr std::int32_t SCE_PAD_ERROR_ALREADY_OPENED    = static_cast<std::int32_t>(0x80FF0003);
constexpr std::int32_t SCE_PAD_ERROR_NO_HANDLE         = static_cast<std::int32_t>(0x80FF0004);
constexpr std::int32_t SCE_PAD_ERROR_NOT_INITIALIZED   = static_cast<std::int32_t>(0x80FF0005);
constexpr std::int32_t SCE_PAD_ERROR_NOT_CONNECTED     = static_cast<std::int32_t>(0x80FF0006);

#pragma pack(push, 1)
// Estrutura ScePadData formatada para a memória do guest
struct ScePadData {
    std::uint32_t buttons{0};
    std::uint8_t left_stick_x{128};
    std::uint8_t left_stick_y{128};
    std::uint8_t right_stick_x{128};
    std::uint8_t right_stick_y{128};
    std::uint8_t l2_analog{0};
    std::uint8_t r2_analog{0};
    std::uint8_t padding[2]{0, 0};
    std::uint32_t connected{0};
    std::uint64_t timestamp{0};
};
static_assert(sizeof(ScePadData) == 24, "ScePadData must be 24 bytes");

// Parâmetros de intensidade de vibração dos motores hápticos
struct ScePadVibrationParam {
    std::uint8_t large_motor{0}; // Motor grande (baixa frequência)
    std::uint8_t small_motor{0}; // Motor pequeno (alta frequência)
};
static_assert(sizeof(ScePadVibrationParam) == 2, "ScePadVibrationParam must be 2 bytes");

// Cor RGB do LightBar do controle DualShock 4
struct ScePadColor {
    std::uint8_t r{0};
    std::uint8_t g{0};
    std::uint8_t b{0};
    std::uint8_t reserved{0};
};
static_assert(sizeof(ScePadColor) == 4, "ScePadColor must be 4 bytes");

struct ScePadOpenParam {
    std::uint32_t reserved{0};
};
#pragma pack(pop)

// Instância interna de porta de controle HLE
struct PadPortInstance {
    std::int32_t handle{-1};
    std::uint32_t user_id{0};
    std::int32_t type{0};
    std::int32_t index{0};
    bool is_open{false};
    InputManager* input_source{nullptr};
    std::unique_ptr<InputManager> owned_input;
    ScePadVibrationParam vibration{};
    ScePadColor lightbar{0, 0, 255, 0}; // Cor inicial azul padrão DS4
    float deadzone{0.15f};
    std::uint64_t last_timestamp{0};
};

class PadHleManager {
public:
    explicit PadHleManager(InputManager* default_input = nullptr, UserServiceManager* user_service = nullptr);
    ~PadHleManager() = default;

    // Métodos da API HLE libScePad
    std::int32_t scePadOpen(std::uint32_t userId, std::int32_t type, std::int32_t index, const void* param = nullptr);
    std::int32_t scePadOpen(GuestMemory& mem, std::uint32_t userId, std::int32_t type, std::int32_t index, std::uint64_t param_addr);

    std::int32_t scePadClose(std::int32_t handle);

    std::int32_t scePadReadState(std::int32_t handle, ScePadData* data);
    std::int32_t scePadReadState(GuestMemory& mem, std::int32_t handle, std::uint64_t guest_addr);

    std::int32_t scePadSetVibration(std::int32_t handle, const ScePadVibrationParam* param);
    std::int32_t scePadSetVibration(GuestMemory& mem, std::int32_t handle, std::uint64_t guest_param_addr);

    std::int32_t scePadSetLightBar(std::int32_t handle, const ScePadColor* color);
    std::int32_t scePadSetLightBar(GuestMemory& mem, std::int32_t handle, std::uint64_t guest_color_addr);

    // Métodos de configuração e inspeção
    void set_user_service(UserServiceManager* user_service);
    void set_default_input(InputManager* input);
    void bind_port_input(std::int32_t handle, InputManager* input);
    InputManager* get_input_manager(std::int32_t handle) const;

    bool is_handle_valid(std::int32_t handle) const;
    std::size_t active_pad_count() const;
    ScePadVibrationParam get_vibration(std::int32_t handle) const;
    ScePadColor get_lightbar(std::int32_t handle) const;
    void set_deadzone(std::int32_t handle, float deadzone);
    void reset();

    // Helper estático de formatação e calibração
    static ScePadData format_pad_data(const PadData& pad, std::uint64_t timestamp = 0);

private:
    PadPortInstance* find_port_by_handle_locked(std::int32_t handle);
    const PadPortInstance* find_port_by_handle_locked(std::int32_t handle) const;
    std::uint64_t generate_timestamp_locked();

    mutable std::mutex mutex_;
    InputManager* default_input_{nullptr};
    UserServiceManager* user_service_{nullptr};
    std::unique_ptr<InputManager> fallback_input_;
    std::array<PadPortInstance, SCE_PAD_MAX_PORTS> ports_{};
    std::int32_t next_handle_{1};
    std::uint64_t timestamp_counter_{1000};
};

// Instância padrão global para chamadas HLE estáticas
PadHleManager& default_pad_hle();

// Funções C-style / wrappers globais HLE delegando para a default_pad_hle()
std::int32_t scePadOpen(std::uint32_t userId, std::int32_t type, std::int32_t index, const void* param = nullptr);
std::int32_t scePadClose(std::int32_t handle);
std::int32_t scePadReadState(std::int32_t handle, ScePadData* data);
std::int32_t scePadReadState(GuestMemory& mem, std::int32_t handle, std::uint64_t guest_addr);
std::int32_t scePadSetVibration(std::int32_t handle, const ScePadVibrationParam* param);
std::int32_t scePadSetVibration(GuestMemory& mem, std::int32_t handle, std::uint64_t guest_param_addr);
std::int32_t scePadSetLightBar(std::int32_t handle, const ScePadColor* color);
std::int32_t scePadSetLightBar(GuestMemory& mem, std::int32_t handle, std::uint64_t guest_color_addr);

} // namespace bb
