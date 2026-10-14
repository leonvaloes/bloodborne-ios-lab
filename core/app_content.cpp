#include "app_content.h"
#include "guest_memory.h"

#include <cstring>
#include <algorithm>

namespace bb {

// ============================================================================
// SysmoduleManager
// ============================================================================

struct SysmoduleMeta {
    uint16_t id;
    const char* name;
};

static const SysmoduleMeta kKnownSysmodules[] = {
    { SCE_SYSMODULE_NET,             "libSceNet" },
    { SCE_SYSMODULE_HTTP,            "libSceHttp" },
    { SCE_SYSMODULE_SSL,             "libSceSsl" },
    { SCE_SYSMODULE_COMMUNITY,       "libSceCommunity" },
    { SCE_SYSMODULE_NP_PARTY,        "libSceNpParty" },
    { SCE_SYSMODULE_NP_MATCHING2,    "libSceNpMatching2" },
    { SCE_SYSMODULE_NP_SCORE,        "libSceNpScore" },
    { SCE_SYSMODULE_VOICE,           "libSceVoice" },
    { SCE_SYSMODULE_NP_COMMUNITY,    "libSceNpCommunity" },
    { SCE_SYSMODULE_NP_SIGNALING,    "libSceNpSignaling" },
    { SCE_SYSMODULE_NP_UTILITY,      "libSceNpUtility" },
    { SCE_SYSMODULE_NP_SNS_FACEBOOK, "libSceNpSnsFacebook" },
    { SCE_SYSMODULE_FIOS2,           "libSceFios2" },
    { SCE_SYSMODULE_FONT,            "libSceFont" },
    { SCE_SYSMODULE_FONT_FT,         "libSceFontFt" },
    { SCE_SYSMODULE_FREETYPE_OT,     "libSceFreeTypeOt" },
    { SCE_SYSMODULE_PAD,             "libScePad" },
    { SCE_SYSMODULE_SAVE_DATA,       "libSceSaveData" },
    { SCE_SYSMODULE_AUDIO_OUT,       "libSceAudioOut" },
    { SCE_SYSMODULE_VIDEO_OUT,       "libSceVideoOut" },
    { SCE_SYSMODULE_CAMERA,          "libSceCamera" },
    { SCE_SYSMODULE_MOVE,            "libSceMove" },
    { SCE_SYSMODULE_VOICEQ,          "libSceVoiceQos" },
    { SCE_SYSMODULE_AVPLAYER,        "libSceAvPlayer" },
    { SCE_SYSMODULE_ATRAC9,          "libSceAtrac9" },
    { SCE_SYSMODULE_AUDIO_3D,        "libSceAudio3d" },
    { SCE_SYSMODULE_PNG_DEC,         "libScePngDec" },
    { SCE_SYSMODULE_JPEG_DEC,        "libSceJpegDec" },
    { SCE_SYSMODULE_USER_SERVICE,    "libSceUserService" },
    { SCE_SYSMODULE_APP_CONTENT,     "libSceAppContent" },
    { SCE_SYSMODULE_NP_TROPHY,       "libSceNpTrophy" },
    { SCE_SYSMODULE_NP_AUTH,         "libSceNpAuth" },
    { SCE_SYSMODULE_MESSAGE_DIALOG,  "libSceMsgDialog" },
    { SCE_SYSMODULE_SAVE_DATA_DIALOG,"libSceSaveDataDialog" },
    { SCE_SYSMODULE_WEB_BROWSER_DIALOG, "libSceWebBrowserDialog" },
    { SCE_SYSMODULE_ERROR_DIALOG,    "libSceErrorDialog" },
    { SCE_SYSMODULE_IME,             "libSceIme" },
    { SCE_SYSMODULE_IME_DIALOG,      "libSceImeDialog" },
    { SCE_SYSMODULE_GNM_DRIVER,      "libSceGnmDriver" }
};

SysmoduleManager::SysmoduleManager() = default;

bool SysmoduleManager::is_valid_module_id(uint16_t id) {
    if (id == SCE_SYSMODULE_INVALID) {
        return false;
    }
    for (const auto& entry : kKnownSysmodules) {
        if (entry.id == id) {
            return true;
        }
    }
    return false;
}

const char* SysmoduleManager::get_module_name(uint16_t id) {
    for (const auto& entry : kKnownSysmodules) {
        if (entry.id == id) {
            return entry.name;
        }
    }
    return "unknown";
}

int32_t SysmoduleManager::sceSysmoduleLoadModule(uint16_t id) {
    if (!is_valid_module_id(id)) {
        return SCE_SYSMODULE_ERROR_INVALID_VALUE;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (loaded_modules_.find(id) != loaded_modules_.end()) {
        return SCE_SYSMODULE_ERROR_ALREADY_LOADED;
    }

    loaded_modules_.insert(id);
    return SCE_SYSMODULE_OK;
}

int32_t SysmoduleManager::sceSysmoduleLoadModule(int32_t id) {
    if (id <= 0 || id > 0xFFFF) {
        return SCE_SYSMODULE_ERROR_INVALID_VALUE;
    }
    return sceSysmoduleLoadModule(static_cast<uint16_t>(id));
}

int32_t SysmoduleManager::sceSysmoduleUnloadModule(uint16_t id) {
    if (!is_valid_module_id(id)) {
        return SCE_SYSMODULE_ERROR_INVALID_VALUE;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = loaded_modules_.find(id);
    if (it == loaded_modules_.end()) {
        return SCE_SYSMODULE_ERROR_UNLOADED;
    }

    loaded_modules_.erase(it);
    return SCE_SYSMODULE_OK;
}

int32_t SysmoduleManager::sceSysmoduleUnloadModule(int32_t id) {
    if (id <= 0 || id > 0xFFFF) {
        return SCE_SYSMODULE_ERROR_INVALID_VALUE;
    }
    return sceSysmoduleUnloadModule(static_cast<uint16_t>(id));
}

int32_t SysmoduleManager::sceSysmoduleIsLoaded(uint16_t id) const {
    if (!is_valid_module_id(id)) {
        return SCE_SYSMODULE_ERROR_INVALID_VALUE;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (loaded_modules_.find(id) != loaded_modules_.end()) {
        return SCE_SYSMODULE_LOADED;
    }
    return SCE_SYSMODULE_ERROR_UNLOADED;
}

int32_t SysmoduleManager::sceSysmoduleIsLoaded(int32_t id) const {
    if (id <= 0 || id > 0xFFFF) {
        return SCE_SYSMODULE_ERROR_INVALID_VALUE;
    }
    return sceSysmoduleIsLoaded(static_cast<uint16_t>(id));
}

bool SysmoduleManager::is_loaded(uint16_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return loaded_modules_.find(id) != loaded_modules_.end();
}

std::size_t SysmoduleManager::loaded_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return loaded_modules_.size();
}

std::vector<uint16_t> SysmoduleManager::get_loaded_modules() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<uint16_t>(loaded_modules_.begin(), loaded_modules_.end());
}

void SysmoduleManager::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    loaded_modules_.clear();
}

// ============================================================================
// AppContentManager
// ============================================================================

AppContentManager::AppContentManager() = default;

void AppContentManager::populate_defaults_locked() {
    if (dlcs_.empty()) {
        // DLC The Old Hunters do Bloodborne
        dlcs_.push_back({
            "THEOLDHUNTERS000",
            SCE_APP_CONTENT_ADDCONT_DOWNLOAD_STATUS_INSTALLED,
            0, // DLC
            3221225472ULL, // 3 GiB
            "/addcont0",
            "Bloodborne: The Old Hunters"
        });

        // Patch de Compatibilidade de Título
        dlcs_.push_back({
            "COMPATIBILITY001",
            SCE_APP_CONTENT_ADDCONT_DOWNLOAD_STATUS_INSTALLED,
            1, // Title Patch Compatibility
            268435456ULL, // 256 MiB
            "/addcont1",
            "Bloodborne 1.09 Compatibility Data"
        });
    }

    if (app_params_.empty()) {
        app_params_[SCE_APP_CONTENT_APPPARAM_ID_SKU_FLAG]     = SCE_APP_CONTENT_SKU_FLAG_FULL;
        app_params_[SCE_APP_CONTENT_APPPARAM_ID_APP_VER]      = 0x0100;
        app_params_[SCE_APP_CONTENT_APPPARAM_ID_CATEGORY]     = 0;
        app_params_[SCE_APP_CONTENT_APPPARAM_ID_PATCH_VER]    = 0x0109;
        app_params_[SCE_APP_CONTENT_APPPARAM_ID_USER_PARAM_0] = 0;
        app_params_[SCE_APP_CONTENT_APPPARAM_ID_USER_PARAM_1] = 0;
    }
}

int32_t AppContentManager::sceAppContentInitialize(const SceAppContentInitParam* init_param,
                                                  SceAppContentBootParam* boot_param) {
    (void)init_param;
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        return SCE_APP_CONTENT_ERROR_BUSY;
    }

    initialized_ = true;

    if (boot_param) {
        std::memset(boot_param, 0, sizeof(*boot_param));
        boot_param->user_service_flags = 1;
    }

    populate_defaults_locked();
    return SCE_APP_CONTENT_OK;
}

int32_t AppContentManager::sceAppContentInitialize(GuestMemory& mem,
                                                  uint64_t init_param_addr,
                                                  uint64_t boot_param_addr) {
    if (init_param_addr != 0) {
        SceAppContentInitParam init_param{};
        if (mem.load(init_param_addr, &init_param, sizeof(init_param)) != MemoryError::ok) {
            return SCE_APP_CONTENT_ERROR_PARAMETER;
        }
    }

    SceAppContentBootParam boot_param{};
    int32_t ret = sceAppContentInitialize(nullptr, &boot_param);
    if (ret != SCE_APP_CONTENT_OK) {
        return ret;
    }

    if (boot_param_addr != 0) {
        if (mem.store(boot_param_addr, &boot_param, sizeof(boot_param)) != MemoryError::ok) {
            return SCE_APP_CONTENT_ERROR_PARAMETER;
        }
    }

    return SCE_APP_CONTENT_OK;
}

int32_t AppContentManager::terminate() {
    std::lock_guard<std::mutex> lock(mutex_);
    initialized_ = false;
    return SCE_APP_CONTENT_OK;
}

bool AppContentManager::is_initialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
}

int32_t AppContentManager::sceAppContentGetAddcontInfoList(uint32_t service_label,
                                                         SceAppContentAddcontInfo* list,
                                                         uint32_t list_num,
                                                         uint32_t* hit_num) {
    (void)service_label;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return SCE_APP_CONTENT_ERROR_NOT_INITIALIZED;
    }

    if (list_num == 0) {
        if (hit_num == nullptr) {
            return SCE_APP_CONTENT_ERROR_PARAMETER;
        }
        *hit_num = static_cast<uint32_t>(dlcs_.size());
        return SCE_APP_CONTENT_OK;
    }

    if (list == nullptr) {
        return SCE_APP_CONTENT_ERROR_PARAMETER;
    }

    uint32_t count = std::min(static_cast<uint32_t>(dlcs_.size()), list_num);
    for (uint32_t i = 0; i < count; ++i) {
        std::memset(&list[i], 0, sizeof(SceAppContentAddcontInfo));
        std::size_t copy_len = std::min(dlcs_[i].entitlement_label.size(), sizeof(list[i].entitlement_label.data) - 1);
        std::memcpy(list[i].entitlement_label.data, dlcs_[i].entitlement_label.data(), copy_len);
        list[i].entitlement_label.data[copy_len] = '\0';
        list[i].status = dlcs_[i].status;
    }

    if (hit_num != nullptr) {
        *hit_num = count;
    }

    return SCE_APP_CONTENT_OK;
}

int32_t AppContentManager::sceAppContentGetAddcontInfoList(GuestMemory& mem,
                                                         uint32_t service_label,
                                                         uint64_t list_addr,
                                                         uint32_t list_num,
                                                         uint64_t hit_num_addr) {
    (void)service_label;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return SCE_APP_CONTENT_ERROR_NOT_INITIALIZED;
    }

    if (list_num == 0) {
        if (hit_num_addr == 0) {
            return SCE_APP_CONTENT_ERROR_PARAMETER;
        }
        uint32_t total = static_cast<uint32_t>(dlcs_.size());
        if (mem.store(hit_num_addr, &total, sizeof(total)) != MemoryError::ok) {
            return SCE_APP_CONTENT_ERROR_PARAMETER;
        }
        return SCE_APP_CONTENT_OK;
    }

    if (list_addr == 0) {
        return SCE_APP_CONTENT_ERROR_PARAMETER;
    }

    uint32_t count = std::min(static_cast<uint32_t>(dlcs_.size()), list_num);
    std::vector<SceAppContentAddcontInfo> out_infos(count);
    for (uint32_t i = 0; i < count; ++i) {
        std::memset(&out_infos[i], 0, sizeof(SceAppContentAddcontInfo));
        std::size_t copy_len = std::min(dlcs_[i].entitlement_label.size(), sizeof(out_infos[i].entitlement_label.data) - 1);
        std::memcpy(out_infos[i].entitlement_label.data, dlcs_[i].entitlement_label.data(), copy_len);
        out_infos[i].entitlement_label.data[copy_len] = '\0';
        out_infos[i].status = dlcs_[i].status;
    }

    if (count > 0) {
        std::size_t total_bytes = sizeof(SceAppContentAddcontInfo) * count;
        if (mem.store(list_addr, out_infos.data(), total_bytes) != MemoryError::ok) {
            return SCE_APP_CONTENT_ERROR_PARAMETER;
        }
    }

    if (hit_num_addr != 0) {
        if (mem.store(hit_num_addr, &count, sizeof(count)) != MemoryError::ok) {
            return SCE_APP_CONTENT_ERROR_PARAMETER;
        }
    }

    return SCE_APP_CONTENT_OK;
}

int32_t AppContentManager::sceAppContentGetPaddedAddcontInfo(uint32_t service_label,
                                                           const SceNpUnifiedEntitlementLabel* entitlement_label,
                                                           SceAppContentPaddedAddcontInfo* info) {
    (void)service_label;
    if (!entitlement_label || !info) {
        return SCE_APP_CONTENT_ERROR_PARAMETER;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return SCE_APP_CONTENT_ERROR_NOT_INITIALIZED;
    }

    std::size_t label_len = 0;
    while (label_len < sizeof(entitlement_label->data) && entitlement_label->data[label_len] != '\0') {
        ++label_len;
    }
    std::string query_label(entitlement_label->data, label_len);

    for (const auto& dlc : dlcs_) {
        if (dlc.entitlement_label == query_label) {
            std::memset(info, 0, sizeof(SceAppContentPaddedAddcontInfo));
            std::size_t c_len = std::min(dlc.entitlement_label.size(), sizeof(info->entitlement_label.data) - 1);
            std::memcpy(info->entitlement_label.data, dlc.entitlement_label.data(), c_len);
            info->entitlement_label.data[c_len] = '\0';
            info->status = dlc.status;
            info->package_type = dlc.package_type;
            info->size_bytes = dlc.size_bytes;

            std::size_t mp_len = std::min(dlc.mount_point.size(), sizeof(info->mount_point) - 1);
            std::memcpy(info->mount_point, dlc.mount_point.data(), mp_len);
            info->mount_point[mp_len] = '\0';

            std::size_t t_len = std::min(dlc.title.size(), sizeof(info->title) - 1);
            std::memcpy(info->title, dlc.title.data(), t_len);
            info->title[t_len] = '\0';
            return SCE_APP_CONTENT_OK;
        }
    }

    return SCE_APP_CONTENT_ERROR_DRM_NO_ENTITLEMENT;
}

int32_t AppContentManager::sceAppContentGetPaddedAddcontInfo(GuestMemory& mem,
                                                           uint32_t service_label,
                                                           uint64_t entitlement_label_addr,
                                                           uint64_t info_addr) {
    if (entitlement_label_addr == 0 || info_addr == 0) {
        return SCE_APP_CONTENT_ERROR_PARAMETER;
    }

    SceNpUnifiedEntitlementLabel label{};
    if (mem.load(entitlement_label_addr, &label, sizeof(label)) != MemoryError::ok) {
        return SCE_APP_CONTENT_ERROR_PARAMETER;
    }

    SceAppContentPaddedAddcontInfo padded_info{};
    int32_t ret = sceAppContentGetPaddedAddcontInfo(service_label, &label, &padded_info);
    if (ret != SCE_APP_CONTENT_OK) {
        return ret;
    }

    if (mem.store(info_addr, &padded_info, sizeof(padded_info)) != MemoryError::ok) {
        return SCE_APP_CONTENT_ERROR_PARAMETER;
    }

    return SCE_APP_CONTENT_OK;
}

int32_t AppContentManager::sceAppContentGetPaddedAddcontInfo(const std::string& entitlement_label,
                                                           SceAppContentPaddedAddcontInfo* info) {
    if (!info) {
        return SCE_APP_CONTENT_ERROR_PARAMETER;
    }

    SceNpUnifiedEntitlementLabel label{};
    std::memset(&label, 0, sizeof(label));
    std::size_t len = std::min(entitlement_label.size(), sizeof(label.data) - 1);
    std::memcpy(label.data, entitlement_label.data(), len);

    return sceAppContentGetPaddedAddcontInfo(0, &label, info);
}

int32_t AppContentManager::sceAppContentAppParamGetInt(uint32_t param_id, int32_t* value) {
    if (!value) {
        return SCE_APP_CONTENT_ERROR_PARAMETER;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return SCE_APP_CONTENT_ERROR_NOT_INITIALIZED;
    }

    auto it = app_params_.find(param_id);
    if (it == app_params_.end()) {
        return SCE_APP_CONTENT_ERROR_PARAMETER;
    }

    *value = it->second;
    return SCE_APP_CONTENT_OK;
}

int32_t AppContentManager::sceAppContentAppParamGetInt(GuestMemory& mem,
                                                     uint32_t param_id,
                                                     uint64_t value_addr) {
    if (value_addr == 0) {
        return SCE_APP_CONTENT_ERROR_PARAMETER;
    }

    int32_t val = 0;
    int32_t ret = sceAppContentAppParamGetInt(param_id, &val);
    if (ret != SCE_APP_CONTENT_OK) {
        return ret;
    }

    if (mem.store(value_addr, &val, sizeof(val)) != MemoryError::ok) {
        return SCE_APP_CONTENT_ERROR_PARAMETER;
    }

    return SCE_APP_CONTENT_OK;
}

bool AppContentManager::register_dlc(const std::string& entitlement_label,
                                    uint32_t status,
                                    const std::string& title,
                                    uint64_t size_bytes,
                                    const std::string& mount_point,
                                    uint32_t package_type) {
    if (entitlement_label.empty() || entitlement_label.size() >= SCE_NP_UNIFIED_ENTITLEMENT_LABEL_SIZE) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& item : dlcs_) {
        if (item.entitlement_label == entitlement_label) {
            item.status = status;
            item.title = title;
            item.size_bytes = size_bytes;
            item.mount_point = mount_point;
            item.package_type = package_type;
            return true;
        }
    }

    if (dlcs_.size() >= SCE_APP_CONTENT_MAX_ADDCONT) {
        return false;
    }

    dlcs_.push_back({
        entitlement_label,
        status,
        package_type,
        size_bytes,
        mount_point,
        title
    });
    return true;
}

bool AppContentManager::unregister_dlc(const std::string& entitlement_label) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = dlcs_.begin(); it != dlcs_.end(); ++it) {
        if (it->entitlement_label == entitlement_label) {
            dlcs_.erase(it);
            return true;
        }
    }
    return false;
}

bool AppContentManager::has_dlc(const std::string& entitlement_label) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& item : dlcs_) {
        if (item.entitlement_label == entitlement_label) {
            return true;
        }
    }
    return false;
}

std::size_t AppContentManager::get_dlc_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dlcs_.size();
}

void AppContentManager::clear_dlcs() {
    std::lock_guard<std::mutex> lock(mutex_);
    dlcs_.clear();
}

void AppContentManager::set_app_param(uint32_t param_id, int32_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    app_params_[param_id] = value;
}

bool AppContentManager::get_app_param(uint32_t param_id, int32_t& out_value) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = app_params_.find(param_id);
    if (it != app_params_.end()) {
        out_value = it->second;
        return true;
    }
    return false;
}

void AppContentManager::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    initialized_ = false;
    dlcs_.clear();
    app_params_.clear();
}

// ============================================================================
// Instâncias Globais Padrão e Wrappers HLE
// ============================================================================

AppContentManager& default_app_content() {
    static AppContentManager instance;
    return instance;
}

SysmoduleManager& default_sysmodule() {
    static SysmoduleManager instance;
    return instance;
}

int32_t sceAppContentInitialize(const SceAppContentInitParam* init_param,
                                SceAppContentBootParam* boot_param) {
    return default_app_content().sceAppContentInitialize(init_param, boot_param);
}

int32_t sceAppContentInitialize(GuestMemory& mem,
                                uint64_t init_param_addr,
                                uint64_t boot_param_addr) {
    return default_app_content().sceAppContentInitialize(mem, init_param_addr, boot_param_addr);
}

int32_t sceAppContentGetAddcontInfoList(uint32_t service_label,
                                       SceAppContentAddcontInfo* list,
                                       uint32_t list_num,
                                       uint32_t* hit_num) {
    return default_app_content().sceAppContentGetAddcontInfoList(service_label, list, list_num, hit_num);
}

int32_t sceAppContentGetAddcontInfoList(GuestMemory& mem,
                                       uint32_t service_label,
                                       uint64_t list_addr,
                                       uint32_t list_num,
                                       uint64_t hit_num_addr) {
    return default_app_content().sceAppContentGetAddcontInfoList(mem, service_label, list_addr, list_num, hit_num_addr);
}

int32_t sceAppContentGetPaddedAddcontInfo(uint32_t service_label,
                                         const SceNpUnifiedEntitlementLabel* entitlement_label,
                                         SceAppContentPaddedAddcontInfo* info) {
    return default_app_content().sceAppContentGetPaddedAddcontInfo(service_label, entitlement_label, info);
}

int32_t sceAppContentGetPaddedAddcontInfo(GuestMemory& mem,
                                         uint32_t service_label,
                                         uint64_t entitlement_label_addr,
                                         uint64_t info_addr) {
    return default_app_content().sceAppContentGetPaddedAddcontInfo(mem, service_label, entitlement_label_addr, info_addr);
}

int32_t sceAppContentAppParamGetInt(uint32_t param_id, int32_t* value) {
    return default_app_content().sceAppContentAppParamGetInt(param_id, value);
}

int32_t sceAppContentAppParamGetInt(GuestMemory& mem, uint32_t param_id, uint64_t value_addr) {
    return default_app_content().sceAppContentAppParamGetInt(mem, param_id, value_addr);
}

int32_t sceSysmoduleLoadModule(uint16_t id) {
    return default_sysmodule().sceSysmoduleLoadModule(id);
}

int32_t sceSysmoduleUnloadModule(uint16_t id) {
    return default_sysmodule().sceSysmoduleUnloadModule(id);
}

int32_t sceSysmoduleIsLoaded(uint16_t id) {
    return default_sysmodule().sceSysmoduleIsLoaded(id);
}

} // namespace bb
