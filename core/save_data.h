#ifndef BB_SAVE_DATA_H
#define BB_SAVE_DATA_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <array>
#include <mutex>
#include <filesystem>
#include <unordered_map>

namespace bb {

class VirtualFileSystem;

// Orbis OS libSceSaveData Error Codes
constexpr int32_t SCE_SAVEDATA_OK                     = 0;
constexpr int32_t SCE_SAVEDATA_ERROR_PARAMETER        = static_cast<int32_t>(0x809F0000);
constexpr int32_t SCE_SAVEDATA_ERROR_NOT_INITIALIZED  = static_cast<int32_t>(0x809F0001);
constexpr int32_t SCE_SAVEDATA_ERROR_NOT_MOUNTED      = static_cast<int32_t>(0x809F0004);
constexpr int32_t SCE_SAVEDATA_ERROR_BROKEN           = static_cast<int32_t>(0x809F0005);
constexpr int32_t SCE_SAVEDATA_ERROR_EXISTS           = static_cast<int32_t>(0x809F0007);
constexpr int32_t SCE_SAVEDATA_ERROR_NOT_FOUND        = static_cast<int32_t>(0x809F0008);
constexpr int32_t SCE_SAVEDATA_ERROR_INTERNAL         = static_cast<int32_t>(0x809F000B);
constexpr int32_t SCE_SAVEDATA_ERROR_MOUNT_FULL       = static_cast<int32_t>(0x809F000C);
constexpr int32_t SCE_SAVEDATA_ERROR_BAD_MOUNTED      = static_cast<int32_t>(0x809F000D);
constexpr int32_t SCE_SAVEDATA_ERROR_INVALID_USER     = static_cast<int32_t>(0x809F0011);
constexpr int32_t SCE_SAVEDATA_ERROR_MEMORY_NOT_READY = static_cast<int32_t>(0x809F0012);
constexpr int32_t SCE_SAVEDATA_ERROR_QUOTA_EXCEEDED   = static_cast<int32_t>(0x809F0013);
constexpr int32_t SCE_SAVEDATA_ERROR_PATH_TRAVERSAL   = static_cast<int32_t>(0x809F0014);

// Mount Mode Flags
constexpr uint32_t SCE_SAVEDATA_MOUNT_MODE_RDONLY     = 1;
constexpr uint32_t SCE_SAVEDATA_MOUNT_MODE_RDWR       = 2;
constexpr uint32_t SCE_SAVEDATA_MOUNT_MODE_CREATE     = 4;
constexpr uint32_t SCE_SAVEDATA_MOUNT_MODE_COPY_ICON  = 16;
constexpr uint32_t SCE_SAVEDATA_MOUNT_MODE_CREATE2    = 32;

// Mount Status
constexpr uint32_t SCE_SAVEDATA_MOUNT_STATUS_EXISTING = 0;
constexpr uint32_t SCE_SAVEDATA_MOUNT_STATUS_CREATED  = 1;

// Umount Flags
constexpr uint32_t SCE_SAVEDATA_UMOUNT_FLAG_DISCARD   = 0;
constexpr uint32_t SCE_SAVEDATA_UMOUNT_FLAG_COMMIT    = 1;

// Search sort keys & orders
constexpr uint32_t SCE_SAVEDATA_SORT_KEY_DIRNAME      = 0;
constexpr uint32_t SCE_SAVEDATA_SORT_KEY_USER_PARAM   = 1;
constexpr uint32_t SCE_SAVEDATA_SORT_KEY_MTIME        = 3;
constexpr uint32_t SCE_SAVEDATA_SORT_ORDER_ASC        = 0;
constexpr uint32_t SCE_SAVEDATA_SORT_ORDER_DESC       = 1;

// Capacity & Limits
constexpr std::size_t SCE_SAVEDATA_MAX_SLOTS          = 16;
constexpr std::size_t SCE_SAVEDATA_DEFAULT_QUOTA      = 32 * 1024 * 1024; // 32 MiB por slot

struct SceSaveDataTitleId {
    char data[10];
    char pad[6];
};

struct SceSaveDataDirName {
    char data[32];
};

struct SceSaveDataMountPoint {
    char data[16];
};

struct SceSaveDataParam {
    char title[128];
    char subtitle[128];
    char detail[1024];
    uint32_t user_param;
    int32_t pad;
    int64_t mtime;
    uint8_t reserved[32];
};

struct SceSaveDataMount {
    int32_t user;
    int32_t pad;
    const SceSaveDataTitleId *title;
    const SceSaveDataDirName *dir;
    const void *fingerprint;
    uint64_t blocks;
    uint32_t mode;
    uint8_t reserved[32];
};

struct SceSaveDataMount2 {
    int32_t user;
    int32_t pad;
    const SceSaveDataDirName *dir;
    uint64_t blocks;
    uint32_t mode;
    uint8_t reserved[32];
    int32_t pad2;
};

struct SceSaveDataMountResult {
    SceSaveDataMountPoint point;
    uint64_t required_blocks;
    uint32_t unused;
    uint32_t status;
    uint8_t reserved[28];
    int32_t pad;
};

struct SceSaveDataDelete {
    int32_t user;
    int32_t pad;
    const SceSaveDataTitleId *title;
    const SceSaveDataDirName *dir;
    uint32_t unused;
    uint8_t reserved[32];
    int32_t pad2;
};

struct SceSaveDataSearchCond {
    int32_t user;
    int32_t pad;
    const SceSaveDataTitleId *title;
    const SceSaveDataDirName *dir;
    uint32_t key;
    uint32_t order;
    uint8_t reserved[32];
};

struct SceSaveDataSearchInfo {
    uint64_t blocks;
    uint64_t free_blocks;
    uint8_t reserved[32];
};

struct SceSaveDataSearchResult {
    uint32_t hits;
    int32_t pad;
    SceSaveDataDirName *names;
    uint32_t names_capacity;
    uint32_t set_count;
    SceSaveDataParam *params;
    SceSaveDataSearchInfo *infos;
    uint8_t reserved[12];
    int32_t pad2;
};

struct SceSaveDataIcon {
    const void *buffer;
    uint64_t buffer_size;
    uint64_t data_size;
    uint8_t reserved[32];
};

struct SceSaveDataInitParams {
    uint64_t max_quota_per_slot;
    uint8_t reserved[56];
};

static_assert(sizeof(SceSaveDataParam) == 1328, "SceSaveDataParam layout");
static_assert(sizeof(SceSaveDataMount) == 80, "SceSaveDataMount layout");
static_assert(sizeof(SceSaveDataMount2) == 64, "SceSaveDataMount2 layout");
static_assert(sizeof(SceSaveDataMountResult) == 64, "SceSaveDataMountResult layout");
static_assert(sizeof(SceSaveDataDelete) == 64, "SceSaveDataDelete layout");
static_assert(sizeof(SceSaveDataSearchCond) == 64, "SceSaveDataSearchCond layout");
static_assert(sizeof(SceSaveDataSearchResult) == 56, "SceSaveDataSearchResult layout");
static_assert(sizeof(SceSaveDataInitParams) == 64, "SceSaveDataInitParams layout");

struct SaveSlotInfo {
    bool used{false};
    bool created_slot{false};
    bool original_param_exists{false};
    SceSaveDataParam original_param{};
    bool original_icon_exists{false};
    std::vector<uint8_t> original_icon{};
    int32_t user{0};
    std::string title_id;
    std::string dir_name;
    std::string host_path;        // Diretorio persistente no armazenamento
    std::string meta_path;        // Diretorio de metadados (.sce_sys)
    std::string work_path;        // Copia de trabalho isolada (staging transacional)
    std::string mount_point;      // Ponto de montagem virtual (ex: /savedata0)
    uint32_t mode{0};
    bool read_only{false};
    uint64_t blocks{0};
};

struct SaveSearchEntry {
    std::string dir_name;
    SceSaveDataParam param{};
    uint64_t blocks{32768};
    uint64_t free_blocks{16384};
};

// Operacoes injetaveis para testar falhas de exclusao sem depender de ACL do host.
class SaveDeleteFilesystem {
public:
    virtual ~SaveDeleteFilesystem() = default;
    virtual bool exists(const std::filesystem::path &path, std::error_code &ec) = 0;
    virtual std::uintmax_t remove_all(const std::filesystem::path &path, std::error_code &ec) = 0;
};

class SaveDataManager {
public:
    static constexpr std::size_t MAX_SLOTS = SCE_SAVEDATA_MAX_SLOTS;
    static constexpr std::size_t DEFAULT_MAX_QUOTA_PER_SLOT = SCE_SAVEDATA_DEFAULT_QUOTA;

    SaveDataManager();
    ~SaveDataManager();

    static SaveDataManager& instance();

    // Ciclo de vida do subsistema
    bool initialize(const std::string &base_directory,
                    std::size_t max_quota = DEFAULT_MAX_QUOTA_PER_SLOT,
                    VirtualFileSystem *vfs = nullptr);
    bool terminate();
    bool is_initialized() const;

    // Configuracao
    void set_base_directory(const std::string &base_directory);
    const std::string& get_base_directory() const;
    void set_default_title_id(const std::string &title_id);
    const std::string& get_default_title_id() const;
    void set_max_quota_per_slot(std::size_t quota_bytes);
    std::size_t get_max_quota_per_slot() const;
    void set_vfs(VirtualFileSystem *vfs);
    VirtualFileSystem* get_vfs() const;

    // Gerenciamento de slots
    int32_t mount(int32_t user, const char *title, const char *dir_name, uint32_t mode,
                  uint64_t blocks, std::string &out_mount_point, uint32_t &out_status);
    int32_t umount(const std::string &mount_point, bool commit = true);
    int32_t delete_save(int32_t user, const char *title, const char *dir_name);
    // nullptr restaura as operacoes padrao; o chamador mantem a instancia viva.
    void set_delete_filesystem_for_testing(SaveDeleteFilesystem *filesystem);
    enum class WriteFailurePhase { None, TmpWrite, Checksum, Backup, Replacement, MetadataUpdate };
    void set_test_write_failure_phase(WriteFailurePhase phase) { test_write_failure_phase_ = phase; }
    WriteFailurePhase get_test_write_failure_phase() const { return test_write_failure_phase_; }
    int32_t dir_name_search(int32_t user, const char *title, const char *pattern,
                            uint32_t sort_key, uint32_t sort_order,
                            std::vector<SaveSearchEntry> &out_entries);

    // Metadados e Icone
    int32_t set_param(const std::string &mount_point, uint32_t type, const void *buf, std::size_t size);
    int32_t get_param(const std::string &mount_point, SceSaveDataParam &out_param) const;
    int32_t save_icon(const std::string &mount_point, const void *data, std::size_t size);

    // SaveDataMemory
    int32_t setup_memory(int32_t user, uint64_t size, const SceSaveDataParam *param);
    int32_t get_memory(int32_t user, void *buffer, uint64_t size, int64_t offset);
    int32_t set_memory(int32_t user, const void *buffer, uint64_t size, int64_t offset);

    // Manipulacao de arquivos dentro do slot montado (com quota e traversal)
    int32_t write_file(const std::string &mount_point, const std::string &relative_path,
                       const void *data, std::size_t size, bool atomic = true);
    int32_t read_file(const std::string &mount_point, const std::string &relative_path,
                      std::vector<uint8_t> &out_data) const;
    int32_t remove_file(const std::string &mount_point, const std::string &relative_path);
    bool file_exists(const std::string &mount_point, const std::string &relative_path) const;

    // Consultas de slot
    int find_slot(const std::string &mount_point) const;
    const SaveSlotInfo* get_slot(int index) const;
    std::size_t calculate_directory_size(const std::string &dir_path) const;
    std::size_t calculate_slot_size(int index) const;

    // Validacao e protecao contra traversal
    static bool is_valid_dir_name(const char *name, std::size_t max_len = 32);
    static bool is_valid_title_id(const char *title, std::size_t max_len = 16);
    static bool contains_traversal(const std::string &path);
    static bool pattern_matches(const std::string &str, const std::string &pattern);
    std::filesystem::path resolve_savedata_root() const;
    bool is_confined_to_savedata_root(const std::filesystem::path &target) const;
    bool check_work_quota(const std::string &file_path, std::int64_t current_file_size, std::int64_t new_file_size) const;
    void recover_interrupted_commits();

    // Ajuste legado opt-in; montagens normais preservam integralmente o save.
    void set_sound_hack_enabled(bool enabled);
    void apply_bloodborne_sound_hack(const std::string &work_path);

private:
    mutable std::mutex mutex_;
    SaveDeleteFilesystem *delete_filesystem_for_testing_{nullptr};
    bool initialized_{false};
    bool sound_hack_enabled_{false};
    WriteFailurePhase test_write_failure_phase_{WriteFailurePhase::None};
    std::string base_dir_;
    std::string default_title_id_{"CUSA03173"};
    std::size_t max_quota_per_slot_{DEFAULT_MAX_QUOTA_PER_SLOT};
    VirtualFileSystem *vfs_{nullptr};
    std::array<SaveSlotInfo, MAX_SLOTS> slots_{};

    // SaveDataMemory per-user
    struct UserMemoryState {
        std::string path;
        uint64_t size{0};
    };
    std::unordered_map<int32_t, UserMemoryState> user_memories_;

    std::string resolve_save_dir(int32_t user, const char *title, const char *dir_name) const;
    std::string resolve_meta_dir(int32_t user, const char *title, const char *dir_name) const;
    bool atomic_commit_directory(const std::string &staging_dir, const std::string &dest_dir);
    bool write_meta_param(const std::string &meta_dir, const SceSaveDataParam &param);
    bool read_meta_param(const std::string &meta_dir, SceSaveDataParam &param) const;
    static std::string compute_directory_sha256(const std::filesystem::path &dir_path);
    static bool write_checksum(const std::filesystem::path &meta_dir, const std::string &checksum);
    static bool verify_checksum(const std::filesystem::path &host_dir, const std::filesystem::path &meta_dir);
};

} // namespace bb

#ifdef __cplusplus
extern "C" {
#endif

int32_t sceSaveDataInitialize(const bb::SceSaveDataInitParams *init_params);
int32_t sceSaveDataInitialize2(const bb::SceSaveDataInitParams *init_params);
int32_t sceSaveDataInitialize3(const bb::SceSaveDataInitParams *init_params);
int32_t sceSaveDataTerminate(void);

int32_t sceSaveDataMount(const bb::SceSaveDataMount *mount, bb::SceSaveDataMountResult *result);
int32_t sceSaveDataMount2(const bb::SceSaveDataMount2 *mount, bb::SceSaveDataMountResult *result);
#ifdef __cplusplus
int32_t sceSaveDataUmount(const bb::SceSaveDataMountPoint *mount_point, uint32_t flags = bb::SCE_SAVEDATA_UMOUNT_FLAG_COMMIT);
#else
int32_t sceSaveDataUmount(const bb::SceSaveDataMountPoint *mount_point, uint32_t flags);
#endif
int32_t sceSaveDataUmountWithCommit(const bb::SceSaveDataMountPoint *mount_point, bool commit);
int32_t sceSaveDataDelete(const bb::SceSaveDataDelete *del);
int32_t sceSaveDataDirNameSearch(const bb::SceSaveDataSearchCond *cond, bb::SceSaveDataSearchResult *result);
int32_t sceSaveDataSetParam(const bb::SceSaveDataMountPoint *mount_point, uint32_t type, const void *buffer, uint64_t size);
int32_t sceSaveDataSaveIcon(const bb::SceSaveDataMountPoint *mount_point, const bb::SceSaveDataIcon *icon);
int32_t sceSaveDataSetupSaveDataMemory(int32_t user, uint64_t size, const bb::SceSaveDataParam *param);
int32_t sceSaveDataGetSaveDataMemory(int32_t user, void *buffer, uint64_t size, int64_t offset);
int32_t sceSaveDataSetSaveDataMemory(int32_t user, const void *buffer, uint64_t size, int64_t offset);

#ifdef __cplusplus
}
#endif

#endif // BB_SAVE_DATA_H
