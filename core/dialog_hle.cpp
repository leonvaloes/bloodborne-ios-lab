#include "dialog_hle.h"
#include <algorithm>
#include <cstring>

namespace bb {

DialogManager::DialogManager() = default;

// Utilitários de conversão UTF-8 <-> UTF-16 sem uso do cabeçalho depreciado <codecvt>
std::u16string DialogManager::utf8_to_utf16(const std::string& u8) {
    std::u16string u16;
    u16.reserve(u8.size());
    std::size_t i = 0;
    while (i < u8.size()) {
        const auto c = static_cast<std::uint8_t>(u8[i]);
        if (c < 0x80) {
            u16.push_back(static_cast<char16_t>(c));
            ++i;
        } else if ((c & 0xE0) == 0xC0) {
            if (i + 1 >= u8.size()) break;
            const std::uint32_t cp = ((c & 0x1F) << 6) | (static_cast<std::uint8_t>(u8[i + 1]) & 0x3F);
            u16.push_back(static_cast<char16_t>(cp));
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 >= u8.size()) break;
            const std::uint32_t cp = ((c & 0x0F) << 12) |
                                     ((static_cast<std::uint8_t>(u8[i + 1]) & 0x3F) << 6) |
                                     (static_cast<std::uint8_t>(u8[i + 2]) & 0x3F);
            u16.push_back(static_cast<char16_t>(cp));
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            if (i + 3 >= u8.size()) break;
            std::uint32_t cp = ((c & 0x07) << 18) |
                               ((static_cast<std::uint8_t>(u8[i + 1]) & 0x3F) << 12) |
                               ((static_cast<std::uint8_t>(u8[i + 2]) & 0x3F) << 6) |
                               (static_cast<std::uint8_t>(u8[i + 3]) & 0x3F);
            if (cp >= 0x10000) {
                cp -= 0x10000;
                u16.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
                u16.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
            } else {
                u16.push_back(static_cast<char16_t>(cp));
            }
            i += 4;
        } else {
            ++i;
        }
    }
    return u16;
}

std::string DialogManager::utf16_to_utf8(const std::u16string& u16) {
    std::string u8;
    u8.reserve(u16.size() * 3);
    for (std::size_t i = 0; i < u16.size(); ++i) {
        std::uint32_t cp = static_cast<std::uint16_t>(u16[i]);
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (i + 1 < u16.size()) {
                const auto low = static_cast<std::uint16_t>(u16[i + 1]);
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                    ++i;
                }
            }
        }
        if (cp < 0x80) {
            u8.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            u8.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            u8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            u8.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            u8.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            u8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            u8.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            u8.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            u8.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            u8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return u8;
}

bool DialogManager::read_guest_string(const GuestMemory& mem, std::uint64_t addr, std::string& out, std::size_t max_len) {
    if (addr == 0 || max_len == 0) {
        return false;
    }
    out.clear();
    for (std::size_t i = 0; i < max_len; ++i) {
        char ch = 0;
        if (mem.load(addr + i, &ch, 1) != MemoryError::ok) {
            return false;
        }
        if (ch == '\0') {
            return true;
        }
        out.push_back(ch);
    }
    return false;
}

bool DialogManager::read_guest_string_u16(const GuestMemory& mem, std::uint64_t addr, std::u16string& out, std::size_t max_chars) {
    if (addr == 0 || max_chars == 0) {
        return false;
    }
    out.clear();
    for (std::size_t i = 0; i < max_chars; ++i) {
        char16_t ch = 0;
        if (mem.load(addr + i * sizeof(char16_t), &ch, sizeof(char16_t)) != MemoryError::ok) {
            return false;
        }
        if (ch == 0) {
            return true;
        }
        out.push_back(ch);
    }
    return false;
}

bool DialogManager::write_guest_string(GuestMemory& mem, std::uint64_t addr, const std::string& str, std::size_t max_bytes) {
    if (addr == 0 || str.size() + 1 > max_bytes || str.size() > SCE_DIALOG_MAX_STRING_LENGTH) {
        return false;
    }
    return mem.store(addr, str.c_str(), str.size() + 1) == MemoryError::ok;
}

bool DialogManager::write_guest_string_u16(GuestMemory& mem, std::uint64_t addr, const std::u16string& str, std::size_t max_chars) {
    if (addr == 0 || str.size() + 1 > max_chars || str.size() > SCE_DIALOG_MAX_STRING_LENGTH) {
        return false;
    }
    std::vector<char16_t> buffer(str.begin(), str.end());
    buffer.push_back(0);
    const std::size_t bytes = buffer.size() * sizeof(char16_t);
    return mem.store(addr, buffer.data(), bytes) == MemoryError::ok;
}

// --- CommonDialog ---
std::int32_t DialogManager::sceCommonDialogInitialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (common_initialized_) {
        return SCE_COMMON_DIALOG_ERROR_ALREADY_INITIALIZED;
    }
    common_initialized_ = true;
    return SCE_OK;
}

std::int32_t DialogManager::sceCommonDialogGetStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!common_initialized_) {
        return SCE_COMMON_DIALOG_STATUS_NONE;
    }
    if (msg_status_ == SCE_MSG_DIALOG_STATUS_RUNNING || ime_status_ == SCE_IME_DIALOG_STATUS_RUNNING) {
        return SCE_COMMON_DIALOG_STATUS_RUNNING;
    }
    if (msg_status_ == SCE_MSG_DIALOG_STATUS_FINISHED || ime_status_ == SCE_IME_DIALOG_STATUS_FINISHED) {
        return SCE_COMMON_DIALOG_STATUS_FINISHED;
    }
    return SCE_COMMON_DIALOG_STATUS_INITIALIZED;
}

bool DialogManager::is_common_initialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return common_initialized_;
}

// --- MsgDialog ---
std::int32_t DialogManager::sceMsgDialogInitialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (msg_initialized_) {
        return SCE_MSG_DIALOG_ERROR_ALREADY_INITIALIZED;
    }
    common_initialized_ = true;
    msg_initialized_ = true;
    msg_status_ = SCE_MSG_DIALOG_STATUS_INITIALIZED;
    msg_result_button_ = headless_default_button_;
    return SCE_OK;
}

std::int32_t DialogManager::sceMsgDialogOpen(const SceMsgDialogParam* param) {
    if (param == nullptr) {
        return SCE_MSG_DIALOG_ERROR_INVALID_ARG;
    }
    if (param->size != sizeof(SceMsgDialogParam)) {
        return SCE_MSG_DIALOG_ERROR_INVALID_ARG;
    }

    std::string msg;
    if (param->user_msg_param.msg_addr != 0) {
        const auto* raw_ptr = reinterpret_cast<const char*>(param->user_msg_param.msg_addr);
        std::size_t len = 0;
        while (len <= SCE_DIALOG_MAX_STRING_LENGTH && raw_ptr[len] != '\0') {
            ++len;
        }
        if (len > SCE_DIALOG_MAX_STRING_LENGTH) {
            return SCE_MSG_DIALOG_ERROR_INVALID_ARG;
        }
        msg.assign(raw_ptr, len);
    }

    const auto btn_type = static_cast<SceMsgDialogButtonType>(param->user_msg_param.button_type);
    const auto mode = static_cast<SceMsgDialogMode>(param->mode);
    const auto sys_type = static_cast<SceMsgDialogSysMsgType>(param->sys_msg_type);
    return sceMsgDialogOpen(msg, btn_type, mode, sys_type);
}

std::int32_t DialogManager::sceMsgDialogOpen(GuestMemory& mem, std::uint64_t param_addr) {
    if (param_addr == 0) {
        return SCE_MSG_DIALOG_ERROR_INVALID_ARG;
    }

    SceMsgDialogParam param{};
    if (mem.load(param_addr, &param, sizeof(param)) != MemoryError::ok) {
        return SCE_MSG_DIALOG_ERROR_INVALID_ARG;
    }
    if (param.size != sizeof(SceMsgDialogParam)) {
        return SCE_MSG_DIALOG_ERROR_INVALID_ARG;
    }

    std::string msg;
    if (param.user_msg_param.msg_addr != 0) {
        if (!read_guest_string(mem, param.user_msg_param.msg_addr, msg, SCE_DIALOG_MAX_STRING_LENGTH)) {
            return SCE_MSG_DIALOG_ERROR_INVALID_ARG;
        }
    }

    const auto btn_type = static_cast<SceMsgDialogButtonType>(param.user_msg_param.button_type);
    const auto mode = static_cast<SceMsgDialogMode>(param.mode);
    const auto sys_type = static_cast<SceMsgDialogSysMsgType>(param.sys_msg_type);
    return sceMsgDialogOpen(msg, btn_type, mode, sys_type);
}

std::int32_t DialogManager::sceMsgDialogOpen(const std::string& message,
                                            SceMsgDialogButtonType button_type,
                                            SceMsgDialogMode mode,
                                            SceMsgDialogSysMsgType sys_msg_type) {
    MsgDialogCallback cb;
    MsgDialogRequest req;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!msg_initialized_) {
            return SCE_MSG_DIALOG_ERROR_NOT_INITIALIZED;
        }
        if (msg_status_ == SCE_MSG_DIALOG_STATUS_RUNNING) {
            return SCE_MSG_DIALOG_ERROR_BUSY;
        }
        if (message.size() > SCE_DIALOG_MAX_STRING_LENGTH) {
            return SCE_MSG_DIALOG_ERROR_INVALID_ARG;
        }

        const auto b_val = static_cast<std::int32_t>(button_type);
        if (b_val < 0 || b_val > 7) {
            return SCE_MSG_DIALOG_ERROR_INVALID_ARG;
        }

        const auto m_val = static_cast<std::int32_t>(mode);
        if (m_val < 1 || m_val > 3) {
            return SCE_MSG_DIALOG_ERROR_INVALID_ARG;
        }

        msg_text_ = message;
        msg_button_type_ = button_type;
        msg_mode_ = mode;
        msg_sys_type_ = sys_msg_type;
        msg_status_ = SCE_MSG_DIALOG_STATUS_RUNNING;

        // Determinação do botão padrão em modo headless
        if (headless_mode_) {
            if (headless_default_button_ != SceMsgDialogButtonId::INVALID) {
                msg_result_button_ = headless_default_button_;
            } else {
                switch (button_type) {
                case SceMsgDialogButtonType::YESNO:
                case SceMsgDialogButtonType::YESNO_FOCUS_NO:
                    msg_result_button_ = SceMsgDialogButtonId::YES;
                    break;
                case SceMsgDialogButtonType::RETRY_CANCEL:
                    msg_result_button_ = SceMsgDialogButtonId::RETRY;
                    break;
                default:
                    msg_result_button_ = SceMsgDialogButtonId::OK;
                    break;
                }
            }
        }

        req.message = msg_text_;
        req.button_type = msg_button_type_;
        req.mode = msg_mode_;
        req.sys_msg_type = msg_sys_type_;
        cb = msg_callback_;
    }

    if (cb) {
        cb(req);
    }
    return SCE_OK;
}

std::int32_t DialogManager::sceMsgDialogGetStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!msg_initialized_) {
        return SCE_MSG_DIALOG_STATUS_NONE;
    }
    return msg_status_;
}

std::int32_t DialogManager::sceMsgDialogUpdateStatus() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!msg_initialized_) {
        return SCE_MSG_DIALOG_STATUS_NONE;
    }
    if (msg_status_ == SCE_MSG_DIALOG_STATUS_RUNNING) {
        if (headless_mode_) {
            msg_status_ = SCE_MSG_DIALOG_STATUS_FINISHED;
        }
    }
    return msg_status_;
}

std::int32_t DialogManager::sceMsgDialogGetResult(SceMsgDialogResult* result) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!msg_initialized_) {
        return SCE_MSG_DIALOG_ERROR_NOT_INITIALIZED;
    }
    if (msg_status_ == SCE_MSG_DIALOG_STATUS_RUNNING) {
        return SCE_MSG_DIALOG_ERROR_NOT_FINISHED;
    }
    if (msg_status_ != SCE_MSG_DIALOG_STATUS_FINISHED) {
        return SCE_MSG_DIALOG_ERROR_ILLEGAL_STATE;
    }
    if (result == nullptr) {
        return SCE_MSG_DIALOG_ERROR_INVALID_ARG;
    }

    result->mode = static_cast<std::int32_t>(msg_mode_);
    result->result = SCE_OK;
    result->button_id = static_cast<std::int32_t>(msg_result_button_);
    std::memset(result->reserved, 0, sizeof(result->reserved));
    return SCE_OK;
}

std::int32_t DialogManager::sceMsgDialogGetResult(GuestMemory& mem, std::uint64_t result_addr) {
    if (result_addr == 0) {
        return SCE_MSG_DIALOG_ERROR_INVALID_ARG;
    }
    SceMsgDialogResult res{};
    const auto err = sceMsgDialogGetResult(&res);
    if (err != SCE_OK) {
        return err;
    }
    if (mem.store(result_addr, &res, sizeof(res)) != MemoryError::ok) {
        return SCE_MSG_DIALOG_ERROR_INVALID_ARG;
    }
    return SCE_OK;
}

std::int32_t DialogManager::sceMsgDialogTerminate() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!msg_initialized_) {
        return SCE_MSG_DIALOG_ERROR_NOT_INITIALIZED;
    }
    msg_initialized_ = false;
    msg_status_ = SCE_MSG_DIALOG_STATUS_NONE;
    msg_text_.clear();
    msg_result_button_ = headless_default_button_;
    return SCE_OK;
}

// --- ImeDialog ---
std::int32_t DialogManager::sceImeDialogInit(const SceImeDialogParam* param, const void* /*extended_param*/) {
    if (param == nullptr) {
        return SCE_IME_DIALOG_ERROR_INVALID_ARG;
    }
    if (param->size != sizeof(SceImeDialogParam)) {
        return SCE_IME_DIALOG_ERROR_INVALID_ARG;
    }
    if (param->max_text_length == 0) {
        return SCE_IME_DIALOG_ERROR_INVALID_ARG;
    }
    if (param->max_text_length > SCE_DIALOG_MAX_STRING_LENGTH) {
        return SCE_IME_DIALOG_ERROR_TEXT_TOO_LONG;
    }

    std::string title;
    std::string initial_text;
    if (param->title_addr != 0) {
        const auto* raw_title = reinterpret_cast<const char*>(param->title_addr);
        std::size_t len = 0;
        while (len <= SCE_DIALOG_MAX_STRING_LENGTH && raw_title[len] != '\0') {
            ++len;
        }
        if (len > SCE_DIALOG_MAX_STRING_LENGTH) {
            return SCE_IME_DIALOG_ERROR_TEXT_TOO_LONG;
        }
        title.assign(raw_title, len);
    }
    if (param->initial_text_addr != 0) {
        const auto* raw_text = reinterpret_cast<const char*>(param->initial_text_addr);
        std::size_t len = 0;
        while (len <= SCE_DIALOG_MAX_STRING_LENGTH && raw_text[len] != '\0') {
            ++len;
        }
        if (len > SCE_DIALOG_MAX_STRING_LENGTH) {
            return SCE_IME_DIALOG_ERROR_TEXT_TOO_LONG;
        }
        initial_text.assign(raw_text, len);
    }

    const auto t_type = (param->text_type == SCE_IME_TEXT_TYPE_UTF16) ? SceImeTextType::UTF16 : SceImeTextType::UTF8;
    return sceImeDialogInit(title, initial_text, param->max_text_length, t_type);
}

std::int32_t DialogManager::sceImeDialogInit(GuestMemory& mem, std::uint64_t param_addr) {
    if (param_addr == 0) {
        return SCE_IME_DIALOG_ERROR_INVALID_ARG;
    }

    SceImeDialogParam param{};
    if (mem.load(param_addr, &param, sizeof(param)) != MemoryError::ok) {
        return SCE_IME_DIALOG_ERROR_INVALID_ARG;
    }
    if (param.size != sizeof(SceImeDialogParam)) {
        return SCE_IME_DIALOG_ERROR_INVALID_ARG;
    }
    if (param.max_text_length == 0) {
        return SCE_IME_DIALOG_ERROR_INVALID_ARG;
    }
    if (param.max_text_length > SCE_DIALOG_MAX_STRING_LENGTH) {
        return SCE_IME_DIALOG_ERROR_TEXT_TOO_LONG;
    }

    std::string title;
    std::string initial_text;
    const bool is_u16 = (param.text_type == SCE_IME_TEXT_TYPE_UTF16);

    if (param.title_addr != 0) {
        if (is_u16) {
            std::u16string u16;
            if (!read_guest_string_u16(mem, param.title_addr, u16, SCE_DIALOG_MAX_STRING_LENGTH)) {
                return SCE_IME_DIALOG_ERROR_INVALID_ARG;
            }
            title = utf16_to_utf8(u16);
        } else {
            if (!read_guest_string(mem, param.title_addr, title, SCE_DIALOG_MAX_STRING_LENGTH)) {
                return SCE_IME_DIALOG_ERROR_INVALID_ARG;
            }
        }
    }

    if (param.initial_text_addr != 0) {
        if (is_u16) {
            std::u16string u16;
            if (!read_guest_string_u16(mem, param.initial_text_addr, u16, SCE_DIALOG_MAX_STRING_LENGTH)) {
                return SCE_IME_DIALOG_ERROR_INVALID_ARG;
            }
            initial_text = utf16_to_utf8(u16);
        } else {
            if (!read_guest_string(mem, param.initial_text_addr, initial_text, SCE_DIALOG_MAX_STRING_LENGTH)) {
                return SCE_IME_DIALOG_ERROR_INVALID_ARG;
            }
        }
    }

    const auto t_type = is_u16 ? SceImeTextType::UTF16 : SceImeTextType::UTF8;
    const auto err = sceImeDialogInit(title, initial_text, param.max_text_length, t_type);
    if (err == SCE_OK) {
        std::lock_guard<std::mutex> lock(mutex_);
        ime_input_buffer_addr_ = param.input_text_buffer;
        ime_guest_memory_ = &mem;
    }
    return err;
}

std::int32_t DialogManager::sceImeDialogInit(const std::string& title,
                                            const std::string& initial_text,
                                            std::uint32_t max_text_length,
                                            SceImeTextType text_type) {
    ImeDialogCallback cb;
    ImeDialogRequest req;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ime_status_ == SCE_IME_DIALOG_STATUS_RUNNING) {
            return SCE_IME_DIALOG_ERROR_BUSY;
        }
        if (max_text_length == 0) {
            return SCE_IME_DIALOG_ERROR_INVALID_ARG;
        }
        if (max_text_length > SCE_DIALOG_MAX_STRING_LENGTH) {
            return SCE_IME_DIALOG_ERROR_TEXT_TOO_LONG;
        }
        if (title.size() > SCE_DIALOG_MAX_STRING_LENGTH || initial_text.size() > SCE_DIALOG_MAX_STRING_LENGTH) {
            return SCE_IME_DIALOG_ERROR_TEXT_TOO_LONG;
        }

        common_initialized_ = true;
        ime_initialized_ = true;
        ime_title_ = title;
        ime_max_length_ = max_text_length;
        ime_text_type_ = text_type;
        ime_input_buffer_addr_ = 0;
        ime_guest_memory_ = nullptr;
        ime_query_count_ = 0;

        if (!initial_text.empty()) {
            ime_text_ = initial_text.substr(0, std::min<std::size_t>(initial_text.size(), ime_max_length_));
        } else if (headless_mode_) {
            ime_text_ = headless_default_ime_text_.substr(0, std::min<std::size_t>(headless_default_ime_text_.size(), ime_max_length_));
        } else {
            ime_text_.clear();
        }

        ime_status_ = SCE_IME_DIALOG_STATUS_RUNNING;
        ime_end_status_ = SCE_IME_DIALOG_END_STATUS_OK;

        if (headless_mode_ && ime_auto_finish_ && ime_delay_queries_ == 0) {
            ime_status_ = SCE_IME_DIALOG_STATUS_FINISHED;
        }

        req.title = ime_title_;
        req.initial_text = ime_text_;
        req.max_text_length = ime_max_length_;
        req.text_type = ime_text_type_;
        cb = ime_callback_;
    }

    if (cb) {
        cb(req);
    }
    return SCE_OK;
}

std::int32_t DialogManager::sceImeDialogGetStatus() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ime_initialized_) {
        return SCE_IME_DIALOG_STATUS_NONE;
    }
    if (ime_status_ == SCE_IME_DIALOG_STATUS_RUNNING) {
        if (headless_mode_ && ime_auto_finish_) {
            ++ime_query_count_;
            if (ime_query_count_ > ime_delay_queries_) {
                ime_status_ = SCE_IME_DIALOG_STATUS_FINISHED;
                ime_end_status_ = SCE_IME_DIALOG_END_STATUS_OK;
            }
        }
    }
    return ime_status_;
}

std::int32_t DialogManager::sceImeDialogGetResult(SceImeDialogResult* result) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ime_initialized_) {
        return SCE_IME_DIALOG_ERROR_NOT_INITIALIZED;
    }
    if (ime_status_ == SCE_IME_DIALOG_STATUS_RUNNING) {
        return SCE_IME_DIALOG_ERROR_NOT_FINISHED;
    }
    if (result == nullptr) {
        return SCE_IME_DIALOG_ERROR_INVALID_ARG;
    }

    result->end_status = ime_end_status_;
    result->text_length = static_cast<std::uint32_t>(ime_text_.size());
    std::memset(result->text, 0, sizeof(result->text));
    const std::size_t copy_bytes = std::min(ime_text_.size(), sizeof(result->text) - 1);
    std::memcpy(result->text, ime_text_.data(), copy_bytes);
    result->text[copy_bytes] = '\0';
    std::memset(result->reserved, 0, sizeof(result->reserved));
    return SCE_OK;
}

std::int32_t DialogManager::sceImeDialogGetResult(GuestMemory& mem, std::uint64_t result_addr) {
    if (result_addr == 0) {
        return SCE_IME_DIALOG_ERROR_INVALID_ARG;
    }

    SceImeDialogResult res{};
    const auto err = sceImeDialogGetResult(&res);
    if (err != SCE_OK) {
        return err;
    }

    if (mem.store(result_addr, &res, sizeof(res)) != MemoryError::ok) {
        return SCE_IME_DIALOG_ERROR_INVALID_ARG;
    }

    // Gravação adicional no buffer de entrada especificado nos parâmetros guest do IME
    std::lock_guard<std::mutex> lock(mutex_);
    if (ime_input_buffer_addr_ != 0) {
        if (ime_text_type_ == SceImeTextType::UTF16) {
            const std::u16string u16 = utf8_to_utf16(ime_text_);
            if (!write_guest_string_u16(mem, ime_input_buffer_addr_, u16, ime_max_length_ + 1)) {
                return SCE_IME_DIALOG_ERROR_INVALID_ARG;
            }
        } else {
            if (!write_guest_string(mem, ime_input_buffer_addr_, ime_text_, ime_max_length_ + 1)) {
                return SCE_IME_DIALOG_ERROR_INVALID_ARG;
            }
        }
    }

    return SCE_OK;
}

std::int32_t DialogManager::sceImeDialogTerm() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ime_initialized_) {
        return SCE_IME_DIALOG_ERROR_NOT_INITIALIZED;
    }
    ime_initialized_ = false;
    ime_status_ = SCE_IME_DIALOG_STATUS_NONE;
    ime_title_.clear();
    ime_text_.clear();
    ime_input_buffer_addr_ = 0;
    ime_guest_memory_ = nullptr;
    ime_query_count_ = 0;
    return SCE_OK;
}

std::int32_t DialogManager::sceImeDialogAbort() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ime_status_ != SCE_IME_DIALOG_STATUS_RUNNING) {
        return SCE_IME_DIALOG_ERROR_NOT_RUNNING;
    }
    ime_status_ = SCE_IME_DIALOG_STATUS_ABORTED;
    ime_end_status_ = SCE_IME_DIALOG_END_STATUS_ABORTED;
    return SCE_OK;
}

// --- Configurações e Modos ---
void DialogManager::set_headless_mode(bool headless) {
    std::lock_guard<std::mutex> lock(mutex_);
    headless_mode_ = headless;
}

bool DialogManager::is_headless_mode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return headless_mode_;
}

void DialogManager::set_headless_default_button(SceMsgDialogButtonId button_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    headless_default_button_ = button_id;
}

SceMsgDialogButtonId DialogManager::get_headless_default_button() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return headless_default_button_;
}

void DialogManager::set_headless_default_ime_text(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    headless_default_ime_text_ = text.substr(0, std::min<std::size_t>(text.size(), SCE_DIALOG_MAX_STRING_LENGTH));
}

std::string DialogManager::get_headless_default_ime_text() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return headless_default_ime_text_;
}

void DialogManager::set_ime_auto_finish(bool auto_finish, std::uint32_t delay_queries) {
    std::lock_guard<std::mutex> lock(mutex_);
    ime_auto_finish_ = auto_finish;
    ime_delay_queries_ = delay_queries;
}

void DialogManager::set_msg_dialog_callback(MsgDialogCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    msg_callback_ = std::move(callback);
}

void DialogManager::set_ime_dialog_callback(ImeDialogCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    ime_callback_ = std::move(callback);
}

bool DialogManager::submit_msg_result(SceMsgDialogButtonId button_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (msg_status_ != SCE_MSG_DIALOG_STATUS_RUNNING) {
        return false;
    }
    msg_result_button_ = button_id;
    msg_status_ = SCE_MSG_DIALOG_STATUS_FINISHED;
    return true;
}

bool DialogManager::submit_ime_result(const std::string& text, std::int32_t end_status) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ime_status_ != SCE_IME_DIALOG_STATUS_RUNNING) {
        return false;
    }
    const std::size_t limit = std::min<std::size_t>(ime_max_length_, SCE_DIALOG_MAX_STRING_LENGTH);
    ime_text_ = text.substr(0, std::min<std::size_t>(text.size(), limit));
    ime_end_status_ = end_status;
    ime_status_ = (end_status == SCE_IME_DIALOG_END_STATUS_ABORTED) ? SCE_IME_DIALOG_STATUS_ABORTED : SCE_IME_DIALOG_STATUS_FINISHED;
    return true;
}

void DialogManager::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    common_initialized_ = false;
    msg_initialized_ = false;
    msg_status_ = SCE_MSG_DIALOG_STATUS_NONE;
    msg_mode_ = SceMsgDialogMode::USER_MSG;
    msg_sys_type_ = SceMsgDialogSysMsgType::AUTO_SAVE_WARNING;
    msg_button_type_ = SceMsgDialogButtonType::OK;
    msg_text_.clear();
    msg_result_button_ = SceMsgDialogButtonId::OK;

    ime_initialized_ = false;
    ime_status_ = SCE_IME_DIALOG_STATUS_NONE;
    ime_end_status_ = SCE_IME_DIALOG_END_STATUS_OK;
    ime_title_.clear();
    ime_text_.clear();
    ime_max_length_ = 256;
    ime_text_type_ = SceImeTextType::UTF8;
    ime_input_buffer_addr_ = 0;
    ime_guest_memory_ = nullptr;

    headless_mode_ = true;
    headless_default_button_ = SceMsgDialogButtonId::OK;
    headless_default_ime_text_ = "Hunter";
    ime_auto_finish_ = true;
    ime_delay_queries_ = 0;
    ime_query_count_ = 0;

    msg_callback_ = nullptr;
    ime_callback_ = nullptr;
}

// Instância singleton padrão global
DialogManager& default_dialog_manager() {
    static DialogManager instance;
    return instance;
}

// Wrappers C-style para compatibilidade com o subsistema Orbis HLE
std::int32_t sceCommonDialogInitialize() {
    return default_dialog_manager().sceCommonDialogInitialize();
}

std::int32_t sceCommonDialogGetStatus() {
    return default_dialog_manager().sceCommonDialogGetStatus();
}

std::int32_t sceMsgDialogInitialize() {
    return default_dialog_manager().sceMsgDialogInitialize();
}

std::int32_t sceMsgDialogOpen(const SceMsgDialogParam* param) {
    return default_dialog_manager().sceMsgDialogOpen(param);
}

std::int32_t sceMsgDialogOpen(GuestMemory& mem, std::uint64_t param_addr) {
    return default_dialog_manager().sceMsgDialogOpen(mem, param_addr);
}

std::int32_t sceMsgDialogGetStatus() {
    return default_dialog_manager().sceMsgDialogGetStatus();
}

std::int32_t sceMsgDialogUpdateStatus() {
    return default_dialog_manager().sceMsgDialogUpdateStatus();
}

std::int32_t sceMsgDialogGetResult(SceMsgDialogResult* result) {
    return default_dialog_manager().sceMsgDialogGetResult(result);
}

std::int32_t sceMsgDialogGetResult(GuestMemory& mem, std::uint64_t result_addr) {
    return default_dialog_manager().sceMsgDialogGetResult(mem, result_addr);
}

std::int32_t sceMsgDialogTerminate() {
    return default_dialog_manager().sceMsgDialogTerminate();
}

std::int32_t sceMsgDialogOpen(const std::string& message,
                              SceMsgDialogButtonType button_type,
                              SceMsgDialogMode mode,
                              SceMsgDialogSysMsgType sys_msg_type) {
    return default_dialog_manager().sceMsgDialogOpen(message, button_type, mode, sys_msg_type);
}

std::int32_t sceImeDialogInit(const SceImeDialogParam* param, const void* extended_param) {
    return default_dialog_manager().sceImeDialogInit(param, extended_param);
}

std::int32_t sceImeDialogInit(GuestMemory& mem, std::uint64_t param_addr) {
    return default_dialog_manager().sceImeDialogInit(mem, param_addr);
}

std::int32_t sceImeDialogInit(const std::string& title,
                              const std::string& initial_text,
                              std::uint32_t max_text_length,
                              SceImeTextType text_type) {
    return default_dialog_manager().sceImeDialogInit(title, initial_text, max_text_length, text_type);
}

std::int32_t sceImeDialogGetStatus() {
    return default_dialog_manager().sceImeDialogGetStatus();
}

std::int32_t sceImeDialogGetResult(SceImeDialogResult* result) {
    return default_dialog_manager().sceImeDialogGetResult(result);
}

std::int32_t sceImeDialogGetResult(GuestMemory& mem, std::uint64_t result_addr) {
    return default_dialog_manager().sceImeDialogGetResult(mem, result_addr);
}

std::int32_t sceImeDialogTerm() {
    return default_dialog_manager().sceImeDialogTerm();
}

std::int32_t sceImeDialogAbort() {
    return default_dialog_manager().sceImeDialogAbort();
}

} // namespace bb
