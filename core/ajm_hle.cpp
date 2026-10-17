#include "ajm_hle.h"
#include <algorithm>
#include <cstring>

namespace bb {

AjmManager::AjmManager() = default;

std::int32_t AjmManager::initialize(std::int64_t priority, std::int32_t* out_ctx_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto ctx = std::make_unique<Context>();
    ctx->id = next_context_id_++;
    ctx->priority = priority;
    // Por padrão o módulo ATRAC9 fica disponível
    ctx->registered_codecs.push_back(SCE_AJM_CODEC_ATRAC9);

    std::int32_t id = ctx->id;
    contexts_[id] = std::move(ctx);
    if (out_ctx_id) *out_ctx_id = id;
    return SCE_AJM_OK;
}

std::int32_t AjmManager::finalize(std::int32_t ctx_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = contexts_.find(ctx_id);
    if (it == contexts_.end()) return SCE_AJM_ERROR_INVALID_CONTEXT;
    contexts_.erase(it);
    return SCE_AJM_OK;
}

std::int32_t AjmManager::module_register(std::int32_t ctx_id, std::uint32_t codec) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = contexts_.find(ctx_id);
    if (it == contexts_.end()) return SCE_AJM_ERROR_INVALID_CONTEXT;
    if (codec != SCE_AJM_CODEC_ATRAC9 && codec != SCE_AJM_CODEC_MP3 && codec != SCE_AJM_CODEC_AAC) {
        diag_.last_error = SCE_AJM_ERROR_UNSUPPORTED_CODEC;
        return SCE_AJM_ERROR_UNSUPPORTED_CODEC;
    }
    auto& codecs = it->second->registered_codecs;
    if (std::find(codecs.begin(), codecs.end(), codec) == codecs.end()) {
        codecs.push_back(codec);
    }
    return SCE_AJM_OK;
}

std::int32_t AjmManager::module_unregister(std::int32_t ctx_id, std::uint32_t codec) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = contexts_.find(ctx_id);
    if (it == contexts_.end()) return SCE_AJM_ERROR_INVALID_CONTEXT;
    auto& codecs = it->second->registered_codecs;
    codecs.erase(std::remove(codecs.begin(), codecs.end(), codec), codecs.end());
    return SCE_AJM_OK;
}

std::int32_t AjmManager::instance_create(std::int32_t ctx_id, std::uint32_t codec,
                                        const SceAjmInstanceParam* param, std::int32_t* out_inst_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = contexts_.find(ctx_id);
    if (it == contexts_.end()) return SCE_AJM_ERROR_INVALID_CONTEXT;

    if (codec != SCE_AJM_CODEC_ATRAC9) {
        diag_.last_error = SCE_AJM_ERROR_UNSUPPORTED_CODEC;
        return SCE_AJM_ERROR_UNSUPPORTED_CODEC;
    }

    auto inst = std::make_unique<Instance>();
    inst->id = it->second->next_instance_id++;
    inst->codec = codec;
    if (param) {
        inst->channels = param->channels > 0 ? param->channels : 2;
        inst->sample_rate = param->sample_rate > 0 ? param->sample_rate : 48000;
        inst->atrac9 = std::make_unique<Atrac9Decoder>();
        if (!inst->atrac9->init(param->config_data)) {
            // Inicialização padrão de fallback
            auto def_cfg = Atrac9Decoder::make_config_data(7, 2, 192, 0);
            inst->atrac9->init(def_cfg.data());
        }
    } else {
        inst->atrac9 = std::make_unique<Atrac9Decoder>();
        auto def_cfg = Atrac9Decoder::make_config_data(7, 2, 192, 0);
        inst->atrac9->init(def_cfg.data());
    }

    std::int32_t i_id = inst->id;
    it->second->instances[i_id] = std::move(inst);
    diag_.active_instances_count++;
    diag_.last_codec = codec;
    if (out_inst_id) *out_inst_id = i_id;
    return SCE_AJM_OK;
}

std::int32_t AjmManager::instance_create(std::int32_t ctx_id, std::uint32_t codec,
                                        GuestMemory& mem, std::uint64_t param_addr, std::int32_t* out_inst_id) {
    if (param_addr == 0) {
        return instance_create(ctx_id, codec, static_cast<const SceAjmInstanceParam*>(nullptr), out_inst_id);
    }
    SceAjmInstanceParam param{};
    if (mem.load(param_addr, &param, sizeof(param)) != MemoryError::ok) {
        return SCE_AJM_ERROR_INVALID_ARGUMENT;
    }
    return instance_create(ctx_id, codec, &param, out_inst_id);
}

std::int32_t AjmManager::instance_destroy(std::int32_t ctx_id, std::int32_t inst_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = contexts_.find(ctx_id);
    if (it == contexts_.end()) return SCE_AJM_ERROR_INVALID_CONTEXT;
    auto i_it = it->second->instances.find(inst_id);
    if (i_it == it->second->instances.end()) return SCE_AJM_ERROR_INVALID_INSTANCE;
    it->second->instances.erase(i_it);
    if (diag_.active_instances_count > 0) diag_.active_instances_count--;
    return SCE_AJM_OK;
}

std::int32_t AjmManager::batch_start_buffer(std::int32_t ctx_id, std::uint64_t* out_batch_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = contexts_.find(ctx_id);
    if (it == contexts_.end()) return SCE_AJM_ERROR_INVALID_CONTEXT;
    std::uint64_t b_id = it->second->next_batch_id++;
    it->second->batches[b_id] = {};
    diag_.total_batches_started++;
    if (out_batch_id) *out_batch_id = b_id;
    return SCE_AJM_OK;
}

std::int32_t AjmManager::batch_job_run(std::int32_t ctx_id, std::uint64_t batch_id,
                                      GuestMemory& mem, const SceAjmJobDesc& desc,
                                      std::uint32_t* out_bytes_written) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = contexts_.find(ctx_id);
    if (it == contexts_.end()) return SCE_AJM_ERROR_INVALID_CONTEXT;
    auto b_it = it->second->batches.find(batch_id);
    if (b_it == it->second->batches.end()) return SCE_AJM_ERROR_INVALID_ARGUMENT;

    auto i_it = it->second->instances.find(desc.instance_id);
    if (i_it == it->second->instances.end()) return SCE_AJM_ERROR_INVALID_INSTANCE;

    auto& inst = i_it->second;
    if (desc.job_type != SCE_AJM_JOB_TYPE_DECODE) {
        diag_.last_error = SCE_AJM_ERROR_UNSUPPORTED_CODEC;
        return SCE_AJM_ERROR_UNSUPPORTED_CODEC;
    }
    if (desc.in_size == 0 || desc.in_buffer == 0 || desc.out_buffer == 0 || desc.out_size == 0) {
        return SCE_AJM_ERROR_INVALID_ARGUMENT;
    }

    std::vector<std::uint8_t> in_data(desc.in_size);
    if (mem.load(desc.in_buffer, in_data.data(), desc.in_size) != MemoryError::ok) {
        return SCE_AJM_ERROR_INVALID_ARGUMENT;
    }

    std::uint32_t written = 0;
    if (inst->codec == SCE_AJM_CODEC_ATRAC9 && inst->atrac9) {
        std::vector<std::int16_t> pcm_out;
        std::size_t frames = inst->atrac9->decode_stream_to_stereo_48k(in_data.data(), in_data.size(), pcm_out);
        std::size_t pcm_bytes = frames * 2 * sizeof(std::int16_t);
        if (pcm_bytes == 0 || pcm_bytes > desc.out_size) {
            diag_.last_error = SCE_AJM_ERROR_JOB_FAILED;
            return SCE_AJM_ERROR_JOB_FAILED;
        }
        written = static_cast<std::uint32_t>(pcm_bytes);
        if (written > 0) {
            if (mem.store(desc.out_buffer, pcm_out.data(), written) != MemoryError::ok) {
                return SCE_AJM_ERROR_INVALID_ARGUMENT;
            }
        }
    } else {
        diag_.last_error = SCE_AJM_ERROR_UNSUPPORTED_CODEC;
        return SCE_AJM_ERROR_UNSUPPORTED_CODEC;
    }

    diag_.total_jobs_processed++;
    diag_.total_bytes_decoded += written;
    b_it->second.push_back(desc);
    if (out_bytes_written) *out_bytes_written = written;
    return SCE_AJM_OK;
}

std::int32_t AjmManager::batch_job_run_buffer(std::int32_t ctx_id, std::uint64_t batch_id,
                                             GuestMemory& mem, std::uint64_t in_buf, std::uint32_t in_size,
                                             std::uint64_t out_buf, std::uint32_t out_size,
                                             std::int32_t inst_id, std::uint32_t* out_bytes_written) {
    SceAjmJobDesc desc{};
    desc.job_type = SCE_AJM_JOB_TYPE_DECODE;
    desc.instance_id = inst_id;
    desc.in_buffer = in_buf;
    desc.in_size = in_size;
    desc.out_buffer = out_buf;
    desc.out_size = out_size;
    return batch_job_run(ctx_id, batch_id, mem, desc, out_bytes_written);
}

std::int32_t AjmManager::batch_wait(std::int32_t ctx_id, std::uint64_t batch_id, std::uint32_t /*timeout_us*/) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = contexts_.find(ctx_id);
    if (it == contexts_.end()) return SCE_AJM_ERROR_INVALID_CONTEXT;
    auto b_it = it->second->batches.find(batch_id);
    if (b_it == it->second->batches.end()) return SCE_AJM_ERROR_INVALID_ARGUMENT;
    return SCE_AJM_OK;
}

std::int32_t AjmManager::batch_cancel(std::int32_t ctx_id, std::uint64_t batch_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = contexts_.find(ctx_id);
    if (it == contexts_.end()) return SCE_AJM_ERROR_INVALID_CONTEXT;
    auto b_it = it->second->batches.find(batch_id);
    if (b_it == it->second->batches.end()) return SCE_AJM_ERROR_INVALID_ARGUMENT;
    it->second->batches.erase(b_it);
    return SCE_AJM_OK;
}

AjmDiagnostics AjmManager::diagnostics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return diag_;
}

void AjmManager::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    contexts_.clear();
    next_context_id_ = 1;
    diag_ = {};
}

AjmManager& default_ajm_manager() {
    static AjmManager instance;
    return instance;
}

} // namespace bb
