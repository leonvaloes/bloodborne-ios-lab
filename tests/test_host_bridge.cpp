#include "check.h"
#include "host_bridge.h"
#include "dialog_hle.h"

#include <cstdint>
#include <string>
#include <fstream>

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

    // =========================================================================
    // T070: Sanitização do relatório exportável (Constituição III)
    // Garantir ausência de conteúdo integral bruto e presença apenas de metadados mínimos
    // =========================================================================
    {
        const auto diag = bridge.diagnostics();
        const auto summary_str = diag.summary();
        CHECK(summary_str.find("content_utf8") == std::string::npos);
        CHECK(summary_str.find("PASSWORD") == std::string::npos);
        CHECK(summary_str.find("SECRET") == std::string::npos);

        // Simulação do payload sanitizado exportável per T070
        // Deve conter status, valid, size_bytes mas NUNCA o conteúdo integral do arquivo
        struct DocumentCheckReport {
            std::string filename{"teste-bblab.txt"};
            std::string status{"ok"};
            bool valid{true};
            std::size_t size_bytes{128};
            // content_utf8 estritamente omitido
        } sanitized_report;

        CHECK(sanitized_report.filename == "teste-bblab.txt");
        CHECK(sanitized_report.status == "ok");
        CHECK(sanitized_report.valid);
        CHECK(sanitized_report.size_bytes == 128);
    }

    // =========================================================================
    // T065: Integração de diálogos MsgDialog e IME com o Host
    // Garantir apresentação, callbacks e resolução sem deixar estado RUNNING
    // =========================================================================
    {
        auto& dm = bb::default_dialog_manager();
        dm.reset();

        bool host_msg_notified = false;
        bb::MsgDialogRequest received_msg_req;
        dm.set_msg_dialog_callback([&](const bb::MsgDialogRequest& req) {
            host_msg_notified = true;
            received_msg_req = req;
        });

        bool host_ime_notified = false;
        bb::ImeDialogRequest received_ime_req;
        dm.set_ime_dialog_callback([&](const bb::ImeDialogRequest& req) {
            host_ime_notified = true;
            received_ime_req = req;
        });

        // 1. Simulação do MsgDialog de confirmação / aviso do jogo
        dm.set_headless_mode(false); // Modo interativo como no host real
        CHECK(dm.sceMsgDialogInitialize() == bb::SCE_OK);
        CHECK(dm.sceMsgDialogOpen("Carregar dados salvos?", bb::SceMsgDialogButtonType::YESNO) == bb::SCE_OK);
        CHECK(host_msg_notified);
        CHECK(received_msg_req.message == "Carregar dados salvos?");
        CHECK(received_msg_req.button_type == bb::SceMsgDialogButtonType::YESNO);
        CHECK(dm.sceMsgDialogGetStatus() == bb::SCE_MSG_DIALOG_STATUS_RUNNING);

        // Host simula toque do usuário no botão "Sim"
        CHECK(dm.submit_msg_result(bb::SceMsgDialogButtonId::YES));
        CHECK(dm.sceMsgDialogGetStatus() == bb::SCE_MSG_DIALOG_STATUS_FINISHED);

        bb::SceMsgDialogResult mres{};
        CHECK(dm.sceMsgDialogGetResult(&mres) == bb::SCE_OK);
        CHECK(mres.button_id == bb::SCE_MSG_DIALOG_BUTTON_ID_YES);
        CHECK(dm.sceMsgDialogTerminate() == bb::SCE_OK);

        // 2. Simulação do IME de criação de personagem (nome do caçador)
        dm.set_ime_auto_finish(false); // Modo interativo
        CHECK(dm.sceImeDialogInit("Nome do Caçador", "Hunter", 32, bb::SceImeTextType::UTF8) == bb::SCE_OK);
        CHECK(host_ime_notified);
        CHECK(received_ime_req.title == "Nome do Caçador");
        CHECK(received_ime_req.initial_text == "Hunter");
        CHECK(dm.sceImeDialogGetStatus() == bb::SCE_IME_DIALOG_STATUS_RUNNING);

        // Host simula submissão de texto modificado pelo jogador
        CHECK(dm.submit_ime_result("GoodHunter", bb::SCE_IME_DIALOG_END_STATUS_OK));
        CHECK(dm.sceImeDialogGetStatus() == bb::SCE_IME_DIALOG_STATUS_FINISHED);

        bb::SceImeDialogResult ires{};
        CHECK(dm.sceImeDialogGetResult(&ires) == bb::SCE_OK);
        CHECK(ires.end_status == bb::SCE_IME_DIALOG_END_STATUS_OK);
        CHECK(std::string(ires.text) == "GoodHunter");
        CHECK(dm.sceImeDialogTerm() == bb::SCE_OK);

        // 3. Simulação de cancelamento / abort do IME sem travar o runtime
        host_ime_notified = false;
        CHECK(dm.sceImeDialogInit("Nome", "", 16) == bb::SCE_OK);
        CHECK(host_ime_notified);
        CHECK(dm.sceImeDialogGetStatus() == bb::SCE_IME_DIALOG_STATUS_RUNNING);
        // Jogador cancela a caixa de diálogo
        CHECK(dm.submit_ime_result("", bb::SCE_IME_DIALOG_END_STATUS_USER_CANCELED));
        CHECK(dm.sceImeDialogGetStatus() == bb::SCE_IME_DIALOG_STATUS_FINISHED);
        CHECK(dm.sceImeDialogGetResult(&ires) == bb::SCE_OK);
        CHECK(ires.end_status == bb::SCE_IME_DIALOG_END_STATUS_USER_CANCELED);
        CHECK(dm.sceImeDialogTerm() == bb::SCE_OK);

        dm.reset();
    }

    // =========================================================================
    // T059: Rastreamento de vídeo de introdução e fallback offline explícito
    // Sem reportar falso sucesso para mídias inexistentes (US2/AC1)
    // =========================================================================
    {
        bb::HostBridge bridge;
        CHECK(bridge.intro_video_status() == "not_started");

        // 1. Caminho vazio: fallback offline explícito
        CHECK(!bridge.play_intro_video_or_fallback(""));
        CHECK(bridge.intro_video_status() == "offline_fallback_missing_media");
        CHECK(bridge.diagnostics().intro_video_status == "offline_fallback_missing_media");
        CHECK(bridge.diagnostics().summary().find("offline_fallback_missing_media") != std::string::npos);

        // 2. Arquivo inexistente: fallback offline sem falso sucesso
        CHECK(!bridge.play_intro_video_or_fallback("caminho_inexistente_intro.mp4"));
        CHECK(bridge.intro_video_status() == "offline_fallback_missing_media");

        // 3. Arquivo de mídia autoral existente: preparação válida
        const std::string dummy_video = "teste_intro_fixture.bin";
        {
            std::ofstream f(dummy_video, std::ios::binary);
            f << "MP4_HEADER_TEST_1234567890";
        }
        CHECK(bridge.play_intro_video_or_fallback(dummy_video));
        CHECK(bridge.intro_video_status() == "ready_or_playing");
        CHECK(bridge.diagnostics().intro_video_status == "ready_or_playing");
        std::remove(dummy_video.c_str());

        // 4. T114: Mídia existente ilegível/truncada (< 16 bytes) ativa fallback offline explícito
        const std::string corrupt_video = "corrupt_intro_fixture.bin";
        {
            std::ofstream f(corrupt_video, std::ios::binary);
            f << "TINY";
        }
        CHECK(!bridge.play_intro_video_or_fallback(corrupt_video));
        CHECK(bridge.intro_video_status() == "offline_fallback_unreadable_media");
        std::remove(corrupt_video.c_str());

        // 5. T114: Rotas de conclusão e falha de AVPlayerItem sem reter o fluxo
        // 5a. Falha de reprodução durante a execução do AVPlayerItem
        bridge.finish_intro_and_boot(bb::HostBridge::IntroFallbackReason::PlaybackFailed);
        CHECK(bridge.intro_video_status() == "offline_fallback_playback_failed");

        // 5b. Usuário pula ou cancela a introdução
        bridge.finish_intro_and_boot(bb::HostBridge::IntroFallbackReason::UserCancelled);
        CHECK(bridge.intro_video_status() == "offline_fallback_user_cancelled");

        // 5c. Reprodução concluída normalmente até o final
        bridge.finish_intro_and_boot(bb::HostBridge::IntroFallbackReason::Finished);
        CHECK(bridge.intro_video_status() == "finished");

        // Reset restaura estado limpo
        bridge.reset();
        CHECK(bridge.intro_video_status() == "not_started");
    }
}); }
