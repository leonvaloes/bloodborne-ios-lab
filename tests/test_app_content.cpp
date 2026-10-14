#include "check.h"
#include "app_content.h"
#include "guest_memory.h"

#include <cstring>
#include <string>

static void test_sysmodule_lifecycle_and_validation() {
    using namespace bb;
    SysmoduleManager sysmgr;

    CHECK(sysmgr.loaded_count() == 0);

    // Validação de IDs inválidos ou nulos
    CHECK(!SysmoduleManager::is_valid_module_id(SCE_SYSMODULE_INVALID));
    CHECK(!SysmoduleManager::is_valid_module_id(0xFFFF));
    CHECK(!SysmoduleManager::is_valid_module_id(0x0500));

    CHECK(sysmgr.sceSysmoduleLoadModule(SCE_SYSMODULE_INVALID) == SCE_SYSMODULE_ERROR_INVALID_VALUE);
    CHECK(sysmgr.sceSysmoduleLoadModule(static_cast<uint16_t>(0xFFFF)) == SCE_SYSMODULE_ERROR_INVALID_VALUE);
    CHECK(sysmgr.sceSysmoduleLoadModule(-1) == SCE_SYSMODULE_ERROR_INVALID_VALUE);
    CHECK(sysmgr.sceSysmoduleLoadModule(0) == SCE_SYSMODULE_ERROR_INVALID_VALUE);

    CHECK(sysmgr.sceSysmoduleUnloadModule(SCE_SYSMODULE_INVALID) == SCE_SYSMODULE_ERROR_INVALID_VALUE);
    CHECK(sysmgr.sceSysmoduleUnloadModule(static_cast<uint16_t>(0xFFFF)) == SCE_SYSMODULE_ERROR_INVALID_VALUE);
    CHECK(sysmgr.sceSysmoduleUnloadModule(-5) == SCE_SYSMODULE_ERROR_INVALID_VALUE);

    CHECK(sysmgr.sceSysmoduleIsLoaded(SCE_SYSMODULE_INVALID) == SCE_SYSMODULE_ERROR_INVALID_VALUE);
    CHECK(sysmgr.sceSysmoduleIsLoaded(static_cast<uint16_t>(0xFFFF)) == SCE_SYSMODULE_ERROR_INVALID_VALUE);

    // Consulta de módulos válidos antes do carregamento
    CHECK(sysmgr.sceSysmoduleIsLoaded(SCE_SYSMODULE_PAD) == SCE_SYSMODULE_ERROR_UNLOADED);
    CHECK(!sysmgr.is_loaded(SCE_SYSMODULE_PAD));
    CHECK(sysmgr.sceSysmoduleUnloadModule(SCE_SYSMODULE_PAD) == SCE_SYSMODULE_ERROR_UNLOADED);

    // Carregamento de módulos do sistema essenciais do Bloodborne
    CHECK(sysmgr.sceSysmoduleLoadModule(SCE_SYSMODULE_FIOS2) == SCE_SYSMODULE_OK);
    CHECK(sysmgr.sceSysmoduleLoadModule(SCE_SYSMODULE_AUDIO_OUT) == SCE_SYSMODULE_OK);
    CHECK(sysmgr.sceSysmoduleLoadModule(SCE_SYSMODULE_PAD) == SCE_SYSMODULE_OK);
    CHECK(sysmgr.sceSysmoduleLoadModule(SCE_SYSMODULE_SAVE_DATA) == SCE_SYSMODULE_OK);
    CHECK(sysmgr.sceSysmoduleLoadModule(SCE_SYSMODULE_VOICE) == SCE_SYSMODULE_OK);

    CHECK(sysmgr.loaded_count() == 5);
    CHECK(sysmgr.is_loaded(SCE_SYSMODULE_FIOS2));
    CHECK(sysmgr.is_loaded(SCE_SYSMODULE_AUDIO_OUT));
    CHECK(sysmgr.is_loaded(SCE_SYSMODULE_PAD));
    CHECK(sysmgr.is_loaded(SCE_SYSMODULE_SAVE_DATA));
    CHECK(sysmgr.is_loaded(SCE_SYSMODULE_VOICE));

    CHECK(sysmgr.sceSysmoduleIsLoaded(SCE_SYSMODULE_PAD) == SCE_SYSMODULE_LOADED);
    CHECK(sysmgr.sceSysmoduleIsLoaded(SCE_SYSMODULE_SAVE_DATA) == SCE_SYSMODULE_LOADED);

    // Verificação de nomes identificáveis dos módulos
    CHECK(std::strcmp(SysmoduleManager::get_module_name(SCE_SYSMODULE_PAD), "libScePad") == 0);
    CHECK(std::strcmp(SysmoduleManager::get_module_name(SCE_SYSMODULE_SAVE_DATA), "libSceSaveData") == 0);
    CHECK(std::strcmp(SysmoduleManager::get_module_name(SCE_SYSMODULE_FIOS2), "libSceFios2") == 0);

    // Prevenção estrita de carregamento duplicado (retorna SCE_SYSMODULE_ERROR_ALREADY_LOADED)
    CHECK(sysmgr.sceSysmoduleLoadModule(SCE_SYSMODULE_PAD) == SCE_SYSMODULE_ERROR_ALREADY_LOADED);
    CHECK(sysmgr.sceSysmoduleLoadModule(SCE_SYSMODULE_FIOS2) == SCE_SYSMODULE_ERROR_ALREADY_LOADED);
    CHECK(sysmgr.loaded_count() == 5);

    // Descarregamento ordenado
    CHECK(sysmgr.sceSysmoduleUnloadModule(SCE_SYSMODULE_PAD) == SCE_SYSMODULE_OK);
    CHECK(sysmgr.loaded_count() == 4);
    CHECK(!sysmgr.is_loaded(SCE_SYSMODULE_PAD));
    CHECK(sysmgr.sceSysmoduleIsLoaded(SCE_SYSMODULE_PAD) == SCE_SYSMODULE_ERROR_UNLOADED);

    // Segundo descarregamento deve falhar com SCE_SYSMODULE_ERROR_UNLOADED
    CHECK(sysmgr.sceSysmoduleUnloadModule(SCE_SYSMODULE_PAD) == SCE_SYSMODULE_ERROR_UNLOADED);

    // Recarregamento do módulo previamente descarregado
    CHECK(sysmgr.sceSysmoduleLoadModule(SCE_SYSMODULE_PAD) == SCE_SYSMODULE_OK);
    CHECK(sysmgr.is_loaded(SCE_SYSMODULE_PAD));
    CHECK(sysmgr.loaded_count() == 5);

    // Reset limpa todos os módulos
    sysmgr.reset();
    CHECK(sysmgr.loaded_count() == 0);
    CHECK(!sysmgr.is_loaded(SCE_SYSMODULE_PAD));
    CHECK(!sysmgr.is_loaded(SCE_SYSMODULE_FIOS2));
}

static void test_app_content_lifecycle() {
    using namespace bb;
    AppContentManager app_mgr;

    CHECK(!app_mgr.is_initialized());

    // Consultas antes da inicialização devem retornar SCE_APP_CONTENT_ERROR_NOT_INITIALIZED
    uint32_t hit_num = 0;
    CHECK(app_mgr.sceAppContentGetAddcontInfoList(0, nullptr, 0, &hit_num) == SCE_APP_CONTENT_ERROR_NOT_INITIALIZED);

    SceAppContentPaddedAddcontInfo p_info{};
    CHECK(app_mgr.sceAppContentGetPaddedAddcontInfo("THEOLDHUNTERS000", &p_info) == SCE_APP_CONTENT_ERROR_NOT_INITIALIZED);

    int32_t val = 0;
    CHECK(app_mgr.sceAppContentAppParamGetInt(SCE_APP_CONTENT_APPPARAM_ID_SKU_FLAG, &val) == SCE_APP_CONTENT_ERROR_NOT_INITIALIZED);

    // Inicialização regular
    SceAppContentBootParam boot_param{};
    CHECK(app_mgr.sceAppContentInitialize(nullptr, &boot_param) == SCE_APP_CONTENT_OK);
    CHECK(app_mgr.is_initialized());
    CHECK(boot_param.user_service_flags == 1);

    // Tentativa de reinicialização retorna SCE_APP_CONTENT_ERROR_BUSY
    CHECK(app_mgr.sceAppContentInitialize(nullptr, nullptr) == SCE_APP_CONTENT_ERROR_BUSY);

    // Finalização e verificação de bloqueio
    CHECK(app_mgr.terminate() == SCE_APP_CONTENT_OK);
    CHECK(!app_mgr.is_initialized());
    CHECK(app_mgr.sceAppContentAppParamGetInt(SCE_APP_CONTENT_APPPARAM_ID_SKU_FLAG, &val) == SCE_APP_CONTENT_ERROR_NOT_INITIALIZED);

    // Inicialização via GuestMemory
    GuestMemory mem(4096);
    CHECK(mem.map(0x1000, 1024, read | write) == MemoryError::ok);

    SceAppContentInitParam guest_init{};
    std::memset(&guest_init, 0xAB, sizeof(guest_init));
    CHECK(mem.store(0x1000, &guest_init, sizeof(guest_init)) == MemoryError::ok);

    CHECK(app_mgr.sceAppContentInitialize(mem, 0x1000, 0x1100) == SCE_APP_CONTENT_OK);
    CHECK(app_mgr.is_initialized());

    SceAppContentBootParam guest_boot{};
    CHECK(mem.load(0x1100, &guest_boot, sizeof(guest_boot)) == MemoryError::ok);
    CHECK(guest_boot.user_service_flags == 1);

    // Verificação de bounds em GuestMemory durante inicialização
    app_mgr.terminate();
    CHECK(!app_mgr.is_initialized());

    // Endereço de init param não mapeado
    CHECK(app_mgr.sceAppContentInitialize(mem, 0x9000, 0x1100) == SCE_APP_CONTENT_ERROR_PARAMETER);
    CHECK(!app_mgr.is_initialized());

    // Endereço de boot param em página somente leitura
    CHECK(mem.map(0x2000, 128, read) == MemoryError::ok);
    CHECK(app_mgr.sceAppContentInitialize(mem, 0x1000, 0x2000) == SCE_APP_CONTENT_ERROR_PARAMETER);
}

static void test_app_content_enumeration_and_guest_memory() {
    using namespace bb;
    AppContentManager app_mgr;
    CHECK(app_mgr.sceAppContentInitialize(nullptr, nullptr) == SCE_APP_CONTENT_OK);

    // Por padrão foram povoados 2 pacotes (The Old Hunters + Patch de compatibilidade)
    CHECK(app_mgr.get_dlc_count() == 2);
    CHECK(app_mgr.has_dlc("THEOLDHUNTERS000"));
    CHECK(app_mgr.has_dlc("COMPATIBILITY001"));

    // Adiciona uma DLC adicional para testes
    CHECK(app_mgr.register_dlc("CUSTOM_MOD_001",
                               SCE_APP_CONTENT_ADDCONT_DOWNLOAD_STATUS_INSTALLED,
                               "Hunter Dream Visual Enhancements",
                               104857600ULL, // 100 MiB
                               "/addcont2",
                               0));
    CHECK(app_mgr.get_dlc_count() == 3);

    // 1. Consulta de contagem: list_num == 0 preenche hit_num com o total
    uint32_t total_hit = 0;
    CHECK(app_mgr.sceAppContentGetAddcontInfoList(0, nullptr, 0, &total_hit) == SCE_APP_CONTENT_OK);
    CHECK(total_hit == 3);

    // list_num == 0 e hit_num nulo retorna erro de parâmetro
    CHECK(app_mgr.sceAppContentGetAddcontInfoList(0, nullptr, 0, nullptr) == SCE_APP_CONTENT_ERROR_PARAMETER);

    // 2. Consulta em GuestMemory: list_num == 0
    GuestMemory mem(8192);
    CHECK(mem.map(0x1000, 4096, read | write) == MemoryError::ok);

    uint64_t hit_num_addr = 0x1000;
    uint64_t list_addr = 0x1100;

    CHECK(app_mgr.sceAppContentGetAddcontInfoList(mem, 0, 0, 0, hit_num_addr) == SCE_APP_CONTENT_OK);
    uint32_t guest_total = 0;
    CHECK(mem.load(hit_num_addr, &guest_total, sizeof(guest_total)) == MemoryError::ok);
    CHECK(guest_total == 3);

    // 3. Enumeração parcial (list_num = 1 < total 3): truncamento seguro sem overflow
    CHECK(app_mgr.sceAppContentGetAddcontInfoList(mem, 0, list_addr, 1, hit_num_addr) == SCE_APP_CONTENT_OK);
    uint32_t written_count = 0;
    CHECK(mem.load(hit_num_addr, &written_count, sizeof(written_count)) == MemoryError::ok);
    CHECK(written_count == 1);

    SceAppContentAddcontInfo single_info{};
    CHECK(mem.load(list_addr, &single_info, sizeof(single_info)) == MemoryError::ok);
    CHECK(std::strcmp(single_info.entitlement_label.data, "THEOLDHUNTERS000") == 0);
    CHECK(single_info.status == SCE_APP_CONTENT_ADDCONT_DOWNLOAD_STATUS_INSTALLED);

    // 4. Enumeração completa de todas as 3 DLCs
    CHECK(app_mgr.sceAppContentGetAddcontInfoList(mem, 0, list_addr, 10, hit_num_addr) == SCE_APP_CONTENT_OK);
    CHECK(mem.load(hit_num_addr, &written_count, sizeof(written_count)) == MemoryError::ok);
    CHECK(written_count == 3);

    SceAppContentAddcontInfo all_infos[3]{};
    CHECK(mem.load(list_addr, all_infos, sizeof(all_infos)) == MemoryError::ok);
    CHECK(std::strcmp(all_infos[0].entitlement_label.data, "THEOLDHUNTERS000") == 0);
    CHECK(all_infos[0].status == SCE_APP_CONTENT_ADDCONT_DOWNLOAD_STATUS_INSTALLED);

    CHECK(std::strcmp(all_infos[1].entitlement_label.data, "COMPATIBILITY001") == 0);
    CHECK(all_infos[1].status == SCE_APP_CONTENT_ADDCONT_DOWNLOAD_STATUS_INSTALLED);

    CHECK(std::strcmp(all_infos[2].entitlement_label.data, "CUSTOM_MOD_001") == 0);
    CHECK(all_infos[2].status == SCE_APP_CONTENT_ADDCONT_DOWNLOAD_STATUS_INSTALLED);

    // 5. Validação rigorosa de bounds e permissões em GuestMemory
    // list_addr nulo com list_num > 0
    CHECK(app_mgr.sceAppContentGetAddcontInfoList(mem, 0, 0, 5, hit_num_addr) == SCE_APP_CONTENT_ERROR_PARAMETER);

    // list_addr não mapeado
    CHECK(app_mgr.sceAppContentGetAddcontInfoList(mem, 0, 0x8888, 5, hit_num_addr) == SCE_APP_CONTENT_ERROR_PARAMETER);

    // hit_num_addr não mapeado
    CHECK(app_mgr.sceAppContentGetAddcontInfoList(mem, 0, list_addr, 5, 0x9999) == SCE_APP_CONTENT_ERROR_PARAMETER);

    // list_addr em região somente de leitura (read sem write)
    CHECK(mem.map(0x6000, 256, read) == MemoryError::ok);
    CHECK(app_mgr.sceAppContentGetAddcontInfoList(mem, 0, 0x6000, 5, hit_num_addr) == SCE_APP_CONTENT_ERROR_PARAMETER);

    // hit_num_addr em região somente de leitura
    CHECK(app_mgr.sceAppContentGetAddcontInfoList(mem, 0, list_addr, 5, 0x6000) == SCE_APP_CONTENT_ERROR_PARAMETER);
}

static void test_app_content_padded_info_and_params() {
    using namespace bb;
    AppContentManager app_mgr;
    CHECK(app_mgr.sceAppContentInitialize(nullptr, nullptr) == SCE_APP_CONTENT_OK);

    // 1. Consulta Host de DLC existente (The Old Hunters)
    SceAppContentPaddedAddcontInfo info{};
    CHECK(app_mgr.sceAppContentGetPaddedAddcontInfo("THEOLDHUNTERS000", &info) == SCE_APP_CONTENT_OK);
    CHECK(std::strcmp(info.entitlement_label.data, "THEOLDHUNTERS000") == 0);
    CHECK(info.status == SCE_APP_CONTENT_ADDCONT_DOWNLOAD_STATUS_INSTALLED);
    CHECK(info.package_type == 0);
    CHECK(info.size_bytes == 3221225472ULL);
    CHECK(std::strcmp(info.mount_point, "/addcont0") == 0);
    CHECK(std::strcmp(info.title, "Bloodborne: The Old Hunters") == 0);

    // Consulta de Patch de compatibilidade
    SceAppContentPaddedAddcontInfo patch_info{};
    CHECK(app_mgr.sceAppContentGetPaddedAddcontInfo("COMPATIBILITY001", &patch_info) == SCE_APP_CONTENT_OK);
    CHECK(patch_info.package_type == 1);
    CHECK(std::strcmp(patch_info.title, "Bloodborne 1.09 Compatibility Data") == 0);

    // Consulta de DLC inexistente retorna DRM_NO_ENTITLEMENT
    SceAppContentPaddedAddcontInfo dummy{};
    CHECK(app_mgr.sceAppContentGetPaddedAddcontInfo("NONEXISTENT_DLC", &dummy) == SCE_APP_CONTENT_ERROR_DRM_NO_ENTITLEMENT);
    CHECK(app_mgr.sceAppContentGetPaddedAddcontInfo("THEOLDHUNTERS000", nullptr) == SCE_APP_CONTENT_ERROR_PARAMETER);

    // 2. Consulta via GuestMemory
    GuestMemory mem(4096);
    CHECK(mem.map(0x2000, 2048, read | write) == MemoryError::ok);

    uint64_t label_addr = 0x2000;
    uint64_t info_addr = 0x2100;

    SceNpUnifiedEntitlementLabel query_label{};
    std::memcpy(query_label.data, "THEOLDHUNTERS000", 16);
    CHECK(mem.store(label_addr, &query_label, sizeof(query_label)) == MemoryError::ok);

    CHECK(app_mgr.sceAppContentGetPaddedAddcontInfo(mem, 0, label_addr, info_addr) == SCE_APP_CONTENT_OK);

    SceAppContentPaddedAddcontInfo guest_padded{};
    CHECK(mem.load(info_addr, &guest_padded, sizeof(guest_padded)) == MemoryError::ok);
    CHECK(std::strcmp(guest_padded.entitlement_label.data, "THEOLDHUNTERS000") == 0);
    CHECK(guest_padded.size_bytes == 3221225472ULL);
    CHECK(std::strcmp(guest_padded.mount_point, "/addcont0") == 0);

    // Bounds checking no GuestMemory
    CHECK(app_mgr.sceAppContentGetPaddedAddcontInfo(mem, 0, 0, info_addr) == SCE_APP_CONTENT_ERROR_PARAMETER);
    CHECK(app_mgr.sceAppContentGetPaddedAddcontInfo(mem, 0, label_addr, 0) == SCE_APP_CONTENT_ERROR_PARAMETER);
    CHECK(app_mgr.sceAppContentGetPaddedAddcontInfo(mem, 0, 0x8888, info_addr) == SCE_APP_CONTENT_ERROR_PARAMETER);
    CHECK(app_mgr.sceAppContentGetPaddedAddcontInfo(mem, 0, label_addr, 0x9999) == SCE_APP_CONTENT_ERROR_PARAMETER);

    // 3. Consulta de AppParams (SKU_FLAG, APP_VER, CATEGORY, PATCH_VER)
    int32_t sku_val = 0;
    CHECK(app_mgr.sceAppContentAppParamGetInt(SCE_APP_CONTENT_APPPARAM_ID_SKU_FLAG, &sku_val) == SCE_APP_CONTENT_OK);
    CHECK(sku_val == SCE_APP_CONTENT_SKU_FLAG_FULL);

    int32_t app_ver = 0;
    CHECK(app_mgr.sceAppContentAppParamGetInt(SCE_APP_CONTENT_APPPARAM_ID_APP_VER, &app_ver) == SCE_APP_CONTENT_OK);
    CHECK(app_ver == 0x0100);

    int32_t patch_ver = 0;
    CHECK(app_mgr.sceAppContentAppParamGetInt(SCE_APP_CONTENT_APPPARAM_ID_PATCH_VER, &patch_ver) == SCE_APP_CONTENT_OK);
    CHECK(patch_ver == 0x0109);

    // Consulta de parâmetro não cadastrado retorna erro
    int32_t unknown_val = 0;
    CHECK(app_mgr.sceAppContentAppParamGetInt(9999, &unknown_val) == SCE_APP_CONTENT_ERROR_PARAMETER);
    CHECK(app_mgr.sceAppContentAppParamGetInt(SCE_APP_CONTENT_APPPARAM_ID_SKU_FLAG, nullptr) == SCE_APP_CONTENT_ERROR_PARAMETER);

    // Consulta de AppParam via GuestMemory
    uint64_t val_addr = 0x2200;
    CHECK(app_mgr.sceAppContentAppParamGetInt(mem, SCE_APP_CONTENT_APPPARAM_ID_PATCH_VER, val_addr) == SCE_APP_CONTENT_OK);
    int32_t guest_patch = 0;
    CHECK(mem.load(val_addr, &guest_patch, sizeof(guest_patch)) == MemoryError::ok);
    CHECK(guest_patch == 0x0109);

    CHECK(app_mgr.sceAppContentAppParamGetInt(mem, SCE_APP_CONTENT_APPPARAM_ID_PATCH_VER, 0) == SCE_APP_CONTENT_ERROR_PARAMETER);
    CHECK(app_mgr.sceAppContentAppParamGetInt(mem, SCE_APP_CONTENT_APPPARAM_ID_PATCH_VER, 0x7777) == SCE_APP_CONTENT_ERROR_PARAMETER);
}

static void test_global_hle_wrappers() {
    using namespace bb;

    // Reset nas instâncias globais
    default_sysmodule().reset();
    default_app_content().reset();

    // Testa Sysmodule global
    CHECK(sceSysmoduleIsLoaded(SCE_SYSMODULE_FIOS2) == SCE_SYSMODULE_ERROR_UNLOADED);
    CHECK(sceSysmoduleLoadModule(SCE_SYSMODULE_FIOS2) == SCE_SYSMODULE_OK);
    CHECK(sceSysmoduleIsLoaded(SCE_SYSMODULE_FIOS2) == SCE_SYSMODULE_LOADED);
    CHECK(sceSysmoduleLoadModule(SCE_SYSMODULE_FIOS2) == SCE_SYSMODULE_ERROR_ALREADY_LOADED);
    CHECK(sceSysmoduleUnloadModule(SCE_SYSMODULE_FIOS2) == SCE_SYSMODULE_OK);
    CHECK(sceSysmoduleIsLoaded(SCE_SYSMODULE_FIOS2) == SCE_SYSMODULE_ERROR_UNLOADED);

    // Testa AppContent global
    CHECK(sceAppContentInitialize(nullptr, nullptr) == SCE_APP_CONTENT_OK);

    uint32_t hits = 0;
    CHECK(sceAppContentGetAddcontInfoList(0, nullptr, 0, &hits) == SCE_APP_CONTENT_OK);
    CHECK(hits >= 2);

    int32_t sku = 0;
    CHECK(sceAppContentAppParamGetInt(SCE_APP_CONTENT_APPPARAM_ID_SKU_FLAG, &sku) == SCE_APP_CONTENT_OK);
    CHECK(sku == SCE_APP_CONTENT_SKU_FLAG_FULL);
}

int main() {
    return run([] {
        test_sysmodule_lifecycle_and_validation();
        test_app_content_lifecycle();
        test_app_content_enumeration_and_guest_memory();
        test_app_content_padded_info_and_params();
        test_global_hle_wrappers();
    });
}
