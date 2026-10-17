#pragma once

#include "guest_memory.h"
#include "atrac9_decoder.h"
#include <cstdint>
#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <string>

namespace bb {

// ============================================================================
// Tipos e Constantes do Subsistema SCE AJM (Audio Judgement Module)
// ============================================================================

constexpr std::int32_t SCE_AJM_OK                         = 0;
constexpr std::int32_t SCE_AJM_ERROR_NOT_INITIALIZED      = static_cast<std::int32_t>(0x805B0001);
constexpr std::int32_t SCE_AJM_ERROR_INVALID_CONTEXT      = static_cast<std::int32_t>(0x805B0002);
constexpr std::int32_t SCE_AJM_ERROR_INVALID_INSTANCE     = static_cast<std::int32_t>(0x805B0003);
constexpr std::int32_t SCE_AJM_ERROR_INVALID_ARGUMENT     = static_cast<std::int32_t>(0x805B0004);
constexpr std::int32_t SCE_AJM_ERROR_UNSUPPORTED_CODEC    = static_cast<std::int32_t>(0x805B0005);
constexpr std::int32_t SCE_AJM_ERROR_OUT_OF_MEMORY        = static_cast<std::int32_t>(0x805B0006);
constexpr std::int32_t SCE_AJM_ERROR_JOB_FAILED           = static_cast<std::int32_t>(0x805B0007);

// Codecs suportados pelo AJM
constexpr std::uint32_t SCE_AJM_CODEC_NONE               = 0x0000;
constexpr std::uint32_t SCE_AJM_CODEC_ATRAC9             = 0x1000;
constexpr std::uint32_t SCE_AJM_CODEC_MP3                = 0x2000;
constexpr std::uint32_t SCE_AJM_CODEC_AAC                = 0x3000;

// Tipos de jobs AJM
constexpr std::uint32_t SCE_AJM_JOB_TYPE_DECODE          = 1;
constexpr std::uint32_t SCE_AJM_JOB_TYPE_RESAMPLE        = 2;
constexpr std::uint32_t SCE_AJM_JOB_TYPE_MIX             = 3;

#pragma pack(push, 1)

// Parâmetros de criação de instância AJM na memória guest / host
struct SceAjmInstanceParam {
    std::uint32_t codec{SCE_AJM_CODEC_ATRAC9};
    std::uint32_t channels{2};
    std::uint32_t sample_rate{48000};
    std::uint8_t  config_data[4]{0};
    std::uint8_t  reserved[16]{0};
};

// Descritor de job de lote AJM
struct SceAjmJobDesc {
    std::uint32_t job_type{SCE_AJM_JOB_TYPE_DECODE};
    std::int32_t  instance_id{0};
    std::uint64_t in_buffer{0};
    std::uint32_t in_size{0};
    std::uint64_t out_buffer{0};
    std::uint32_t out_size{0};
    std::uint32_t result{0};
    std::uint32_t bytes_written{0};
};

#pragma pack(pop)

// Diagnósticos observáveis do AJM
struct AjmDiagnostics {
    std::uint64_t total_batches_started{0};
    std::uint64_t total_jobs_processed{0};
    std::uint64_t total_bytes_decoded{0};
    std::uint32_t active_instances_count{0};
    std::uint32_t last_codec{0};
    std::int32_t  last_error{0};
};

class AjmManager {
public:
    AjmManager();
    ~AjmManager() = default;

    std::int32_t initialize(std::int64_t priority, std::int32_t* out_ctx_id);
    std::int32_t finalize(std::int32_t ctx_id);

    std::int32_t module_register(std::int32_t ctx_id, std::uint32_t codec);
    std::int32_t module_unregister(std::int32_t ctx_id, std::uint32_t codec);

    std::int32_t instance_create(std::int32_t ctx_id, std::uint32_t codec,
                                const SceAjmInstanceParam* param, std::int32_t* out_inst_id);
    std::int32_t instance_create(std::int32_t ctx_id, std::uint32_t codec,
                                GuestMemory& mem, std::uint64_t param_addr, std::int32_t* out_inst_id);
    std::int32_t instance_destroy(std::int32_t ctx_id, std::int32_t inst_id);

    std::int32_t batch_start_buffer(std::int32_t ctx_id, std::uint64_t* out_batch_id);
    std::int32_t batch_job_run(std::int32_t ctx_id, std::uint64_t batch_id,
                              GuestMemory& mem, const SceAjmJobDesc& desc,
                              std::uint32_t* out_bytes_written = nullptr);
    std::int32_t batch_job_run_buffer(std::int32_t ctx_id, std::uint64_t batch_id,
                                     GuestMemory& mem, std::uint64_t in_buf, std::uint32_t in_size,
                                     std::uint64_t out_buf, std::uint32_t out_size,
                                     std::int32_t inst_id, std::uint32_t* out_bytes_written = nullptr);
    std::int32_t batch_wait(std::int32_t ctx_id, std::uint64_t batch_id, std::uint32_t timeout_us);
    std::int32_t batch_cancel(std::int32_t ctx_id, std::uint64_t batch_id);

    AjmDiagnostics diagnostics() const;
    void reset();

private:
    struct Instance {
        std::int32_t id{0};
        std::uint32_t codec{0};
        std::uint32_t channels{2};
        std::uint32_t sample_rate{48000};
        std::unique_ptr<Atrac9Decoder> atrac9;
    };

    struct Context {
        std::int32_t id{0};
        std::int64_t priority{0};
        std::vector<std::uint32_t> registered_codecs;
        std::unordered_map<std::int32_t, std::unique_ptr<Instance>> instances;
        std::unordered_map<std::uint64_t, std::vector<SceAjmJobDesc>> batches;
        std::int32_t next_instance_id{1};
        std::uint64_t next_batch_id{1};
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::int32_t, std::unique_ptr<Context>> contexts_;
    std::int32_t next_context_id_{1};
    AjmDiagnostics diag_{};
};

AjmManager& default_ajm_manager();

} // namespace bb
