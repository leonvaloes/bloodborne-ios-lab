#include "check.h"
#include "dialog_hle.h"
#include "guest_memory.h"
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>
#include <string>

static void test_common_dialog_lifecycle() {
    using namespace bb;
    DialogManager dm;

    // Estado inicial: NONE
    CHECK(dm.sceCommonDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_NONE);
    CHECK(!dm.is_common_initialized());

    // Inicialização
    CHECK(dm.sceCommonDialogInitialize() == SCE_OK);
    CHECK(dm.is_common_initialized());
    CHECK(dm.sceCommonDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_INITIALIZED);

    // Dupla inicialização deve falhar
    CHECK(dm.sceCommonDialogInitialize() == SCE_COMMON_DIALOG_ERROR_ALREADY_INITIALIZED);

    // Reset
    dm.reset();
    CHECK(dm.sceCommonDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_NONE);
    CHECK(!dm.is_common_initialized());
}

static void test_msg_dialog_headless_lifecycle() {
    using namespace bb;
    DialogManager dm;

    // Inicialização do subsistema de mensagens
    CHECK(dm.sceMsgDialogInitialize() == SCE_OK);
    CHECK(dm.sceMsgDialogGetStatus() == SCE_MSG_DIALOG_STATUS_INITIALIZED);

    // Abrir diálogo com mensagem e botões padrão (OK)
    CHECK(dm.sceMsgDialogOpen("Auto-save occurred.", SceMsgDialogButtonType::OK) == SCE_OK);
    CHECK(dm.sceMsgDialogGetStatus() == SCE_MSG_DIALOG_STATUS_RUNNING);

    // Em modo headless, update_status transiciona imediatamente para FINISHED
    CHECK(dm.sceMsgDialogUpdateStatus() == SCE_MSG_DIALOG_STATUS_FINISHED);

    // Obter resultado
    SceMsgDialogResult result{};
    CHECK(dm.sceMsgDialogGetResult(&result) == SCE_OK);
    CHECK(result.mode == SCE_MSG_DIALOG_MODE_USER_MSG);
    CHECK(result.result == SCE_OK);
    CHECK(result.button_id == SCE_MSG_DIALOG_BUTTON_ID_OK);

    // Encerramento
    CHECK(dm.sceMsgDialogTerminate() == SCE_OK);
    CHECK(dm.sceMsgDialogGetStatus() == SCE_MSG_DIALOG_STATUS_NONE);

    // Após término, get_result deve falhar com NOT_INITIALIZED (já que terminate zera initialized)
    CHECK(dm.sceMsgDialogGetResult(&result) == SCE_MSG_DIALOG_ERROR_NOT_INITIALIZED);
}

static void test_msg_dialog_custom_buttons() {
    using namespace bb;
    DialogManager dm;

    dm.sceMsgDialogInitialize();

    // Teste botão YESNO - default headless deve ser YES
    dm.set_headless_default_button(SceMsgDialogButtonId::INVALID); // usa lógica automática
    CHECK(dm.sceMsgDialogOpen("Continue?", SceMsgDialogButtonType::YESNO) == SCE_OK);
    dm.sceMsgDialogUpdateStatus();
    SceMsgDialogResult res{};
    CHECK(dm.sceMsgDialogGetResult(&res) == SCE_OK);
    CHECK(res.button_id == SCE_MSG_DIALOG_BUTTON_ID_YES);
    dm.sceMsgDialogTerminate();

    // Teste botão RETRY_CANCEL - default headless deve ser RETRY
    dm.sceMsgDialogInitialize();
    CHECK(dm.sceMsgDialogOpen("Retry?", SceMsgDialogButtonType::RETRY_CANCEL) == SCE_OK);
    dm.sceMsgDialogUpdateStatus();
    CHECK(dm.sceMsgDialogGetResult(&res) == SCE_OK);
    CHECK(res.button_id == SCE_MSG_DIALOG_BUTTON_ID_RETRY);
    dm.sceMsgDialogTerminate();

    // Teste botão OK_CANCEL - default headless deve ser OK
    dm.sceMsgDialogInitialize();
    CHECK(dm.sceMsgDialogOpen("OK or Cancel?", SceMsgDialogButtonType::OK_CANCEL) == SCE_OK);
    dm.sceMsgDialogUpdateStatus();
    CHECK(dm.sceMsgDialogGetResult(&res) == SCE_OK);
    CHECK(res.button_id == SCE_MSG_DIALOG_BUTTON_ID_OK);
    dm.sceMsgDialogTerminate();
}

static void test_msg_dialog_guest_memory() {
    using namespace bb;
    DialogManager dm;
    GuestMemory mem(4096);
    CHECK(mem.map(0x1000, 4096, read | write) == MemoryError::ok);

    dm.sceMsgDialogInitialize();

    // Preparar SceMsgDialogParam na memória guest
    SceMsgDialogParam param{};
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    param.user_msg_param.button_type = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
    const char msg[] = "Guest memory test";
    CHECK(mem.store(0x1100, msg, sizeof(msg)) == MemoryError::ok);
    param.user_msg_param.msg_addr = 0x1100;

    CHECK(mem.store(0x1000, &param, sizeof(param)) == MemoryError::ok);
    CHECK(dm.sceMsgDialogOpen(mem, 0x1000) == SCE_OK);
    dm.sceMsgDialogUpdateStatus();

    // Ler resultado na memória guest
    CHECK(dm.sceMsgDialogGetResult(mem, 0x1200) == SCE_OK);
    SceMsgDialogResult res{};
    CHECK(mem.load(0x1200, &res, sizeof(res)) == MemoryError::ok);
    CHECK(res.button_id == SCE_MSG_DIALOG_BUTTON_ID_OK);
    CHECK(res.result == SCE_OK);

    dm.sceMsgDialogTerminate();
}

static void test_msg_dialog_invalid_pointers() {
    using namespace bb;
    DialogManager dm;
    GuestMemory mem(1024);
    CHECK(mem.map(0x1000, 512, read | write) == MemoryError::ok);

    // Ponteiro nulo em sceMsgDialogOpen
    CHECK(dm.sceMsgDialogOpen(static_cast<const SceMsgDialogParam*>(nullptr)) == SCE_MSG_DIALOG_ERROR_INVALID_ARG);

    // Ponteiro nulo em GuestMemory
    CHECK(dm.sceMsgDialogOpen(mem, 0) == SCE_MSG_DIALOG_ERROR_INVALID_ARG);

    // Endereço não mapeado
    CHECK(dm.sceMsgDialogOpen(mem, 0x9000) == SCE_MSG_DIALOG_ERROR_INVALID_ARG);

    // get_result com ponteiro nulo
    dm.sceMsgDialogInitialize();
    dm.sceMsgDialogOpen("Test");
    dm.sceMsgDialogUpdateStatus();
    CHECK(dm.sceMsgDialogGetResult(static_cast<SceMsgDialogResult*>(nullptr)) == SCE_MSG_DIALOG_ERROR_INVALID_ARG);
    CHECK(dm.sceMsgDialogGetResult(mem, 0) == SCE_MSG_DIALOG_ERROR_INVALID_ARG);
    CHECK(dm.sceMsgDialogGetResult(mem, 0x9000) == SCE_MSG_DIALOG_ERROR_INVALID_ARG);

    dm.sceMsgDialogTerminate();
}

static void test_msg_dialog_excessive_string() {
    using namespace bb;
    DialogManager dm;
    dm.sceMsgDialogInitialize();

    // String excedendo limite de 256 caracteres
    std::string long_msg(300, 'X');
    CHECK(dm.sceMsgDialogOpen(long_msg) == SCE_MSG_DIALOG_ERROR_INVALID_ARG);

    dm.sceMsgDialogTerminate();
}

static void test_ime_dialog_headless_lifecycle() {
    using namespace bb;
    DialogManager dm;

    // Inicialização IME com texto inicial
    CHECK(dm.sceImeDialogInit("Enter Hunter Name", "Hunter", 32, SceImeTextType::UTF8) == SCE_OK);
    CHECK(dm.sceImeDialogGetStatus() == SCE_IME_DIALOG_STATUS_FINISHED); // auto_finish imediato

    // Obter resultado
    SceImeDialogResult result{};
    CHECK(dm.sceImeDialogGetResult(&result) == SCE_OK);
    CHECK(result.end_status == SCE_IME_DIALOG_END_STATUS_OK);
    CHECK(result.text_length == 6);
    CHECK(std::strcmp(result.text, "Hunter") == 0);

    // Encerramento
    CHECK(dm.sceImeDialogTerm() == SCE_OK);
    CHECK(dm.sceImeDialogGetStatus() == SCE_IME_DIALOG_STATUS_NONE);
}

static void test_ime_dialog_status_polling() {
    using namespace bb;
    DialogManager dm;

    // Desabilitar auto-finish imediato para testar polling de status RUNNING
    dm.set_ime_auto_finish(true, 2); // finaliza após 2 consultas

    CHECK(dm.sceImeDialogInit("Title", "Initial", 32, SceImeTextType::UTF8) == SCE_OK);

    // Primeira consulta: RUNNING (query_count = 1 <= delay_queries)
    CHECK(dm.sceImeDialogGetStatus() == SCE_IME_DIALOG_STATUS_RUNNING);

    // Segunda consulta: RUNNING (query_count = 2 == delay_queries)
    CHECK(dm.sceImeDialogGetStatus() == SCE_IME_DIALOG_STATUS_RUNNING);

    // Terceira consulta: FINISHED (query_count = 3 > delay_queries)
    CHECK(dm.sceImeDialogGetStatus() == SCE_IME_DIALOG_STATUS_FINISHED);

    SceImeDialogResult res{};
    CHECK(dm.sceImeDialogGetResult(&res) == SCE_OK);
    CHECK(res.end_status == SCE_IME_DIALOG_END_STATUS_OK);
    CHECK(std::strcmp(res.text, "Initial") == 0);

    dm.sceImeDialogTerm();
}

static void test_ime_dialog_guest_memory() {
    using namespace bb;
    DialogManager dm;
    GuestMemory mem(16384);
    CHECK(mem.map(0x1000, 16384, read | write) == MemoryError::ok);

    // Preparar SceImeDialogParam na memória guest
    SceImeDialogParam param{};
    param.max_text_length = 32;
    param.text_type = SCE_IME_TEXT_TYPE_UTF8;
    const char title[] = "Guest IME";
    const char initial[] = "Test";
    CHECK(mem.store(0x1100, title, sizeof(title)) == MemoryError::ok);
    CHECK(mem.store(0x1200, initial, sizeof(initial)) == MemoryError::ok);
    param.title_addr = 0x1100;
    param.initial_text_addr = 0x1200;
    param.input_text_buffer = 0x1300;

    CHECK(mem.store(0x1000, &param, sizeof(param)) == MemoryError::ok);
    CHECK(dm.sceImeDialogInit(mem, 0x1000) == SCE_OK);

    // Status deve ir para FINISHED em headless imediato
    CHECK(dm.sceImeDialogGetStatus() == SCE_IME_DIALOG_STATUS_FINISHED);

    // Ler resultado na memória guest
    CHECK(dm.sceImeDialogGetResult(mem, 0x1400) == SCE_OK);
    SceImeDialogResult res{};
    CHECK(mem.load(0x1400, &res, sizeof(res)) == MemoryError::ok);
    CHECK(res.end_status == SCE_IME_DIALOG_END_STATUS_OK);
    CHECK(res.text_length == 4);
    CHECK(std::strcmp(res.text, "Test") == 0);

    // Verificar gravação no input_text_buffer
    char buffer[64]{};
    CHECK(mem.load(0x1300, buffer, sizeof(buffer)) == MemoryError::ok);
    CHECK(std::strcmp(buffer, "Test") == 0);

    dm.sceImeDialogTerm();
}

static void test_ime_dialog_utf16() {
    using namespace bb;
    DialogManager dm;
    GuestMemory mem(16384);
    CHECK(mem.map(0x1000, 16384, read | write) == MemoryError::ok);

    SceImeDialogParam param{};
    param.max_text_length = 32;
    param.text_type = SCE_IME_TEXT_TYPE_UTF16;

    std::u16string u16_title = u"Título";
    std::u16string u16_initial = u"Inicial";
    CHECK(mem.store(0x1100, u16_title.data(), (u16_title.size() + 1) * sizeof(char16_t)) == MemoryError::ok);
    CHECK(mem.store(0x1200, u16_initial.data(), (u16_initial.size() + 1) * sizeof(char16_t)) == MemoryError::ok);
    param.title_addr = 0x1100;
    param.initial_text_addr = 0x1200;
    param.input_text_buffer = 0x1300;

    CHECK(mem.store(0x1000, &param, sizeof(param)) == MemoryError::ok);
    CHECK(dm.sceImeDialogInit(mem, 0x1000) == SCE_OK);

    CHECK(dm.sceImeDialogGetStatus() == SCE_IME_DIALOG_STATUS_FINISHED);

    CHECK(dm.sceImeDialogGetResult(mem, 0x1400) == SCE_OK);

    // Verificar que input_text_buffer recebeu UTF-16
    char16_t u16_buffer[64]{};
    CHECK(mem.load(0x1300, u16_buffer, sizeof(u16_buffer)) == MemoryError::ok);
    std::u16string result_u16(u16_buffer);
    CHECK(result_u16 == u"Inicial");

    dm.sceImeDialogTerm();
}

static void test_ime_dialog_invalid_pointers() {
    using namespace bb;
    DialogManager dm;
    GuestMemory mem(1024);
    CHECK(mem.map(0x1000, 512, read | write) == MemoryError::ok);

    // Ponteiro nulo
    CHECK(dm.sceImeDialogInit(static_cast<const SceImeDialogParam*>(nullptr)) == SCE_IME_DIALOG_ERROR_INVALID_ARG);
    CHECK(dm.sceImeDialogInit(mem, 0) == SCE_IME_DIALOG_ERROR_INVALID_ARG);
    CHECK(dm.sceImeDialogInit(mem, 0x9000) == SCE_IME_DIALOG_ERROR_INVALID_ARG);

    dm.sceImeDialogInit("Title", "Initial", 32);
    dm.sceImeDialogGetStatus(); // para ir a FINISHED

    // get_result com ponteiro nulo
    CHECK(dm.sceImeDialogGetResult(static_cast<SceImeDialogResult*>(nullptr)) == SCE_IME_DIALOG_ERROR_INVALID_ARG);
    CHECK(dm.sceImeDialogGetResult(mem, 0) == SCE_IME_DIALOG_ERROR_INVALID_ARG);
    CHECK(dm.sceImeDialogGetResult(mem, 0x9000) == SCE_IME_DIALOG_ERROR_INVALID_ARG);

    dm.sceImeDialogTerm();
}

static void test_ime_dialog_excessive_text() {
    using namespace bb;
    DialogManager dm;

    // max_text_length excedendo limite
    CHECK(dm.sceImeDialogInit("Title", "Initial", 300) == SCE_IME_DIALOG_ERROR_TEXT_TOO_LONG);

    // Título ou texto inicial muito longo
    std::string long_str(300, 'X');
    CHECK(dm.sceImeDialogInit(long_str, "Initial", 32) == SCE_IME_DIALOG_ERROR_TEXT_TOO_LONG);
    CHECK(dm.sceImeDialogInit("Title", long_str, 32) == SCE_IME_DIALOG_ERROR_TEXT_TOO_LONG);

    // Texto submetido excedendo limite em modo interativo
    dm.set_ime_auto_finish(false);
    dm.sceImeDialogInit("Title", "", 10);
    std::string long_text(20, 'Y');
    CHECK(dm.submit_ime_result(long_text, SCE_IME_DIALOG_END_STATUS_OK));
    SceImeDialogResult res{};
    dm.sceImeDialogGetResult(&res);
    CHECK(res.text_length <= 10);
    dm.sceImeDialogTerm();
}

static void test_ime_dialog_abort() {
    using namespace bb;
    DialogManager dm;
    dm.set_ime_auto_finish(false);

    CHECK(dm.sceImeDialogInit("Title", "Initial", 32) == SCE_OK);
    CHECK(dm.sceImeDialogGetStatus() == SCE_IME_DIALOG_STATUS_RUNNING);

    // Abortar
    CHECK(dm.sceImeDialogAbort() == SCE_OK);
    CHECK(dm.sceImeDialogGetStatus() == SCE_IME_DIALOG_STATUS_ABORTED);

    // get_result deve retornar status de abortado
    SceImeDialogResult res{};
    CHECK(dm.sceImeDialogGetResult(&res) == SCE_OK);
    CHECK(res.end_status == SCE_IME_DIALOG_END_STATUS_ABORTED);

    dm.sceImeDialogTerm();
}

static void test_readonly_memory_protection() {
    using namespace bb;
    DialogManager dm;
    GuestMemory mem(1024);
    CHECK(mem.map(0x1000, 512, read) == MemoryError::ok); // Apenas leitura

    dm.sceMsgDialogInitialize();
    dm.sceMsgDialogOpen("Test");
    dm.sceMsgDialogUpdateStatus();

    // Tentar gravar resultado em memória somente leitura
    CHECK(dm.sceMsgDialogGetResult(mem, 0x1000) == SCE_MSG_DIALOG_ERROR_INVALID_ARG);

    dm.sceMsgDialogTerminate();

    // Teste IME
    dm.set_ime_auto_finish(true);
    CHECK(dm.sceImeDialogInit("Title", "Initial", 32) == SCE_OK);
    dm.sceImeDialogGetStatus();

    CHECK(dm.sceImeDialogGetResult(mem, 0x1000) == SCE_IME_DIALOG_ERROR_INVALID_ARG);

    dm.sceImeDialogTerm();
}

static void test_concurrency_thread_safety() {
    using namespace bb;
    DialogManager dm;

    // Inicializar ambos os subsistemas
    dm.sceCommonDialogInitialize();
    dm.sceMsgDialogInitialize();
    dm.set_ime_auto_finish(true);

    const int iterations = 100;
    std::atomic<int> success_count{0};
    std::atomic<int> error_count{0};

    auto msg_worker = [&]() {
        for (int i = 0; i < iterations; ++i) {
            if (dm.sceMsgDialogOpen("Concurrent test", SceMsgDialogButtonType::OK) == SCE_OK) {
                dm.sceMsgDialogUpdateStatus();
                SceMsgDialogResult res{};
                if (dm.sceMsgDialogGetResult(&res) == SCE_OK) {
                    success_count++;
                } else {
                    error_count++;
                }
                dm.sceMsgDialogTerminate();
                dm.sceMsgDialogInitialize(); // Re-inicializar para próxima iteração
            } else {
                error_count++;
            }
        }
    };

    auto ime_worker = [&]() {
        for (int i = 0; i < iterations; ++i) {
            if (dm.sceImeDialogInit("Title", "Text", 32) == SCE_OK) {
                dm.sceImeDialogGetStatus();
                SceImeDialogResult res{};
                if (dm.sceImeDialogGetResult(&res) == SCE_OK) {
                    success_count++;
                } else {
                    error_count++;
                }
                dm.sceImeDialogTerm();
            } else {
                error_count++;
            }
        }
    };

    std::thread t1(msg_worker);
    std::thread t2(ime_worker);
    t1.join();
    t2.join();

    // Verificar que não houve corrupção ou deadlock
    CHECK(success_count.load() > 0);
}

static void test_c_style_wrappers() {
    using namespace bb;

    // Teste CommonDialog via wrappers globais
    CHECK(sceCommonDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_NONE);
    CHECK(sceCommonDialogInitialize() == SCE_OK);
    CHECK(sceCommonDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_INITIALIZED);

    // Teste MsgDialog via wrappers globais
    CHECK(sceMsgDialogInitialize() == SCE_OK);
    CHECK(sceMsgDialogGetStatus() == SCE_MSG_DIALOG_STATUS_INITIALIZED);

    CHECK(sceMsgDialogOpen("Wrapper test", SceMsgDialogButtonType::OK) == SCE_OK);
    CHECK(sceMsgDialogGetStatus() == SCE_MSG_DIALOG_STATUS_RUNNING);
    CHECK(sceMsgDialogUpdateStatus() == SCE_MSG_DIALOG_STATUS_FINISHED);

    SceMsgDialogResult res{};
    CHECK(sceMsgDialogGetResult(&res) == SCE_OK);
    CHECK(res.button_id == SCE_MSG_DIALOG_BUTTON_ID_OK);

    CHECK(sceMsgDialogTerminate() == SCE_OK);
    CHECK(sceMsgDialogGetStatus() == SCE_MSG_DIALOG_STATUS_NONE);

    // Teste ImeDialog via wrappers globais
    CHECK(sceImeDialogInit("Wrapper IME", "Default", 32, SceImeTextType::UTF8) == SCE_OK);
    CHECK(sceImeDialogGetStatus() == SCE_IME_DIALOG_STATUS_FINISHED);

    SceImeDialogResult ires{};
    CHECK(sceImeDialogGetResult(&ires) == SCE_OK);
    CHECK(ires.text_length == 7);
    CHECK(std::strcmp(ires.text, "Default") == 0);

    CHECK(sceImeDialogTerm() == SCE_OK);
    CHECK(sceImeDialogGetStatus() == SCE_IME_DIALOG_STATUS_NONE);
}

static void test_headless_mode_toggle() {
    using namespace bb;
    DialogManager dm;

    dm.set_headless_mode(false);
    CHECK(!dm.is_headless_mode());

    dm.sceMsgDialogInitialize();
    CHECK(dm.sceMsgDialogOpen("Test", SceMsgDialogButtonType::OK) == SCE_OK);
    // Em modo não-headless, status deve permanecer RUNNING até submit
    CHECK(dm.sceMsgDialogGetStatus() == SCE_MSG_DIALOG_STATUS_RUNNING);
    CHECK(dm.sceMsgDialogUpdateStatus() == SCE_MSG_DIALOG_STATUS_RUNNING);

    CHECK(dm.submit_msg_result(SceMsgDialogButtonId::CANCEL));
    CHECK(dm.sceMsgDialogGetStatus() == SCE_MSG_DIALOG_STATUS_FINISHED);

    SceMsgDialogResult res{};
    CHECK(dm.sceMsgDialogGetResult(&res) == SCE_OK);
    CHECK(res.button_id == SCE_MSG_DIALOG_BUTTON_ID_CANCEL);

    dm.sceMsgDialogTerminate();

    // Teste modo headless novamente
    dm.set_headless_mode(true);
    CHECK(dm.is_headless_mode());
    dm.sceMsgDialogInitialize();
    CHECK(dm.sceMsgDialogOpen("Test2", SceMsgDialogButtonType::OK) == SCE_OK);
    dm.sceMsgDialogUpdateStatus();
    CHECK(dm.sceMsgDialogGetStatus() == SCE_MSG_DIALOG_STATUS_FINISHED);
    dm.sceMsgDialogTerminate();
}

static void test_callbacks() {
    using namespace bb;
    DialogManager dm;
    dm.set_headless_mode(false);

    bool msg_called = false;
    bool ime_called = false;

    dm.set_msg_dialog_callback([&](const MsgDialogRequest& req) {
        msg_called = true;
        CHECK(req.message == "Callback test");
        CHECK(req.button_type == SceMsgDialogButtonType::YESNO);
    });

    dm.set_ime_dialog_callback([&](const ImeDialogRequest& req) {
        ime_called = true;
        CHECK(req.title == "IME Callback");
        CHECK(req.initial_text == "Default");
    });

    dm.sceMsgDialogInitialize();
    CHECK(dm.sceMsgDialogOpen("Callback test", SceMsgDialogButtonType::YESNO) == SCE_OK);
    CHECK(msg_called);

    dm.set_ime_auto_finish(false);
    CHECK(dm.sceImeDialogInit("IME Callback", "Default", 32) == SCE_OK);
    CHECK(ime_called);

    dm.sceMsgDialogTerminate();
    dm.sceImeDialogTerm();
}

int main() {
    int failures = 0;

    failures += run(test_common_dialog_lifecycle);
    failures += run(test_msg_dialog_headless_lifecycle);
    failures += run(test_msg_dialog_custom_buttons);
    failures += run(test_msg_dialog_guest_memory);
    failures += run(test_msg_dialog_invalid_pointers);
    failures += run(test_msg_dialog_excessive_string);
    failures += run(test_ime_dialog_headless_lifecycle);
    failures += run(test_ime_dialog_status_polling);
    failures += run(test_ime_dialog_guest_memory);
    failures += run(test_ime_dialog_utf16);
    failures += run(test_ime_dialog_invalid_pointers);
    failures += run(test_ime_dialog_excessive_text);
    failures += run(test_ime_dialog_abort);
    failures += run(test_readonly_memory_protection);
    failures += run(test_concurrency_thread_safety);
    failures += run(test_c_style_wrappers);
    failures += run(test_headless_mode_toggle);
    failures += run(test_callbacks);

    return failures;
}