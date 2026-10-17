#include "check.h"
#include "ajm_hle.h"
#include "guest_memory.h"
#include "orbis_runtime.h"
#include "atrac9_decoder.h"
#include <vector>
#include <cstring>

static void test_ajm_lifecycle() {
    using namespace bb;
    AjmManager ajm;

    std::int32_t ctx_id = 0;
    CHECK(ajm.initialize(100, &ctx_id) == SCE_AJM_OK);
    CHECK(ctx_id > 0);

    // Registro de módulo válido
    CHECK(ajm.module_register(ctx_id, SCE_AJM_CODEC_ATRAC9) == SCE_AJM_OK);
    CHECK(ajm.module_register(ctx_id, SCE_AJM_CODEC_MP3) == SCE_AJM_OK);
    CHECK(ajm.module_register(ctx_id, SCE_AJM_CODEC_AAC) == SCE_AJM_OK);

    // Rejeição de codec desconhecido
    CHECK(ajm.module_register(ctx_id, 0x9999) == SCE_AJM_ERROR_UNSUPPORTED_CODEC);

    // Desregistro de módulo
    CHECK(ajm.module_unregister(ctx_id, SCE_AJM_CODEC_MP3) == SCE_AJM_OK);

    // Contexto inválido
    CHECK(ajm.module_register(9999, SCE_AJM_CODEC_ATRAC9) == SCE_AJM_ERROR_INVALID_CONTEXT);
    CHECK(ajm.module_unregister(9999, SCE_AJM_CODEC_ATRAC9) == SCE_AJM_ERROR_INVALID_CONTEXT);

    // Finalização
    CHECK(ajm.finalize(ctx_id) == SCE_AJM_OK);
    CHECK(ajm.finalize(ctx_id) == SCE_AJM_ERROR_INVALID_CONTEXT);
}

static void test_ajm_instances() {
    using namespace bb;
    AjmManager ajm;
    std::int32_t ctx_id = 0;
    CHECK(ajm.initialize(100, &ctx_id) == SCE_AJM_OK);

    // Criação de instância ATRAC9 válida
    SceAjmInstanceParam param{};
    param.codec = SCE_AJM_CODEC_ATRAC9;
    param.channels = 2;
    param.sample_rate = 48000;
    auto cfg = Atrac9Decoder::make_config_data(7, 2, 192, 0);
    std::memcpy(param.config_data, cfg.data(), 4);

    std::int32_t inst_id = 0;
    CHECK(ajm.instance_create(ctx_id, SCE_AJM_CODEC_ATRAC9, &param, &inst_id) == SCE_AJM_OK);
    CHECK(inst_id > 0);

    // Codec não suportado deve retornar erro coerente
    std::int32_t bad_inst = 0;
    CHECK(ajm.instance_create(ctx_id, 0x7777, &param, &bad_inst) == SCE_AJM_ERROR_UNSUPPORTED_CODEC);

    // Destruição de instância
    CHECK(ajm.instance_destroy(ctx_id, inst_id) == SCE_AJM_OK);
    CHECK(ajm.instance_destroy(ctx_id, inst_id) == SCE_AJM_ERROR_INVALID_INSTANCE);
    CHECK(ajm.instance_destroy(9999, inst_id) == SCE_AJM_ERROR_INVALID_CONTEXT);

    ajm.finalize(ctx_id);
}

static void test_ajm_batch_decoding() {
    using namespace bb;
    AjmManager ajm;
    std::int32_t ctx_id = 0;
    CHECK(ajm.initialize(100, &ctx_id) == SCE_AJM_OK);

    SceAjmInstanceParam param{};
    param.codec = SCE_AJM_CODEC_ATRAC9;
    param.channels = 2;
    param.sample_rate = 48000;
    auto cfg = Atrac9Decoder::make_config_data(7, 2, 192, 0);
    std::memcpy(param.config_data, cfg.data(), 4);

    std::int32_t inst_id = 0;
    CHECK(ajm.instance_create(ctx_id, SCE_AJM_CODEC_ATRAC9, &param, &inst_id) == SCE_AJM_OK);

    std::uint64_t batch_id = 0;
    CHECK(ajm.batch_start_buffer(ctx_id, &batch_id) == SCE_AJM_OK);
    CHECK(batch_id > 0);

    // Prepara GuestMemory com buffers de entrada e saída
    GuestMemory mem(1 << 20);
    uint64_t in_addr = 0x10000;
    uint64_t out_addr = 0x20000;
    CHECK(mem.map(in_addr, 0x8000, read | write) == MemoryError::ok);
    CHECK(mem.map(out_addr, 0x8000, read | write) == MemoryError::ok);

    // Preenche buffer de entrada com dados dummy / silenciosos simulando quadro ATRAC9 (192 bytes por superframe)
    std::vector<uint8_t> dummy_at9(192 * 4, 0);
    CHECK(mem.store(in_addr, dummy_at9.data(), dummy_at9.size()) == MemoryError::ok);

    std::uint32_t written = 0;
    auto res = ajm.batch_job_run_buffer(ctx_id, batch_id, mem, in_addr, static_cast<uint32_t>(dummy_at9.size()),
                                       out_addr, 0x8000, inst_id, &written);
    CHECK(res == SCE_AJM_OK);

    auto diag = ajm.diagnostics();
    CHECK(diag.total_batches_started >= 1);
    CHECK(diag.total_jobs_processed >= 1);

    CHECK(ajm.batch_wait(ctx_id, batch_id, 1000) == SCE_AJM_OK);
    CHECK(ajm.batch_cancel(ctx_id, batch_id) == SCE_AJM_OK);

    ajm.finalize(ctx_id);
}

static void test_ajm_orbis_runtime_integration() {
    using namespace bb;
    GuestMemory mem(1 << 20);
    OrbisRuntime rt(mem);

    default_ajm_manager().reset();

    // 1. sceAjmInitialize
    auto ctx_addr = 0x10000ULL;
    mem.map(ctx_addr, 0x1000, read | write);

    CpuState cpu{};
    cpu.gpr[7] = 0; // priority
    cpu.gpr[6] = ctx_addr;
    CHECK(rt.invoke("sceAjmInitialize", cpu) == 0);

    std::int32_t ctx_id = 0;
    CHECK(mem.load(ctx_addr, &ctx_id, 4) == MemoryError::ok);
    CHECK(ctx_id > 0);

    // 2. sceAjmModuleRegister
    cpu.gpr[7] = ctx_id;
    cpu.gpr[6] = SCE_AJM_CODEC_ATRAC9;
    CHECK(rt.invoke("sceAjmModuleRegister", cpu) == 0);

    // 3. sceAjmInstanceCreate
    auto inst_out_addr = 0x11000ULL;
    mem.map(inst_out_addr, 0x1000, read | write);
    cpu.gpr[7] = ctx_id;
    cpu.gpr[6] = SCE_AJM_CODEC_ATRAC9;
    cpu.gpr[2] = 0; // param nulo = fallback padrão
    cpu.gpr[1] = inst_out_addr;
    CHECK(rt.invoke("sceAjmInstanceCreate", cpu) == 0);

    std::int32_t inst_id = 0;
    CHECK(mem.load(inst_out_addr, &inst_id, 4) == MemoryError::ok);
    CHECK(inst_id > 0);

    // 4. sceAjmBatchStartBuffer
    auto batch_out_addr = 0x12000ULL;
    mem.map(batch_out_addr, 0x1000, read | write);
    cpu.gpr[7] = ctx_id;
    cpu.gpr[6] = 0;
    cpu.gpr[2] = 0;
    cpu.gpr[1] = batch_out_addr;
    CHECK(rt.invoke("sceAjmBatchStartBuffer", cpu) == 0);

    std::uint64_t batch_id = 0;
    CHECK(mem.load(batch_out_addr, &batch_id, 8) == MemoryError::ok);
    CHECK(batch_id > 0);

    // 5. sceAjmBatchWait
    cpu.gpr[7] = ctx_id;
    cpu.gpr[6] = batch_id;
    cpu.gpr[2] = 1000;
    CHECK(rt.invoke("sceAjmBatchWait", cpu) == 0);

    // 6. sceAjmInstanceDestroy
    cpu.gpr[7] = ctx_id;
    cpu.gpr[6] = inst_id;
    CHECK(rt.invoke("sceAjmInstanceDestroy", cpu) == 0);

    // 7. sceAjmFinalize
    cpu.gpr[7] = ctx_id;
    CHECK(rt.invoke("sceAjmFinalize", cpu) == 0);
}

int main() {
    int failures = 0;
    failures += run(test_ajm_lifecycle);
    failures += run(test_ajm_instances);
    failures += run(test_ajm_batch_decoding);
    failures += run(test_ajm_orbis_runtime_integration);
    return failures;
}
