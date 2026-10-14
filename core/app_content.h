#ifndef BB_APP_CONTENT_H
#define BB_APP_CONTENT_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>

namespace bb {

class GuestMemory;

// ============================================================================
// libSceSysmodule - Códigos de Retorno e IDs de Módulo do Orbis OS
// ============================================================================

constexpr int32_t SCE_SYSMODULE_OK                     = 0;
constexpr int32_t SCE_SYSMODULE_LOADED                 = 0;
constexpr int32_t SCE_SYSMODULE_ERROR_INVALID_VALUE    = static_cast<int32_t>(0x805A1000);
constexpr int32_t SCE_SYSMODULE_ERROR_UNLOADED         = static_cast<int32_t>(0x805A1001);
constexpr int32_t SCE_SYSMODULE_ERROR_ALREADY_LOADED   = static_cast<int32_t>(0x805A1002);
constexpr int32_t SCE_SYSMODULE_ERROR_FATAL            = static_cast<int32_t>(0x805A10FF);

// Identificadores de módulos do sistema PS4
constexpr uint16_t SCE_SYSMODULE_INVALID               = 0x0000;
constexpr uint16_t SCE_SYSMODULE_NET                   = 0x0001;
constexpr uint16_t SCE_SYSMODULE_HTTP                  = 0x0002;
constexpr uint16_t SCE_SYSMODULE_SSL                   = 0x0003;
constexpr uint16_t SCE_SYSMODULE_COMMUNITY             = 0x0004;
constexpr uint16_t SCE_SYSMODULE_NP_PARTY              = 0x0005;
constexpr uint16_t SCE_SYSMODULE_NP_MATCHING2          = 0x0006;
constexpr uint16_t SCE_SYSMODULE_NP_SCORE              = 0x0007;
constexpr uint16_t SCE_SYSMODULE_VOICE                 = 0x0008;
constexpr uint16_t SCE_SYSMODULE_NP_COMMUNITY          = 0x0009;
constexpr uint16_t SCE_SYSMODULE_NP_SIGNALING          = 0x000A;
constexpr uint16_t SCE_SYSMODULE_NP_UTILITY            = 0x000B;
constexpr uint16_t SCE_SYSMODULE_NP_SNS_FACEBOOK       = 0x000C;
constexpr uint16_t SCE_SYSMODULE_FIOS2                 = 0x000D;
constexpr uint16_t SCE_SYSMODULE_FONT                  = 0x000E;
constexpr uint16_t SCE_SYSMODULE_FONT_FT               = 0x000F;
constexpr uint16_t SCE_SYSMODULE_FREETYPE_OT           = 0x0010;
constexpr uint16_t SCE_SYSMODULE_PAD                   = 0x0011;
constexpr uint16_t SCE_SYSMODULE_SAVE_DATA             = 0x0012;
constexpr uint16_t SCE_SYSMODULE_AUDIO_OUT             = 0x0013;
constexpr uint16_t SCE_SYSMODULE_VIDEO_OUT             = 0x0014;
constexpr uint16_t SCE_SYSMODULE_CAMERA                = 0x0015;
constexpr uint16_t SCE_SYSMODULE_MOVE                  = 0x0016;
constexpr uint16_t SCE_SYSMODULE_VOICEQ                = 0x0017;
constexpr uint16_t SCE_SYSMODULE_AVPLAYER              = 0x0018;
constexpr uint16_t SCE_SYSMODULE_ATRAC9                = 0x0019;
constexpr uint16_t SCE_SYSMODULE_AUDIO_3D              = 0x001A;
constexpr uint16_t SCE_SYSMODULE_PNG_DEC               = 0x001B;
constexpr uint16_t SCE_SYSMODULE_JPEG_DEC              = 0x001C;
constexpr uint16_t SCE_SYSMODULE_USER_SERVICE          = 0x001D;
constexpr uint16_t SCE_SYSMODULE_APP_CONTENT           = 0x001E;
constexpr uint16_t SCE_SYSMODULE_NP_TROPHY             = 0x001F;
constexpr uint16_t SCE_SYSMODULE_NP_AUTH               = 0x0020;
constexpr uint16_t SCE_SYSMODULE_MESSAGE_DIALOG        = 0x0021;
constexpr uint16_t SCE_SYSMODULE_SAVE_DATA_DIALOG      = 0x0022;
constexpr uint16_t SCE_SYSMODULE_WEB_BROWSER_DIALOG    = 0x0023;
constexpr uint16_t SCE_SYSMODULE_ERROR_DIALOG          = 0x0024;
constexpr uint16_t SCE_SYSMODULE_IME                   = 0x0025;
constexpr uint16_t SCE_SYSMODULE_IME_DIALOG            = 0x0026;
constexpr uint16_t SCE_SYSMODULE_GNM_DRIVER            = 0x0027;

// ============================================================================
// libSceAppContent - Códigos de Retorno e Constantes do Orbis OS
// ============================================================================

constexpr int32_t SCE_APP_CONTENT_OK                         = 0;
constexpr int32_t SCE_APP_CONTENT_ERROR_BUSY                 = static_cast<int32_t>(0x80D90001);
constexpr int32_t SCE_APP_CONTENT_ERROR_PARAMETER            = static_cast<int32_t>(0x80D90002);
constexpr int32_t SCE_APP_CONTENT_ERROR_NOT_INITIALIZED      = static_cast<int32_t>(0x80D90003);
constexpr int32_t SCE_APP_CONTENT_ERROR_NOT_MOUNTED          = static_cast<int32_t>(0x80D90004);
constexpr int32_t SCE_APP_CONTENT_ERROR_NOT_FOUND            = static_cast<int32_t>(0x80D90005);
constexpr int32_t SCE_APP_CONTENT_ERROR_NO_MEMORY            = static_cast<int32_t>(0x80D90006);
constexpr int32_t SCE_APP_CONTENT_ERROR_DRM_NO_ENTITLEMENT   = static_cast<int32_t>(0x80D90007);
constexpr int32_t SCE_APP_CONTENT_ERROR_BUFFER_TOO_SMALL     = static_cast<int32_t>(0x80D90008);

// Status de download do Addcont
constexpr uint32_t SCE_APP_CONTENT_ADDCONT_DOWNLOAD_STATUS_NO_EXTRA_DATA      = 0;
constexpr uint32_t SCE_APP_CONTENT_ADDCONT_DOWNLOAD_STATUS_NO_IN_QUEUE        = 1;
constexpr uint32_t SCE_APP_CONTENT_ADDCONT_DOWNLOAD_STATUS_DOWNLOADING        = 2;
constexpr uint32_t SCE_APP_CONTENT_ADDCONT_DOWNLOAD_STATUS_DOWNLOAD_SUSPENDED = 3;
constexpr uint32_t SCE_APP_CONTENT_ADDCONT_DOWNLOAD_STATUS_INSTALLED          = 4;

// Identificadores de parâmetros da aplicação
constexpr uint32_t SCE_APP_CONTENT_APPPARAM_ID_SKU_FLAG      = 0;
constexpr uint32_t SCE_APP_CONTENT_APPPARAM_ID_APP_VER       = 1;
constexpr uint32_t SCE_APP_CONTENT_APPPARAM_ID_CATEGORY      = 2;
constexpr uint32_t SCE_APP_CONTENT_APPPARAM_ID_PATCH_VER     = 3;
constexpr uint32_t SCE_APP_CONTENT_APPPARAM_ID_USER_PARAM_0  = 4;
constexpr uint32_t SCE_APP_CONTENT_APPPARAM_ID_USER_PARAM_1  = 5;

// Flags de SKU
constexpr int32_t SCE_APP_CONTENT_SKU_FLAG_TRIAL             = 1;
constexpr int32_t SCE_APP_CONTENT_SKU_FLAG_FULL              = 3;

// Limites e dimensões
constexpr std::size_t SCE_NP_UNIFIED_ENTITLEMENT_LABEL_SIZE  = 17;
constexpr std::size_t SCE_APP_CONTENT_MOUNT_POINT_SIZE       = 16;
constexpr std::size_t SCE_APP_CONTENT_MAX_ADDCONT            = 64;

#pragma pack(push, 1)

// Rótulo unificado de direito (entitlement label) - 20 bytes
struct SceNpUnifiedEntitlementLabel {
    char data[17];
    char padding[3];
};

// Informações resumidas de Addcont (usadas por sceAppContentGetAddcontInfoList) - 24 bytes
struct SceAppContentAddcontInfo {
    SceNpUnifiedEntitlementLabel entitlement_label;
    uint32_t status;
};

// Informações detalhadas e alinhadas de Addcont (usadas por sceAppContentGetPaddedAddcontInfo) - 148 bytes
struct SceAppContentPaddedAddcontInfo {
    SceNpUnifiedEntitlementLabel entitlement_label;
    uint32_t status;
    uint32_t package_type;
    uint64_t size_bytes;
    char mount_point[16];
    char title[64];
    uint8_t reserved[32];
};

struct SceAppContentInitParam {
    uint8_t reserved[32];
};

struct SceAppContentBootParam {
    uint32_t user_service_flags;
    uint8_t reserved[28];
};

#pragma pack(pop)

static_assert(sizeof(SceNpUnifiedEntitlementLabel) == 20, "SceNpUnifiedEntitlementLabel layout");
static_assert(sizeof(SceAppContentAddcontInfo) == 24, "SceAppContentAddcontInfo layout");
static_assert(sizeof(SceAppContentPaddedAddcontInfo) == 148, "SceAppContentPaddedAddcontInfo layout");
static_assert(sizeof(SceAppContentInitParam) == 32, "SceAppContentInitParam layout");
static_assert(sizeof(SceAppContentBootParam) == 32, "SceAppContentBootParam layout");

// ============================================================================
// SysmoduleManager
// ============================================================================

class SysmoduleManager {
public:
    SysmoduleManager();
    ~SysmoduleManager() = default;

    // Métodos da API HLE libSceSysmodule
    int32_t sceSysmoduleLoadModule(uint16_t id);
    int32_t sceSysmoduleLoadModule(int32_t id);
    int32_t sceSysmoduleUnloadModule(uint16_t id);
    int32_t sceSysmoduleUnloadModule(int32_t id);
    int32_t sceSysmoduleIsLoaded(uint16_t id) const;
    int32_t sceSysmoduleIsLoaded(int32_t id) const;

    // Métodos de conveniência C++
    bool is_loaded(uint16_t id) const;
    std::size_t loaded_count() const;
    std::vector<uint16_t> get_loaded_modules() const;
    void reset();

    // Validação e identificação
    static bool is_valid_module_id(uint16_t id);
    static const char* get_module_name(uint16_t id);

private:
    mutable std::mutex mutex_;
    std::unordered_set<uint16_t> loaded_modules_;
};

// ============================================================================
// AppContentManager
// ============================================================================

struct DlcPackageInfo {
    std::string entitlement_label;
    uint32_t status{SCE_APP_CONTENT_ADDCONT_DOWNLOAD_STATUS_INSTALLED};
    uint32_t package_type{0}; // 0 = DLC / Addcont, 1 = Title Patch Compatibility
    uint64_t size_bytes{0};
    std::string mount_point;
    std::string title;
};

class AppContentManager {
public:
    AppContentManager();
    ~AppContentManager() = default;

    // Ciclo de vida
    int32_t sceAppContentInitialize(const SceAppContentInitParam* init_param = nullptr,
                                    SceAppContentBootParam* boot_param = nullptr);
    int32_t sceAppContentInitialize(GuestMemory& mem,
                                    uint64_t init_param_addr,
                                    uint64_t boot_param_addr);
    int32_t terminate();
    bool is_initialized() const;

    // Enumeração de pacotes de conteúdo adicional
    int32_t sceAppContentGetAddcontInfoList(uint32_t service_label,
                                           SceAppContentAddcontInfo* list,
                                           uint32_t list_num,
                                           uint32_t* hit_num);
    int32_t sceAppContentGetAddcontInfoList(GuestMemory& mem,
                                           uint32_t service_label,
                                           uint64_t list_addr,
                                           uint32_t list_num,
                                           uint64_t hit_num_addr);

    // Consulta de detalhes de DLC específico
    int32_t sceAppContentGetPaddedAddcontInfo(uint32_t service_label,
                                             const SceNpUnifiedEntitlementLabel* entitlement_label,
                                             SceAppContentPaddedAddcontInfo* info);
    int32_t sceAppContentGetPaddedAddcontInfo(GuestMemory& mem,
                                             uint32_t service_label,
                                             uint64_t entitlement_label_addr,
                                             uint64_t info_addr);
    int32_t sceAppContentGetPaddedAddcontInfo(const std::string& entitlement_label,
                                             SceAppContentPaddedAddcontInfo* info);

    // Consulta de metadados da aplicação
    int32_t sceAppContentAppParamGetInt(uint32_t param_id, int32_t* value);
    int32_t sceAppContentAppParamGetInt(GuestMemory& mem, uint32_t param_id, uint64_t value_addr);

    // Gerenciamento e configuração de DLCs
    bool register_dlc(const std::string& entitlement_label,
                      uint32_t status = SCE_APP_CONTENT_ADDCONT_DOWNLOAD_STATUS_INSTALLED,
                      const std::string& title = "",
                      uint64_t size_bytes = 0,
                      const std::string& mount_point = "",
                      uint32_t package_type = 0);
    bool unregister_dlc(const std::string& entitlement_label);
    bool has_dlc(const std::string& entitlement_label) const;
    std::size_t get_dlc_count() const;
    void clear_dlcs();

    // Metadados da aplicação (AppParams)
    void set_app_param(uint32_t param_id, int32_t value);
    bool get_app_param(uint32_t param_id, int32_t& out_value) const;

    // Reset geral
    void reset();

private:
    void populate_defaults_locked();

    mutable std::mutex mutex_;
    bool initialized_{false};
    std::vector<DlcPackageInfo> dlcs_;
    std::unordered_map<uint32_t, int32_t> app_params_;
};

// Instâncias globais padrão HLE
AppContentManager& default_app_content();
SysmoduleManager& default_sysmodule();

// Funções C-style / wrappers globais HLE delegando para default_app_content() e default_sysmodule()
int32_t sceAppContentInitialize(const SceAppContentInitParam* init_param = nullptr,
                                SceAppContentBootParam* boot_param = nullptr);
int32_t sceAppContentInitialize(GuestMemory& mem,
                                uint64_t init_param_addr,
                                uint64_t boot_param_addr);
int32_t sceAppContentGetAddcontInfoList(uint32_t service_label,
                                       SceAppContentAddcontInfo* list,
                                       uint32_t list_num,
                                       uint32_t* hit_num);
int32_t sceAppContentGetAddcontInfoList(GuestMemory& mem,
                                       uint32_t service_label,
                                       uint64_t list_addr,
                                       uint32_t list_num,
                                       uint64_t hit_num_addr);
int32_t sceAppContentGetPaddedAddcontInfo(uint32_t service_label,
                                         const SceNpUnifiedEntitlementLabel* entitlement_label,
                                         SceAppContentPaddedAddcontInfo* info);
int32_t sceAppContentGetPaddedAddcontInfo(GuestMemory& mem,
                                         uint32_t service_label,
                                         uint64_t entitlement_label_addr,
                                         uint64_t info_addr);
int32_t sceAppContentAppParamGetInt(uint32_t param_id, int32_t* value);
int32_t sceAppContentAppParamGetInt(GuestMemory& mem, uint32_t param_id, uint64_t value_addr);

int32_t sceSysmoduleLoadModule(uint16_t id);
int32_t sceSysmoduleUnloadModule(uint16_t id);
int32_t sceSysmoduleIsLoaded(uint16_t id);

} // namespace bb

#endif // BB_APP_CONTENT_H
