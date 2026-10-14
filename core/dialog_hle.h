#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include "guest_memory.h"

namespace bb {

// Limite estrito de tamanho de strings do sistema de diálogos (prevenção de estouro de buffers)
constexpr std::size_t SCE_DIALOG_MAX_STRING_LENGTH = 256;

// Código de sucesso canônico Orbis
constexpr std::int32_t SCE_OK = 0;

// Códigos de erro PS4 libSceCommonDialog
constexpr std::int32_t SCE_COMMON_DIALOG_ERROR_NOT_INITIALIZED     = static_cast<std::int32_t>(0x80800001);
constexpr std::int32_t SCE_COMMON_DIALOG_ERROR_ALREADY_INITIALIZED = static_cast<std::int32_t>(0x80800002);
constexpr std::int32_t SCE_COMMON_DIALOG_ERROR_NOT_RUNNING         = static_cast<std::int32_t>(0x80800003);
constexpr std::int32_t SCE_COMMON_DIALOG_ERROR_ILLEGAL_STATE       = static_cast<std::int32_t>(0x80800004);
constexpr std::int32_t SCE_COMMON_DIALOG_ERROR_INVALID_ARG         = static_cast<std::int32_t>(0x80800005);
constexpr std::int32_t SCE_COMMON_DIALOG_ERROR_NULL_POINTER        = static_cast<std::int32_t>(0x80800006);
constexpr std::int32_t SCE_COMMON_DIALOG_ERROR_NOT_FINISHED       = static_cast<std::int32_t>(0x80800007);
constexpr std::int32_t SCE_COMMON_DIALOG_ERROR_BUSY               = static_cast<std::int32_t>(0x80800008);

// Códigos de erro PS4 libSceMsgDialog
constexpr std::int32_t SCE_MSG_DIALOG_ERROR_NOT_INITIALIZED        = static_cast<std::int32_t>(0x80801001);
constexpr std::int32_t SCE_MSG_DIALOG_ERROR_ALREADY_INITIALIZED    = static_cast<std::int32_t>(0x80801002);
constexpr std::int32_t SCE_MSG_DIALOG_ERROR_NOT_RUNNING            = static_cast<std::int32_t>(0x80801003);
constexpr std::int32_t SCE_MSG_DIALOG_ERROR_ILLEGAL_STATE          = static_cast<std::int32_t>(0x80801004);
constexpr std::int32_t SCE_MSG_DIALOG_ERROR_INVALID_ARG            = static_cast<std::int32_t>(0x80801005);
constexpr std::int32_t SCE_MSG_DIALOG_ERROR_BUSY                   = static_cast<std::int32_t>(0x80801006);
constexpr std::int32_t SCE_MSG_DIALOG_ERROR_NOT_FINISHED           = static_cast<std::int32_t>(0x80801007);

// Códigos de erro PS4 libSceImeDialog
constexpr std::int32_t SCE_IME_DIALOG_ERROR_NOT_INITIALIZED        = static_cast<std::int32_t>(0x80802001);
constexpr std::int32_t SCE_IME_DIALOG_ERROR_ALREADY_INITIALIZED    = static_cast<std::int32_t>(0x80802002);
constexpr std::int32_t SCE_IME_DIALOG_ERROR_NOT_RUNNING            = static_cast<std::int32_t>(0x80802003);
constexpr std::int32_t SCE_IME_DIALOG_ERROR_ILLEGAL_STATE          = static_cast<std::int32_t>(0x80802004);
constexpr std::int32_t SCE_IME_DIALOG_ERROR_INVALID_ARG            = static_cast<std::int32_t>(0x80802005);
constexpr std::int32_t SCE_IME_DIALOG_ERROR_BUSY                   = static_cast<std::int32_t>(0x80802006);
constexpr std::int32_t SCE_IME_DIALOG_ERROR_NOT_FINISHED           = static_cast<std::int32_t>(0x80802007);
constexpr std::int32_t SCE_IME_DIALOG_ERROR_TEXT_TOO_LONG          = static_cast<std::int32_t>(0x80802008);

// Estados canônicos SceCommonDialogStatus
enum class CommonDialogStatus : std::int32_t {
    NONE = 0,
    INITIALIZED = 1,
    RUNNING = 2,
    FINISHED = 3
};

constexpr std::int32_t SCE_COMMON_DIALOG_STATUS_NONE        = 0;
constexpr std::int32_t SCE_COMMON_DIALOG_STATUS_INITIALIZED = 1;
constexpr std::int32_t SCE_COMMON_DIALOG_STATUS_RUNNING     = 2;
constexpr std::int32_t SCE_COMMON_DIALOG_STATUS_FINISHED    = 3;

// Estados canônicos libSceMsgDialog
constexpr std::int32_t SCE_MSG_DIALOG_STATUS_NONE           = 0;
constexpr std::int32_t SCE_MSG_DIALOG_STATUS_INITIALIZED    = 1;
constexpr std::int32_t SCE_MSG_DIALOG_STATUS_RUNNING        = 2;
constexpr std::int32_t SCE_MSG_DIALOG_STATUS_FINISHED       = 3;

// Estados canônicos libSceImeDialog
constexpr std::int32_t SCE_IME_DIALOG_STATUS_NONE           = 0;
constexpr std::int32_t SCE_IME_DIALOG_STATUS_RUNNING        = 1;
constexpr std::int32_t SCE_IME_DIALOG_STATUS_FINISHED       = 2;
constexpr std::int32_t SCE_IME_DIALOG_STATUS_ABORTED        = 3;

// Status de término libSceImeDialog
constexpr std::int32_t SCE_IME_DIALOG_END_STATUS_OK            = 0;
constexpr std::int32_t SCE_IME_DIALOG_END_STATUS_USER_CANCELED = 1;
constexpr std::int32_t SCE_IME_DIALOG_END_STATUS_ABORTED       = 2;

// Tipos de botão para MsgDialog
enum class SceMsgDialogButtonType : std::int32_t {
    OK = 0,
    YESNO = 1,
    NONE = 2,
    OK_CANCEL = 3,
    WAIT = 4,
    WAIT_CANCEL = 5,
    YESNO_FOCUS_NO = 6,
    RETRY_CANCEL = 7
};

constexpr std::int32_t SCE_MSG_DIALOG_BUTTON_TYPE_OK             = 0;
constexpr std::int32_t SCE_MSG_DIALOG_BUTTON_TYPE_YESNO          = 1;
constexpr std::int32_t SCE_MSG_DIALOG_BUTTON_TYPE_NONE           = 2;
constexpr std::int32_t SCE_MSG_DIALOG_BUTTON_TYPE_OK_CANCEL      = 3;
constexpr std::int32_t SCE_MSG_DIALOG_BUTTON_TYPE_WAIT           = 4;
constexpr std::int32_t SCE_MSG_DIALOG_BUTTON_TYPE_WAIT_CANCEL    = 5;
constexpr std::int32_t SCE_MSG_DIALOG_BUTTON_TYPE_YESNO_FOCUS_NO = 6;
constexpr std::int32_t SCE_MSG_DIALOG_BUTTON_TYPE_RETRY_CANCEL   = 7;

// Identificadores de botão selecionado (resultado)
enum class SceMsgDialogButtonId : std::int32_t {
    INVALID = 0,
    OK = 1,
    YES = 1,
    NO = 2,
    CANCEL = 3,
    RETRY = 4
};

constexpr std::int32_t SCE_MSG_DIALOG_BUTTON_ID_INVALID = 0;
constexpr std::int32_t SCE_MSG_DIALOG_BUTTON_ID_OK      = 1;
constexpr std::int32_t SCE_MSG_DIALOG_BUTTON_ID_YES     = 1;
constexpr std::int32_t SCE_MSG_DIALOG_BUTTON_ID_NO      = 2;
constexpr std::int32_t SCE_MSG_DIALOG_BUTTON_ID_CANCEL  = 3;
constexpr std::int32_t SCE_MSG_DIALOG_BUTTON_ID_RETRY   = 4;

// Modos do diálogo de mensagem
enum class SceMsgDialogMode : std::int32_t {
    USER_MSG = 1,
    PROGRESS_BAR = 2,
    SYSTEM_MSG = 3
};

constexpr std::int32_t SCE_MSG_DIALOG_MODE_USER_MSG     = 1;
constexpr std::int32_t SCE_MSG_DIALOG_MODE_PROGRESS_BAR = 2;
constexpr std::int32_t SCE_MSG_DIALOG_MODE_SYSTEM_MSG   = 3;

// Tipos de mensagem do sistema
enum class SceMsgDialogSysMsgType : std::int32_t {
    TRC_EMPTY_STORE = 1,
    SAVE_DATA_CORRUPT = 2,
    SAVE_DATA_NOT_ENOUGH_SPACE = 3,
    AUTO_SAVE_WARNING = 4,
    NETWORK_DISCONNECTED = 5
};

constexpr std::int32_t SCE_MSG_DIALOG_SYSMSG_TYPE_TRC_EMPTY_STORE            = 1;
constexpr std::int32_t SCE_MSG_DIALOG_SYSMSG_TYPE_SAVE_DATA_CORRUPT          = 2;
constexpr std::int32_t SCE_MSG_DIALOG_SYSMSG_TYPE_SAVE_DATA_NOT_ENOUGH_SPACE = 3;
constexpr std::int32_t SCE_MSG_DIALOG_SYSMSG_TYPE_AUTO_SAVE_WARNING          = 4;
constexpr std::int32_t SCE_MSG_DIALOG_SYSMSG_TYPE_NETWORK_DISCONNECTED       = 5;

// Codificação do texto do teclado virtual (IME)
enum class SceImeTextType : std::uint32_t {
    UTF8 = 0,
    UTF16 = 1
};

constexpr std::uint32_t SCE_IME_TEXT_TYPE_UTF8  = 0;
constexpr std::uint32_t SCE_IME_TEXT_TYPE_UTF16 = 1;

#pragma pack(push, 1)

// Estrutura de resultado do diálogo de mensagem na memória guest / host
struct SceMsgDialogResult {
    std::int32_t mode{0};
    std::int32_t result{0};
    std::int32_t button_id{0};
    std::uint8_t reserved[28]{0};
};
static_assert(sizeof(SceMsgDialogResult) == 40, "SceMsgDialogResult must be 40 bytes");

// Parâmetros de mensagem de usuário do MsgDialog
struct SceMsgDialogUserMessageParam {
    std::int32_t button_type{0};
    std::uint32_t reserved{0};
    std::uint64_t msg_addr{0}; // const char* guest address
};

// Estrutura principal de parâmetros do MsgDialog
struct SceMsgDialogParam {
    std::uint64_t size{sizeof(SceMsgDialogParam)};
    std::int32_t mode{SCE_MSG_DIALOG_MODE_USER_MSG};
    std::int32_t sys_msg_type{0};
    SceMsgDialogUserMessageParam user_msg_param{};
    std::uint8_t reserved[32]{0};
};

// Parâmetros do teclado virtual (libSceImeDialog)
struct SceImeDialogParam {
    std::uint64_t size{sizeof(SceImeDialogParam)};
    std::uint32_t type{0};              // Tipo de teclado
    std::uint32_t text_type{0};         // 0 = UTF-8, 1 = UTF-16
    std::uint32_t max_text_length{256}; // Máximo de caracteres (excluindo terminador nulo)
    std::uint32_t option{0};
    std::uint64_t title_addr{0};        // Ponteiro guest para título
    std::uint64_t initial_text_addr{0}; // Ponteiro guest para texto inicial
    std::uint64_t input_text_buffer{0}; // Ponteiro guest para gravação do texto resultante
    std::uint8_t reserved[32]{0};
};

// Resultado do teclado virtual (libSceImeDialog)
struct SceImeDialogResult {
    std::int32_t end_status{0};         // 0 = OK, 1 = USER_CANCELED, 2 = ABORTED
    std::uint32_t text_length{0};       // Quantidade de caracteres
    char text[256]{0};                  // String UTF-8 terminada em nulo
    std::uint8_t reserved[16]{0};
};
static_assert(sizeof(SceImeDialogResult) == 280, "SceImeDialogResult must be 280 bytes");

#pragma pack(pop)

// Requisições para callbacks / integração com UIAlertController no host iOS
struct MsgDialogRequest {
    std::string message;
    SceMsgDialogButtonType button_type{SceMsgDialogButtonType::OK};
    SceMsgDialogMode mode{SceMsgDialogMode::USER_MSG};
    SceMsgDialogSysMsgType sys_msg_type{SceMsgDialogSysMsgType::AUTO_SAVE_WARNING};
};

struct ImeDialogRequest {
    std::string title;
    std::string initial_text;
    std::uint32_t max_text_length{256};
    SceImeTextType text_type{SceImeTextType::UTF8};
};

using MsgDialogCallback = std::function<void(const MsgDialogRequest&)>;
using ImeDialogCallback = std::function<void(const ImeDialogRequest&)>;

// DialogManager: gerencia o ciclo de vida completo dos diálogos do Orbis OS
class DialogManager {
public:
    DialogManager();
    ~DialogManager() = default;

    // --- CommonDialog ---
    std::int32_t sceCommonDialogInitialize();
    std::int32_t sceCommonDialogGetStatus() const;
    bool is_common_initialized() const;

    // --- MsgDialog ---
    std::int32_t sceMsgDialogInitialize();
    std::int32_t sceMsgDialogOpen(const SceMsgDialogParam* param);
    std::int32_t sceMsgDialogOpen(GuestMemory& mem, std::uint64_t param_addr);
    std::int32_t sceMsgDialogOpen(const std::string& message,
                                  SceMsgDialogButtonType button_type = SceMsgDialogButtonType::OK,
                                  SceMsgDialogMode mode = SceMsgDialogMode::USER_MSG,
                                  SceMsgDialogSysMsgType sys_msg_type = SceMsgDialogSysMsgType::AUTO_SAVE_WARNING);

    std::int32_t sceMsgDialogGetStatus() const;
    std::int32_t sceMsgDialogUpdateStatus();
    std::int32_t sceMsgDialogGetResult(SceMsgDialogResult* result);
    std::int32_t sceMsgDialogGetResult(GuestMemory& mem, std::uint64_t result_addr);
    std::int32_t sceMsgDialogTerminate();

    // --- ImeDialog ---
    std::int32_t sceImeDialogInit(const SceImeDialogParam* param, const void* extended_param = nullptr);
    std::int32_t sceImeDialogInit(GuestMemory& mem, std::uint64_t param_addr);
    std::int32_t sceImeDialogInit(const std::string& title,
                                  const std::string& initial_text = "",
                                  std::uint32_t max_text_length = 256,
                                  SceImeTextType text_type = SceImeTextType::UTF8);

    std::int32_t sceImeDialogGetStatus();
    std::int32_t sceImeDialogGetResult(SceImeDialogResult* result);
    std::int32_t sceImeDialogGetResult(GuestMemory& mem, std::uint64_t result_addr);
    std::int32_t sceImeDialogTerm();
    std::int32_t sceImeDialogAbort();

    // --- Modos determinísticos (Headless) e Eventos (Host iOS) ---
    void set_headless_mode(bool headless);
    bool is_headless_mode() const;

    void set_headless_default_button(SceMsgDialogButtonId button_id);
    SceMsgDialogButtonId get_headless_default_button() const;

    void set_headless_default_ime_text(const std::string& text);
    std::string get_headless_default_ime_text() const;

    void set_ime_auto_finish(bool auto_finish, std::uint32_t delay_queries = 0);

    // Callbacks do host
    void set_msg_dialog_callback(MsgDialogCallback callback);
    void set_ime_dialog_callback(ImeDialogCallback callback);

    // Resposta manual/interativa (UIAlertController ou testes)
    bool submit_msg_result(SceMsgDialogButtonId button_id);
    bool submit_ime_result(const std::string& text, std::int32_t end_status = SCE_IME_DIALOG_END_STATUS_OK);

    // Reseta estado completo do subsistema
    void reset();

    // Utilitários estáticos de validação e leitura/escrita de strings em GuestMemory
    static bool read_guest_string(const GuestMemory& mem, std::uint64_t addr, std::string& out, std::size_t max_len = SCE_DIALOG_MAX_STRING_LENGTH);
    static bool read_guest_string_u16(const GuestMemory& mem, std::uint64_t addr, std::u16string& out, std::size_t max_chars = SCE_DIALOG_MAX_STRING_LENGTH);
    static bool write_guest_string(GuestMemory& mem, std::uint64_t addr, const std::string& str, std::size_t max_bytes = SCE_DIALOG_MAX_STRING_LENGTH);
    static bool write_guest_string_u16(GuestMemory& mem, std::uint64_t addr, const std::u16string& str, std::size_t max_chars = SCE_DIALOG_MAX_STRING_LENGTH);

    // Utilitários de conversão UTF-8 <-> UTF-16
    static std::u16string utf8_to_utf16(const std::string& u8);
    static std::string utf16_to_utf8(const std::u16string& u16);

private:
    mutable std::mutex mutex_;

    // Estado do CommonDialog
    bool common_initialized_{false};

    // Estado do MsgDialog
    bool msg_initialized_{false};
    std::int32_t msg_status_{SCE_MSG_DIALOG_STATUS_NONE};
    SceMsgDialogMode msg_mode_{SceMsgDialogMode::USER_MSG};
    SceMsgDialogSysMsgType msg_sys_type_{SceMsgDialogSysMsgType::AUTO_SAVE_WARNING};
    SceMsgDialogButtonType msg_button_type_{SceMsgDialogButtonType::OK};
    std::string msg_text_;
    SceMsgDialogButtonId msg_result_button_{SceMsgDialogButtonId::OK};

    // Estado do ImeDialog
    bool ime_initialized_{false};
    std::int32_t ime_status_{SCE_IME_DIALOG_STATUS_NONE};
    std::int32_t ime_end_status_{SCE_IME_DIALOG_END_STATUS_OK};
    std::string ime_title_;
    std::string ime_text_;
    std::uint32_t ime_max_length_{256};
    SceImeTextType ime_text_type_{SceImeTextType::UTF8};
    std::uint64_t ime_input_buffer_addr_{0};
    GuestMemory* ime_guest_memory_{nullptr};

    // Configuração de modo headless determinístico
    bool headless_mode_{true};
    SceMsgDialogButtonId headless_default_button_{SceMsgDialogButtonId::OK};
    std::string headless_default_ime_text_{"Hunter"};
    bool ime_auto_finish_{true};
    std::uint32_t ime_delay_queries_{0};
    std::uint32_t ime_query_count_{0};

    // Callbacks de evento para integração com host iOS
    MsgDialogCallback msg_callback_;
    ImeDialogCallback ime_callback_;
};

// Instância padrão global para chamadas HLE estáticas
DialogManager& default_dialog_manager();

// Funções C-style / wrappers globais HLE do Orbis OS
std::int32_t sceCommonDialogInitialize();
std::int32_t sceCommonDialogGetStatus();

std::int32_t sceMsgDialogInitialize();
std::int32_t sceMsgDialogOpen(const SceMsgDialogParam* param);
std::int32_t sceMsgDialogOpen(GuestMemory& mem, std::uint64_t param_addr);
std::int32_t sceMsgDialogOpen(const std::string& message,
                              SceMsgDialogButtonType button_type = SceMsgDialogButtonType::OK,
                              SceMsgDialogMode mode = SceMsgDialogMode::USER_MSG,
                              SceMsgDialogSysMsgType sys_msg_type = SceMsgDialogSysMsgType::AUTO_SAVE_WARNING);
std::int32_t sceMsgDialogGetStatus();
std::int32_t sceMsgDialogUpdateStatus();
std::int32_t sceMsgDialogGetResult(SceMsgDialogResult* result);
std::int32_t sceMsgDialogGetResult(GuestMemory& mem, std::uint64_t result_addr);
std::int32_t sceMsgDialogTerminate();

std::int32_t sceImeDialogInit(const SceImeDialogParam* param, const void* extended_param = nullptr);
std::int32_t sceImeDialogInit(GuestMemory& mem, std::uint64_t param_addr);
std::int32_t sceImeDialogInit(const std::string& title,
                              const std::string& initial_text = "",
                              std::uint32_t max_text_length = 256,
                              SceImeTextType text_type = SceImeTextType::UTF8);
std::int32_t sceImeDialogGetStatus();
std::int32_t sceImeDialogGetResult(SceImeDialogResult* result);
std::int32_t sceImeDialogGetResult(GuestMemory& mem, std::uint64_t result_addr);
std::int32_t sceImeDialogTerm();
std::int32_t sceImeDialogAbort();

} // namespace bb
