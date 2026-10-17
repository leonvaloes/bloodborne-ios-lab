#ifndef BB_FIOS2_HLE_H
#define BB_FIOS2_HLE_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include "vfs.h"
#include "guest_memory.h"

namespace bb {

// Tipos de identificadores para FIOS2 (compatíveis com libSceFios2 do PS4)
using SceFiosFH = std::int32_t;
using SceFiosDH = std::int32_t;
using SceFiosOp = std::int32_t;

// Identificadores inválidos
constexpr SceFiosFH SCE_FIOS_FH_INVALID = -1;
constexpr SceFiosDH SCE_FIOS_DH_INVALID = -1;
constexpr SceFiosOp SCE_FIOS_OP_INVALID = -1;

// Limites rígidos do subsistema FIOS2
constexpr std::size_t SCE_FIOS_MAX_HANDLES = 256;
constexpr std::size_t SCE_FIOS_MAX_PATH = 1024;
constexpr std::size_t SCE_FIOS_MAX_NAME = 256;

// Códigos de erro PS4 / Orbis OS para libSceFios2
constexpr std::int32_t SCE_FIOS_OK                       = 0;
constexpr std::int32_t SCE_FIOS_ERROR_INVALID_ARGUMENT   = static_cast<std::int32_t>(0x80530001);
constexpr std::int32_t SCE_FIOS_ERROR_INVALID_HANDLE     = static_cast<std::int32_t>(0x80530002);
constexpr std::int32_t SCE_FIOS_ERROR_NOT_FOUND          = static_cast<std::int32_t>(0x80530003);
constexpr std::int32_t SCE_FIOS_ERROR_ACCESS_DENIED      = static_cast<std::int32_t>(0x80530004);
constexpr std::int32_t SCE_FIOS_ERROR_OUT_OF_MEMORY      = static_cast<std::int32_t>(0x80530005);
constexpr std::int32_t SCE_FIOS_ERROR_TOO_MANY_HANDLES   = static_cast<std::int32_t>(0x80530006);
constexpr std::int32_t SCE_FIOS_ERROR_PATH_TRAVERSAL     = static_cast<std::int32_t>(0x80530007);
constexpr std::int32_t SCE_FIOS_ERROR_IO                 = static_cast<std::int32_t>(0x80530008);
constexpr std::int32_t SCE_FIOS_ERROR_NOT_MOUNTED        = static_cast<std::int32_t>(0x80530009);
constexpr std::int32_t SCE_FIOS_ERROR_ALREADY_EXISTS     = static_cast<std::int32_t>(0x8053000A);
constexpr std::int32_t SCE_FIOS_ERROR_NOT_INITIALIZED    = static_cast<std::int32_t>(0x8053000B);
constexpr std::int32_t SCE_FIOS_ERROR_NOT_A_DIRECTORY    = static_cast<std::int32_t>(0x8053000C);
constexpr std::int32_t SCE_FIOS_ERROR_IS_A_DIRECTORY     = static_cast<std::int32_t>(0x8053000D);
constexpr std::int32_t SCE_FIOS_ERROR_DISK_FULL         = static_cast<std::int32_t>(0x8053000E);
constexpr std::int32_t SCE_FIOS_ERROR_SEEK_OUT_OF_BOUNDS = static_cast<std::int32_t>(0x8053000F);
constexpr std::int32_t SCE_FIOS_ERROR_EOF                = static_cast<std::int32_t>(0x80530010);

// Flags de abertura de arquivo
constexpr std::uint32_t SCE_FIOS_O_RDONLY = 0x00000001;
constexpr std::uint32_t SCE_FIOS_O_WRONLY = 0x00000002;
constexpr std::uint32_t SCE_FIOS_O_RDWR   = 0x00000003;
constexpr std::uint32_t SCE_FIOS_O_APPEND = 0x00000008;
constexpr std::uint32_t SCE_FIOS_O_CREAT  = 0x00000200;
constexpr std::uint32_t SCE_FIOS_O_TRUNC  = 0x00000400;

// Origens de Seek
constexpr std::int32_t SCE_FIOS_SEEK_SET = 0;
constexpr std::int32_t SCE_FIOS_SEEK_CUR = 1;
constexpr std::int32_t SCE_FIOS_SEEK_END = 2;

// Atributos de arquivo / diretório
constexpr std::uint32_t SCE_FIOS_ATTR_READONLY  = 0x00000001;
constexpr std::uint32_t SCE_FIOS_ATTR_DIRECTORY = 0x00000002;
constexpr std::uint32_t SCE_FIOS_ATTR_REGULAR   = 0x00000004;
constexpr std::uint32_t SCE_FIOS_ATTR_ARCHIVE   = 0x00000020;

#pragma pack(push, 1)
// Estrutura de status de arquivo para libSceFios2
struct SceFiosStat {
    std::uint64_t size{0};
    std::uint64_t access_time{0};
    std::uint64_t modify_time{0};
    std::uint64_t create_time{0};
    std::uint32_t attributes{0};
    std::uint32_t flags{0};
};
static_assert(sizeof(SceFiosStat) == 40, "SceFiosStat must be 40 bytes");

// Estrutura de entrada de diretório para libSceFios2
struct SceFiosDirEntry {
    std::uint64_t size{0};
    std::uint32_t attributes{0};
    std::uint32_t reserved{0};
    char name[SCE_FIOS_MAX_NAME]{0};
};
static_assert(sizeof(SceFiosDirEntry) == 272, "SceFiosDirEntry must be 272 bytes");
#pragma pack(pop)

// Emulador HLE da camada de I/O libSceFios2
class Fios2Manager {
public:
    explicit Fios2Manager(VirtualFileSystem* vfs = nullptr, GuestMemory* guest_memory = nullptr);
    ~Fios2Manager();

    // VFS e Memória do Guest
    void set_vfs(VirtualFileSystem* vfs);
    VirtualFileSystem* vfs() const;
    void set_guest_memory(GuestMemory* memory);
    GuestMemory* guest_memory() const;

    // ------------------------------------------------------------------------
    // 1. Gerenciamento de File Handles (SceFiosFH)
    // ------------------------------------------------------------------------
    SceFiosFH sceFiosFHOpenSync(const std::string& path, std::uint32_t open_flags);
    SceFiosFH sceFiosFHOpenWithModeSync(const std::string& path, std::uint32_t open_flags, std::uint32_t mode);

    std::int32_t sceFiosFHOpenSync(SceFiosOp* op, SceFiosFH* out_fh, const char* path, std::uint32_t open_flags);
    std::int32_t sceFiosFHOpenWithModeSync(SceFiosOp* op, SceFiosFH* out_fh, const char* path, std::uint32_t open_flags, std::uint32_t mode);

    std::int32_t sceFiosFHCloseSync(SceFiosFH fh);
    std::int32_t sceFiosFHCloseSync(SceFiosOp* op, SceFiosFH fh);

    // Leitura: memória host ou GuestMemory
    std::int64_t sceFiosFHReadSync(SceFiosFH fh, void* data, std::size_t size);
    std::int64_t sceFiosFHReadSync(SceFiosOp* op, SceFiosFH fh, void* data, std::size_t size);
    std::int64_t sceFiosFHReadSync(SceFiosFH fh, GuestMemory& memory, std::uint64_t guest_addr, std::size_t size);
    std::int64_t sceFiosFHReadSync(SceFiosOp* op, SceFiosFH fh, GuestMemory& memory, std::uint64_t guest_addr, std::size_t size);

    // Escrita: memória host ou GuestMemory
    std::int64_t sceFiosFHWriteSync(SceFiosFH fh, const void* data, std::size_t size);
    std::int64_t sceFiosFHWriteSync(SceFiosOp* op, SceFiosFH fh, const void* data, std::size_t size);
    std::int64_t sceFiosFHWriteSync(SceFiosFH fh, const GuestMemory& memory, std::uint64_t guest_addr, std::size_t size);
    std::int64_t sceFiosFHWriteSync(SceFiosOp* op, SceFiosFH fh, const GuestMemory& memory, std::uint64_t guest_addr, std::size_t size);

    // Cursor e sincronização
    std::int64_t sceFiosFHSeek(SceFiosFH fh, std::int64_t offset, std::int32_t whence);
    std::int64_t sceFiosFHTell(SceFiosFH fh);
    std::int32_t sceFiosFHSyncSync(SceFiosFH fh);
    std::int32_t sceFiosFHSyncSync(SceFiosOp* op, SceFiosFH fh);
    std::int32_t sceFiosFHTruncateSync(SceFiosFH fh, std::int64_t length);
    std::int32_t sceFiosFHTruncateSync(SceFiosOp* op, SceFiosFH fh, std::int64_t length);
    std::int32_t sceFiosFileTruncateSync(const std::string& path, std::int64_t length);
    std::int32_t sceFiosFileTruncateSync(SceFiosOp* op, const char* path, std::int64_t length);

    // Validação de handle
    bool sceFiosIsValidHandle(SceFiosFH fh) const;

    // ------------------------------------------------------------------------
    // 2. Gerenciamento de Directory Handles (SceFiosDH)
    // ------------------------------------------------------------------------
    SceFiosDH sceFiosDHOpenSync(const std::string& path);
    std::int32_t sceFiosDHOpenSync(SceFiosOp* op, SceFiosDH* out_dh, const char* path);

    std::int32_t sceFiosDHCloseSync(SceFiosDH dh);
    std::int32_t sceFiosDHCloseSync(SceFiosOp* op, SceFiosDH dh);

    bool sceFiosIsValidDH(SceFiosDH dh) const;

    // Leitura sequencial de diretório
    std::int32_t sceFiosDHReadSync(SceFiosDH dh, SceFiosDirEntry* out_entry);
    std::int32_t sceFiosDHReadSync(SceFiosOp* op, SceFiosDH dh, SceFiosDirEntry* out_entry);
    std::int32_t sceFiosDHReadSync(SceFiosDH dh, GuestMemory& memory, std::uint64_t guest_addr);
    std::int32_t sceFiosDHReadSync(SceFiosOp* op, SceFiosDH dh, GuestMemory& memory, std::uint64_t guest_addr);

    // ------------------------------------------------------------------------
    // 3. Operações síncronas de arquivos
    // ------------------------------------------------------------------------
    std::int32_t sceFiosStatSync(const std::string& path, SceFiosStat& out_stat);
    std::int32_t sceFiosStatSync(SceFiosOp* op, const char* path, SceFiosStat* out_stat);
    std::int32_t sceFiosStatSync(const std::string& path, GuestMemory& memory, std::uint64_t guest_addr);
    std::int32_t sceFiosStatSync(SceFiosOp* op, const char* path, GuestMemory& memory, std::uint64_t guest_addr);

    std::int32_t sceFiosFileDeleteSync(const std::string& path);
    std::int32_t sceFiosFileDeleteSync(SceFiosOp* op, const char* path);

    std::int32_t sceFiosDirectoryCreate(const std::string& path);
    std::int32_t sceFiosDirectoryCreate(SceFiosOp* op, const char* path);

    std::int32_t sceFiosRenameSync(const std::string& old_path, const std::string& new_path);
    std::int32_t sceFiosRenameSync(SceFiosOp* op, const char* old_path, const char* new_path);

    // ------------------------------------------------------------------------
    // 4. Operações assíncronas / Ops
    // ------------------------------------------------------------------------
    SceFiosOp sceFiosOpCreate(std::int32_t result = SCE_FIOS_OK, std::int64_t count = 0);
    std::int32_t sceFiosOpWait(SceFiosOp op);
    std::int32_t sceFiosOpDelete(SceFiosOp op);
    std::int64_t sceFiosOpGetActualCount(SceFiosOp op);

    // ------------------------------------------------------------------------
    // 5. Overlay e Montagem de Arquivos/Pastas
    // ------------------------------------------------------------------------
    std::int32_t sceFiosOverlayAddSync(const std::string& src_path, const std::string& dst_path,
                                       std::int32_t* out_overlay_id, std::int32_t order = 0);
    std::int32_t sceFiosOverlayAddSync(SceFiosOp* op, std::int32_t* out_overlay_id,
                                       const char* src_path, const char* dst_path,
                                       std::int32_t order = 0);

    std::int32_t sceFiosOverlayRemoveSync(std::int32_t overlay_id);
    std::int32_t sceFiosOverlayRemoveSync(SceFiosOp* op, std::int32_t overlay_id);

    std::int32_t sceFiosArchiveMountSync(const std::string& archive_path, const std::string& mount_path,
                                         SceFiosFH* out_fh = nullptr, std::uint32_t flags = 0);
    std::int32_t sceFiosArchiveMountSync(SceFiosOp* op, SceFiosFH* out_fh,
                                         const char* archive_path, const char* mount_path,
                                         std::uint32_t flags = 0);

    // Resolução de caminho (aplica overlays e normalização)
    std::string resolve_virtual_path(const std::string& virtual_path) const;

    // Métricas internas
    std::size_t open_file_count() const;
    std::size_t open_dir_count() const;
    std::size_t total_handle_count() const;
    std::size_t overlay_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bb

#endif // BB_FIOS2_HLE_H
