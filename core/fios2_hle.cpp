#include "fios2_hle.h"
#include "binder.h"
#include "save_data.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <atomic>
#include <cctype>

namespace bb {

struct FileHandleEntry {
    SceFiosFH id{0};
    std::string original_path;
    std::string resolved_path;
    std::string host_path;
    std::uint32_t flags{0};
    std::uint32_t mode{0};
    std::uint64_t cursor{0};
    std::uint64_t file_size{0};
    bool is_readonly{true};
    bool is_host_file{false};
    std::unique_ptr<std::fstream> host_stream;
    mutable std::mutex handle_mutex;
};

struct DirHandleEntry {
    SceFiosDH id{0};
    std::string original_path;
    std::string resolved_path;
    std::vector<SceFiosDirEntry> entries;
    std::size_t index{0};
    mutable std::mutex handle_mutex;
};

struct OpEntry {
    SceFiosOp id{0};
    std::atomic<bool> completed{false};
    std::int32_t result{SCE_FIOS_OK};
    std::int64_t actual_count{0};
};

struct OverlayEntry {
    std::int32_t id{0};
    std::string src_path;
    std::string dst_path;
    std::int32_t order{0};
};

struct Fios2Manager::Impl {
    VirtualFileSystem* vfs{nullptr};
    GuestMemory* guest_memory{nullptr};
    mutable std::mutex mutex;

    std::unordered_map<SceFiosFH, std::shared_ptr<FileHandleEntry>> file_handles;
    std::unordered_map<SceFiosDH, std::shared_ptr<DirHandleEntry>> dir_handles;
    std::unordered_map<SceFiosOp, std::shared_ptr<OpEntry>> ops;
    std::vector<OverlayEntry> overlays;

    SceFiosFH next_fh{1};
    SceFiosDH next_dh{1};
    SceFiosOp next_op{1};
    std::int32_t next_overlay_id{1};

    SceFiosFH alloc_fh() {
        for (std::size_t i = 0; i < SCE_FIOS_MAX_HANDLES * 2; ++i) {
            SceFiosFH id = next_fh++;
            if (next_fh <= 0) next_fh = 1;
            if (file_handles.find(id) == file_handles.end()) {
                return id;
            }
        }
        return SCE_FIOS_FH_INVALID;
    }

    SceFiosDH alloc_dh() {
        for (std::size_t i = 0; i < SCE_FIOS_MAX_HANDLES * 2; ++i) {
            SceFiosDH id = next_dh++;
            if (next_dh <= 0) next_dh = 1;
            if (dir_handles.find(id) == dir_handles.end()) {
                return id;
            }
        }
        return SCE_FIOS_DH_INVALID;
    }

    void update_op(SceFiosOp* pOp, std::int64_t result_or_count, std::int32_t err_code) {
        if (!pOp) return;
        std::lock_guard<std::mutex> lock(mutex);
        std::shared_ptr<OpEntry> entry;
        if (*pOp > 0) {
            auto it = ops.find(*pOp);
            if (it != ops.end()) {
                entry = it->second;
            }
        }
        if (!entry) {
            entry = std::make_shared<OpEntry>();
            entry->id = next_op++;
            if (next_op <= 0) next_op = 1;
            ops[entry->id] = entry;
            *pOp = entry->id;
        }
        entry->completed = true;
        entry->result = err_code;
        entry->actual_count = (result_or_count >= 0 ? result_or_count : 0);
    }

    std::string resolve_overlay(const std::string& norm, bool for_write = false) const {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& ov : overlays) {
            const std::string& src = ov.src_path;
            if (norm == src || norm.rfind(src + "/", 0) == 0) {
                std::string rel = norm.substr(src.size());
                std::string candidate = VirtualFileSystem::normalize_path(ov.dst_path + rel);
                if (vfs) {
                    if (vfs->exists(candidate)) {
                        return candidate;
                    }
                    if (for_write) {
                        std::string host_p;
                        bool ro = true;
                        if (vfs->resolve(candidate, host_p, ro) == VfsError::ok && !ro) {
                            return candidate;
                        }
                    }
                }
            }
        }
        return norm;
    }
};

Fios2Manager::Fios2Manager(VirtualFileSystem* vfs, GuestMemory* guest_memory)
    : impl_(std::make_unique<Impl>()) {
    impl_->vfs = vfs;
    impl_->guest_memory = guest_memory;
}

Fios2Manager::~Fios2Manager() = default;

void Fios2Manager::set_vfs(VirtualFileSystem* vfs) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->vfs = vfs;
}

VirtualFileSystem* Fios2Manager::vfs() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->vfs;
}

void Fios2Manager::set_guest_memory(GuestMemory* memory) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->guest_memory = memory;
}

GuestMemory* Fios2Manager::guest_memory() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->guest_memory;
}

// ----------------------------------------------------------------------------
// 1. Gerenciamento de File Handles (SceFiosFH)
// ----------------------------------------------------------------------------

SceFiosFH Fios2Manager::sceFiosFHOpenSync(const std::string& path, std::uint32_t open_flags) {
    return sceFiosFHOpenWithModeSync(path, open_flags, 0);
}

SceFiosFH Fios2Manager::sceFiosFHOpenWithModeSync(const std::string& path, std::uint32_t open_flags, std::uint32_t mode) {
    if (!impl_->vfs) return SCE_FIOS_ERROR_NOT_INITIALIZED;
    if (path.empty() || path.size() > SCE_FIOS_MAX_PATH) return SCE_FIOS_ERROR_INVALID_ARGUMENT;

    std::string norm = VirtualFileSystem::normalize_path(path);
    if (VirtualFileSystem::contains_traversal(norm)) return SCE_FIOS_ERROR_PATH_TRAVERSAL;

    bool is_write = (open_flags & SCE_FIOS_O_WRONLY) != 0 || ((open_flags & SCE_FIOS_O_RDWR) == SCE_FIOS_O_RDWR);
    std::string resolved = impl_->resolve_overlay(norm, is_write);

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->file_handles.size() + impl_->dir_handles.size() >= SCE_FIOS_MAX_HANDLES) {
        return SCE_FIOS_ERROR_TOO_MANY_HANDLES;
    }

    if (is_write) {
        std::string host_path;
        bool is_ro = true;
        VfsError v_err = impl_->vfs->resolve(resolved, host_path, is_ro);
        if (v_err == VfsError::traversal_detected) return SCE_FIOS_ERROR_PATH_TRAVERSAL;
        if (v_err == VfsError::not_mounted) return SCE_FIOS_ERROR_NOT_MOUNTED;
        if (v_err != VfsError::ok) return SCE_FIOS_ERROR_IO;
        if (is_ro) return SCE_FIOS_ERROR_ACCESS_DENIED;

        std::error_code ec;
        bool file_exists = std::filesystem::exists(host_path, ec);
        if (!file_exists) {
            if (!(open_flags & SCE_FIOS_O_CREAT)) {
                return SCE_FIOS_ERROR_NOT_FOUND;
            }
            std::filesystem::path p(host_path);
            if (p.has_parent_path()) {
                std::filesystem::create_directories(p.parent_path(), ec);
            }
            std::ofstream touch(host_path, std::ios::binary | std::ios::trunc);
            if (!touch.is_open()) return SCE_FIOS_ERROR_IO;
        } else {
            if (std::filesystem::is_directory(host_path, ec)) {
                return SCE_FIOS_ERROR_IS_A_DIRECTORY;
            }
        }

        std::ios::openmode om = std::ios::in | std::ios::out | std::ios::binary;
        if (open_flags & SCE_FIOS_O_TRUNC) {
            om |= std::ios::trunc;
        }
        auto stream = std::make_unique<std::fstream>(host_path, om);
        if (!stream->is_open()) {
            return SCE_FIOS_ERROR_IO;
        }

        std::uint64_t fsize = std::filesystem::file_size(host_path, ec);
        std::uint64_t init_cursor = 0;
        if (open_flags & SCE_FIOS_O_APPEND) {
            init_cursor = fsize;
        }

        auto entry = std::make_shared<FileHandleEntry>();
        entry->id = impl_->alloc_fh();
        entry->original_path = path;
        entry->resolved_path = resolved;
        entry->host_path = host_path;
        entry->flags = open_flags;
        entry->mode = mode;
        entry->cursor = init_cursor;
        entry->file_size = fsize;
        entry->is_readonly = false;
        entry->is_host_file = true;
        entry->host_stream = std::move(stream);

        impl_->file_handles[entry->id] = entry;
        return entry->id;
    } else {
        if (!impl_->vfs->exists(resolved)) {
            return SCE_FIOS_ERROR_NOT_FOUND;
        }

        VfsStat st{};
        VfsError st_err = impl_->vfs->stat(resolved, st);
        if (st_err != VfsError::ok) return SCE_FIOS_ERROR_NOT_FOUND;
        if (st.is_directory) return SCE_FIOS_ERROR_IS_A_DIRECTORY;

        std::string host_path;
        bool is_ro = true;
        bool is_host = (impl_->vfs->resolve(resolved, host_path, is_ro) == VfsError::ok);

        auto entry = std::make_shared<FileHandleEntry>();
        entry->id = impl_->alloc_fh();
        entry->original_path = path;
        entry->resolved_path = resolved;
        entry->host_path = is_host ? host_path : "";
        entry->flags = open_flags;
        entry->mode = mode;
        entry->cursor = 0;
        entry->file_size = st.size;
        entry->is_readonly = true;
        entry->is_host_file = is_host;
        entry->host_stream = nullptr;

        impl_->file_handles[entry->id] = entry;
        return entry->id;
    }
}

std::int32_t Fios2Manager::sceFiosFHOpenSync(SceFiosOp* op, SceFiosFH* out_fh, const char* path, std::uint32_t open_flags) {
    return sceFiosFHOpenWithModeSync(op, out_fh, path, open_flags, 0);
}

std::int32_t Fios2Manager::sceFiosFHOpenWithModeSync(SceFiosOp* op, SceFiosFH* out_fh, const char* path, std::uint32_t open_flags, std::uint32_t mode) {
    if (!path || !out_fh) {
        impl_->update_op(op, 0, SCE_FIOS_ERROR_INVALID_ARGUMENT);
        return SCE_FIOS_ERROR_INVALID_ARGUMENT;
    }
    SceFiosFH res = sceFiosFHOpenWithModeSync(path, open_flags, mode);
    if (res > 0) {
        *out_fh = res;
        impl_->update_op(op, 1, SCE_FIOS_OK);
        return SCE_FIOS_OK;
    } else {
        *out_fh = SCE_FIOS_FH_INVALID;
        impl_->update_op(op, 0, res);
        return res;
    }
}

std::int32_t Fios2Manager::sceFiosFHCloseSync(SceFiosFH fh) {
    std::shared_ptr<FileHandleEntry> entry;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->file_handles.find(fh);
        if (it == impl_->file_handles.end()) return SCE_FIOS_ERROR_INVALID_HANDLE;
        entry = it->second;
        impl_->file_handles.erase(it);
    }
    std::lock_guard<std::mutex> hlock(entry->handle_mutex);
    if (entry->host_stream) {
        entry->host_stream->flush();
        entry->host_stream->close();
    }
    return SCE_FIOS_OK;
}

std::int32_t Fios2Manager::sceFiosFHCloseSync(SceFiosOp* op, SceFiosFH fh) {
    std::int32_t res = sceFiosFHCloseSync(fh);
    impl_->update_op(op, res == SCE_FIOS_OK ? 1 : 0, res);
    return res;
}

std::int64_t Fios2Manager::sceFiosFHReadSync(SceFiosFH fh, void* data, std::size_t size) {
    if (size == 0) return 0;
    if (!data) return SCE_FIOS_ERROR_INVALID_ARGUMENT;

    std::shared_ptr<FileHandleEntry> entry;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->file_handles.find(fh);
        if (it == impl_->file_handles.end()) return SCE_FIOS_ERROR_INVALID_HANDLE;
        entry = it->second;
    }
    std::lock_guard<std::mutex> hlock(entry->handle_mutex);

    if (entry->cursor >= entry->file_size) {
        return 0; // EOF
    }

    std::size_t to_read = std::min<std::size_t>(size, static_cast<std::size_t>(entry->file_size - entry->cursor));
    std::size_t bytes_read = 0;

    if (entry->host_stream) {
        entry->host_stream->seekg(static_cast<std::streamoff>(entry->cursor));
        entry->host_stream->read(static_cast<char*>(data), static_cast<std::streamsize>(to_read));
        std::streamsize count = entry->host_stream->gcount();
        if (count < 0) return SCE_FIOS_ERROR_IO;
        bytes_read = static_cast<std::size_t>(count);
    } else {
        VfsError err = impl_->vfs->read_at(entry->resolved_path, entry->cursor, data, to_read, bytes_read);
        if (err != VfsError::ok) return SCE_FIOS_ERROR_IO;
    }

    entry->cursor += bytes_read;
    return static_cast<std::int64_t>(bytes_read);
}

std::int64_t Fios2Manager::sceFiosFHReadSync(SceFiosOp* op, SceFiosFH fh, void* data, std::size_t size) {
    std::int64_t res = sceFiosFHReadSync(fh, data, size);
    std::int32_t err = (res >= 0 ? SCE_FIOS_OK : static_cast<std::int32_t>(res));
    impl_->update_op(op, res, err);
    return res;
}

std::int64_t Fios2Manager::sceFiosFHReadSync(SceFiosFH fh, GuestMemory& memory, std::uint64_t guest_addr, std::size_t size) {
    if (size == 0) return 0;
    constexpr std::size_t kChunkSize = 64 * 1024;
    std::vector<std::uint8_t> chunk(std::min(size, kChunkSize));
    std::size_t remaining = size;
    std::uint64_t curr_addr = guest_addr;
    std::int64_t total_read = 0;

    while (remaining > 0) {
        std::size_t to_read = std::min(remaining, chunk.size());
        std::int64_t n = sceFiosFHReadSync(fh, chunk.data(), to_read);
        if (n <= 0) {
            if (total_read == 0 && n < 0) return n;
            break;
        }
        MemoryError mem_err = memory.store(curr_addr, chunk.data(), static_cast<std::size_t>(n));
        if (mem_err != MemoryError::ok) {
            return SCE_FIOS_ERROR_INVALID_ARGUMENT;
        }
        curr_addr += n;
        remaining -= static_cast<std::size_t>(n);
        total_read += n;
        if (static_cast<std::size_t>(n) < to_read) break;
    }
    return total_read;
}

std::int64_t Fios2Manager::sceFiosFHReadSync(SceFiosOp* op, SceFiosFH fh, GuestMemory& memory, std::uint64_t guest_addr, std::size_t size) {
    std::int64_t res = sceFiosFHReadSync(fh, memory, guest_addr, size);
    std::int32_t err = (res >= 0 ? SCE_FIOS_OK : static_cast<std::int32_t>(res));
    impl_->update_op(op, res, err);
    return res;
}

std::int64_t Fios2Manager::sceFiosFHWriteSync(SceFiosFH fh, const void* data, std::size_t size) {
    if (size == 0) return 0;
    if (!data) return SCE_FIOS_ERROR_INVALID_ARGUMENT;

    std::shared_ptr<FileHandleEntry> entry;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->file_handles.find(fh);
        if (it == impl_->file_handles.end()) return SCE_FIOS_ERROR_INVALID_HANDLE;
        entry = it->second;
    }
    std::lock_guard<std::mutex> hlock(entry->handle_mutex);

    if (entry->is_readonly || !entry->host_stream) {
        return SCE_FIOS_ERROR_ACCESS_DENIED;
    }

    const std::uint64_t prospective_size = (entry->flags & SCE_FIOS_O_APPEND) ? (entry->file_size + size) : std::max(entry->file_size, entry->cursor + size);
    if (SaveDataManager::instance().is_initialized() &&
        !SaveDataManager::instance().check_work_quota(entry->host_path, entry->file_size, prospective_size)) {
        return SCE_FIOS_ERROR_DISK_FULL;
    }

    if (entry->flags & SCE_FIOS_O_APPEND) {
        entry->host_stream->seekp(0, std::ios::end);
        entry->cursor = static_cast<std::uint64_t>(entry->host_stream->tellp());
    } else {
        entry->host_stream->seekp(static_cast<std::streamoff>(entry->cursor));
    }

    entry->host_stream->write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    entry->host_stream->flush();
    if (!entry->host_stream->good()) {
        entry->host_stream->clear();
        return SCE_FIOS_ERROR_IO;
    }

    entry->cursor += size;
    if (entry->cursor > entry->file_size) {
        entry->file_size = entry->cursor;
    }
    return static_cast<std::int64_t>(size);
}

std::int64_t Fios2Manager::sceFiosFHWriteSync(SceFiosOp* op, SceFiosFH fh, const void* data, std::size_t size) {
    std::int64_t res = sceFiosFHWriteSync(fh, data, size);
    std::int32_t err = (res >= 0 ? SCE_FIOS_OK : static_cast<std::int32_t>(res));
    impl_->update_op(op, res, err);
    return res;
}

std::int64_t Fios2Manager::sceFiosFHWriteSync(SceFiosFH fh, const GuestMemory& memory, std::uint64_t guest_addr, std::size_t size) {
    if (size == 0) return 0;
    constexpr std::size_t kChunkSize = 64 * 1024;
    std::vector<std::uint8_t> chunk(std::min(size, kChunkSize));
    std::size_t remaining = size;
    std::uint64_t curr_addr = guest_addr;
    std::int64_t total_written = 0;

    while (remaining > 0) {
        std::size_t to_write = std::min(remaining, chunk.size());
        MemoryError mem_err = memory.load(curr_addr, chunk.data(), to_write);
        if (mem_err != MemoryError::ok) {
            return SCE_FIOS_ERROR_INVALID_ARGUMENT;
        }
        std::int64_t n = sceFiosFHWriteSync(fh, chunk.data(), to_write);
        if (n <= 0) {
            if (total_written == 0 && n < 0) return n;
            break;
        }
        curr_addr += n;
        remaining -= static_cast<std::size_t>(n);
        total_written += n;
        if (static_cast<std::size_t>(n) < to_write) break;
    }
    return total_written;
}

std::int64_t Fios2Manager::sceFiosFHWriteSync(SceFiosOp* op, SceFiosFH fh, const GuestMemory& memory, std::uint64_t guest_addr, std::size_t size) {
    std::int64_t res = sceFiosFHWriteSync(fh, memory, guest_addr, size);
    std::int32_t err = (res >= 0 ? SCE_FIOS_OK : static_cast<std::int32_t>(res));
    impl_->update_op(op, res, err);
    return res;
}

std::int64_t Fios2Manager::sceFiosFHSeek(SceFiosFH fh, std::int64_t offset, std::int32_t whence) {
    std::shared_ptr<FileHandleEntry> entry;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->file_handles.find(fh);
        if (it == impl_->file_handles.end()) return SCE_FIOS_ERROR_INVALID_HANDLE;
        entry = it->second;
    }
    std::lock_guard<std::mutex> hlock(entry->handle_mutex);
    std::int64_t target = 0;
    switch (whence) {
        case SCE_FIOS_SEEK_SET:
            target = offset;
            break;
        case SCE_FIOS_SEEK_CUR:
            target = static_cast<std::int64_t>(entry->cursor) + offset;
            break;
        case SCE_FIOS_SEEK_END:
            target = static_cast<std::int64_t>(entry->file_size) + offset;
            break;
        default:
            return SCE_FIOS_ERROR_INVALID_ARGUMENT;
    }
    if (target < 0) {
        return SCE_FIOS_ERROR_SEEK_OUT_OF_BOUNDS;
    }
    entry->cursor = static_cast<std::uint64_t>(target);
    if (entry->host_stream) {
        entry->host_stream->clear();
        entry->host_stream->seekg(static_cast<std::streamoff>(target));
        entry->host_stream->seekp(static_cast<std::streamoff>(target));
    }
    return target;
}

std::int64_t Fios2Manager::sceFiosFHTell(SceFiosFH fh) {
    std::shared_ptr<FileHandleEntry> entry;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->file_handles.find(fh);
        if (it == impl_->file_handles.end()) return SCE_FIOS_ERROR_INVALID_HANDLE;
        entry = it->second;
    }
    std::lock_guard<std::mutex> hlock(entry->handle_mutex);
    return static_cast<std::int64_t>(entry->cursor);
}

std::int32_t Fios2Manager::sceFiosFHSyncSync(SceFiosFH fh) {
    std::shared_ptr<FileHandleEntry> entry;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->file_handles.find(fh);
        if (it == impl_->file_handles.end()) return SCE_FIOS_ERROR_INVALID_HANDLE;
        entry = it->second;
    }
    std::lock_guard<std::mutex> hlock(entry->handle_mutex);
    if (entry->host_stream) {
        entry->host_stream->flush();
    }
    return SCE_FIOS_OK;
}

std::int32_t Fios2Manager::sceFiosFHSyncSync(SceFiosOp* op, SceFiosFH fh) {
    std::int32_t res = sceFiosFHSyncSync(fh);
    impl_->update_op(op, res == SCE_FIOS_OK ? 1 : 0, res);
    return res;
}

std::int32_t Fios2Manager::sceFiosFHTruncateSync(SceFiosFH fh, std::int64_t length) {
    if (length < 0) return SCE_FIOS_ERROR_INVALID_ARGUMENT;
    std::shared_ptr<FileHandleEntry> entry;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->file_handles.find(fh);
        if (it == impl_->file_handles.end()) return SCE_FIOS_ERROR_INVALID_HANDLE;
        entry = it->second;
    }
    std::lock_guard<std::mutex> hlock(entry->handle_mutex);
    if (entry->is_readonly || !entry->host_stream) return SCE_FIOS_ERROR_ACCESS_DENIED;

    if (SaveDataManager::instance().is_initialized() &&
        !SaveDataManager::instance().check_work_quota(entry->host_path, entry->file_size, length)) {
        return SCE_FIOS_ERROR_DISK_FULL;
    }

    std::error_code ec;
    entry->host_stream->flush();
    std::filesystem::resize_file(entry->host_path, static_cast<std::uintmax_t>(length), ec);
    if (ec) return SCE_FIOS_ERROR_IO;

    entry->file_size = static_cast<std::uint64_t>(length);
    if (entry->cursor > entry->file_size) entry->cursor = entry->file_size;
    return SCE_FIOS_OK;
}

std::int32_t Fios2Manager::sceFiosFHTruncateSync(SceFiosOp* op, SceFiosFH fh, std::int64_t length) {
    std::int32_t res = sceFiosFHTruncateSync(fh, length);
    impl_->update_op(op, res == SCE_FIOS_OK ? length : 0, res);
    return res;
}

std::int32_t Fios2Manager::sceFiosFileTruncateSync(const std::string& path, std::int64_t length) {
    if (length < 0) return SCE_FIOS_ERROR_INVALID_ARGUMENT;
    if (!impl_->vfs) return SCE_FIOS_ERROR_NOT_INITIALIZED;
    if (path.empty() || path.size() > SCE_FIOS_MAX_PATH) return SCE_FIOS_ERROR_INVALID_ARGUMENT;

    std::string norm = VirtualFileSystem::normalize_path(path);
    if (VirtualFileSystem::contains_traversal(norm)) return SCE_FIOS_ERROR_PATH_TRAVERSAL;

    std::string resolved = impl_->resolve_overlay(norm, true);
    std::string host_path;
    bool is_ro = true;
    VfsError v_err = impl_->vfs->resolve(resolved, host_path, is_ro);
    if (v_err == VfsError::traversal_detected) return SCE_FIOS_ERROR_PATH_TRAVERSAL;
    if (v_err == VfsError::not_mounted) return SCE_FIOS_ERROR_NOT_MOUNTED;
    if (v_err != VfsError::ok) return SCE_FIOS_ERROR_IO;
    if (is_ro) return SCE_FIOS_ERROR_ACCESS_DENIED;

    std::error_code ec;
    if (!std::filesystem::exists(host_path, ec)) return SCE_FIOS_ERROR_NOT_FOUND;
    if (std::filesystem::is_directory(host_path, ec)) return SCE_FIOS_ERROR_IS_A_DIRECTORY;

    std::uint64_t cur_sz = std::filesystem::file_size(host_path, ec);
    if (ec) return SCE_FIOS_ERROR_IO;

    if (SaveDataManager::instance().is_initialized() &&
        !SaveDataManager::instance().check_work_quota(host_path, cur_sz, length)) {
        return SCE_FIOS_ERROR_DISK_FULL;
    }

    std::filesystem::resize_file(host_path, static_cast<std::uintmax_t>(length), ec);
    if (ec) return SCE_FIOS_ERROR_IO;
    return SCE_FIOS_OK;
}

std::int32_t Fios2Manager::sceFiosFileTruncateSync(SceFiosOp* op, const char* path, std::int64_t length) {
    if (!path) return SCE_FIOS_ERROR_INVALID_ARGUMENT;
    std::int32_t res = sceFiosFileTruncateSync(path, length);
    impl_->update_op(op, res == SCE_FIOS_OK ? length : 0, res);
    return res;
}

bool Fios2Manager::sceFiosIsValidHandle(SceFiosFH fh) const {
    if (fh <= 0) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->file_handles.find(fh) != impl_->file_handles.end();
}

// ----------------------------------------------------------------------------
// 2. Gerenciamento de Directory Handles (SceFiosDH)
// ----------------------------------------------------------------------------

SceFiosDH Fios2Manager::sceFiosDHOpenSync(const std::string& path) {
    if (!impl_->vfs) return SCE_FIOS_ERROR_NOT_INITIALIZED;
    if (path.empty() || path.size() > SCE_FIOS_MAX_PATH) return SCE_FIOS_ERROR_INVALID_ARGUMENT;

    std::string norm = VirtualFileSystem::normalize_path(path);
    if (VirtualFileSystem::contains_traversal(norm)) return SCE_FIOS_ERROR_PATH_TRAVERSAL;

    std::string resolved = impl_->resolve_overlay(norm, false);

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->file_handles.size() + impl_->dir_handles.size() >= SCE_FIOS_MAX_HANDLES) {
        return SCE_FIOS_ERROR_TOO_MANY_HANDLES;
    }

    VfsStat st{};
    VfsError st_err = impl_->vfs->stat(resolved, st);
    if (st_err != VfsError::ok) return SCE_FIOS_ERROR_NOT_FOUND;
    if (!st.is_directory) return SCE_FIOS_ERROR_NOT_A_DIRECTORY;

    std::string host_path;
    bool is_ro = true;
    VfsError res_err = impl_->vfs->resolve(resolved, host_path, is_ro);
    if (res_err != VfsError::ok) return SCE_FIOS_ERROR_NOT_MOUNTED;

    std::vector<SceFiosDirEntry> entries;
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(host_path, ec)) {
        if (ec) break;
        std::string filename = item.path().filename().string();
        if (filename == "." || filename == "..") continue;

        SceFiosDirEntry dir_entry{};
        std::size_t copy_len = std::min(filename.size(), sizeof(dir_entry.name) - 1);
        std::memcpy(dir_entry.name, filename.data(), copy_len);
        dir_entry.name[copy_len] = '\0';

        bool is_dir = item.is_directory(ec);
        if (is_dir) {
            dir_entry.attributes = SCE_FIOS_ATTR_DIRECTORY;
            dir_entry.size = 0;
        } else {
            dir_entry.attributes = SCE_FIOS_ATTR_REGULAR;
            dir_entry.size = item.file_size(ec);
        }
        if (is_ro) {
            dir_entry.attributes |= SCE_FIOS_ATTR_READONLY;
        }
        entries.push_back(dir_entry);
    }

    std::sort(entries.begin(), entries.end(), [](const SceFiosDirEntry& a, const SceFiosDirEntry& b) {
        return std::strcmp(a.name, b.name) < 0;
    });

    auto entry = std::make_shared<DirHandleEntry>();
    entry->id = impl_->alloc_dh();
    entry->original_path = path;
    entry->resolved_path = resolved;
    entry->entries = std::move(entries);
    entry->index = 0;

    impl_->dir_handles[entry->id] = entry;
    return entry->id;
}

std::int32_t Fios2Manager::sceFiosDHOpenSync(SceFiosOp* op, SceFiosDH* out_dh, const char* path) {
    if (!path || !out_dh) {
        impl_->update_op(op, 0, SCE_FIOS_ERROR_INVALID_ARGUMENT);
        return SCE_FIOS_ERROR_INVALID_ARGUMENT;
    }
    SceFiosDH res = sceFiosDHOpenSync(path);
    if (res > 0) {
        *out_dh = res;
        impl_->update_op(op, 1, SCE_FIOS_OK);
        return SCE_FIOS_OK;
    } else {
        *out_dh = SCE_FIOS_DH_INVALID;
        impl_->update_op(op, 0, res);
        return res;
    }
}

std::int32_t Fios2Manager::sceFiosDHCloseSync(SceFiosDH dh) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->dir_handles.find(dh);
    if (it == impl_->dir_handles.end()) return SCE_FIOS_ERROR_INVALID_HANDLE;
    impl_->dir_handles.erase(it);
    return SCE_FIOS_OK;
}

std::int32_t Fios2Manager::sceFiosDHCloseSync(SceFiosOp* op, SceFiosDH dh) {
    std::int32_t res = sceFiosDHCloseSync(dh);
    impl_->update_op(op, res == SCE_FIOS_OK ? 1 : 0, res);
    return res;
}

bool Fios2Manager::sceFiosIsValidDH(SceFiosDH dh) const {
    if (dh <= 0) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->dir_handles.find(dh) != impl_->dir_handles.end();
}

std::int32_t Fios2Manager::sceFiosDHReadSync(SceFiosDH dh, SceFiosDirEntry* out_entry) {
    if (!out_entry) return SCE_FIOS_ERROR_INVALID_ARGUMENT;

    std::shared_ptr<DirHandleEntry> entry;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->dir_handles.find(dh);
        if (it == impl_->dir_handles.end()) return SCE_FIOS_ERROR_INVALID_HANDLE;
        entry = it->second;
    }
    std::lock_guard<std::mutex> hlock(entry->handle_mutex);
    if (entry->index < entry->entries.size()) {
        *out_entry = entry->entries[entry->index++];
        return 1;
    }
    return 0; // EOF
}

std::int32_t Fios2Manager::sceFiosDHReadSync(SceFiosOp* op, SceFiosDH dh, SceFiosDirEntry* out_entry) {
    std::int32_t res = sceFiosDHReadSync(dh, out_entry);
    std::int32_t err = (res >= 0 ? SCE_FIOS_OK : res);
    impl_->update_op(op, res, err);
    return res;
}

std::int32_t Fios2Manager::sceFiosDHReadSync(SceFiosDH dh, GuestMemory& memory, std::uint64_t guest_addr) {
    SceFiosDirEntry entry{};
    std::int32_t res = sceFiosDHReadSync(dh, &entry);
    if (res <= 0) return res;
    MemoryError mem_err = memory.store(guest_addr, &entry, sizeof(entry));
    if (mem_err != MemoryError::ok) {
        return SCE_FIOS_ERROR_INVALID_ARGUMENT;
    }
    return 1;
}

std::int32_t Fios2Manager::sceFiosDHReadSync(SceFiosOp* op, SceFiosDH dh, GuestMemory& memory, std::uint64_t guest_addr) {
    std::int32_t res = sceFiosDHReadSync(dh, memory, guest_addr);
    std::int32_t err = (res >= 0 ? SCE_FIOS_OK : res);
    impl_->update_op(op, res, err);
    return res;
}

// ----------------------------------------------------------------------------
// 3. Operações síncronas de arquivos
// ----------------------------------------------------------------------------

std::int32_t Fios2Manager::sceFiosStatSync(const std::string& path, SceFiosStat& out_stat) {
    if (!impl_->vfs) return SCE_FIOS_ERROR_NOT_INITIALIZED;
    if (path.empty() || path.size() > SCE_FIOS_MAX_PATH) return SCE_FIOS_ERROR_INVALID_ARGUMENT;

    std::string norm = VirtualFileSystem::normalize_path(path);
    if (VirtualFileSystem::contains_traversal(norm)) return SCE_FIOS_ERROR_PATH_TRAVERSAL;

    std::string resolved = impl_->resolve_overlay(norm, false);
    VfsStat st{};
    VfsError err = impl_->vfs->stat(resolved, st);
    if (err == VfsError::not_found) return SCE_FIOS_ERROR_NOT_FOUND;
    if (err == VfsError::access_denied) return SCE_FIOS_ERROR_ACCESS_DENIED;
    if (err == VfsError::traversal_detected) return SCE_FIOS_ERROR_PATH_TRAVERSAL;
    if (err != VfsError::ok) return SCE_FIOS_ERROR_IO;

    out_stat = SceFiosStat{};
    out_stat.size = st.size;
    out_stat.attributes = (st.is_directory ? SCE_FIOS_ATTR_DIRECTORY : SCE_FIOS_ATTR_REGULAR);
    if (st.is_readonly) {
        out_stat.attributes |= SCE_FIOS_ATTR_READONLY;
    }

    std::string host_path;
    bool ro = true;
    if (impl_->vfs->resolve(resolved, host_path, ro) == VfsError::ok) {
        std::error_code ec;
        auto ftime = std::filesystem::last_write_time(host_path, ec);
        if (!ec) {
            auto now_sys = std::chrono::system_clock::now();
            auto now_fs = std::filesystem::file_time_type::clock::now();
            auto sys_time = now_sys + std::chrono::duration_cast<std::chrono::system_clock::duration>(ftime - now_fs);
            auto sec = std::chrono::duration_cast<std::chrono::seconds>(sys_time.time_since_epoch()).count();
            out_stat.modify_time = static_cast<std::uint64_t>(sec > 0 ? sec : 0);
            out_stat.access_time = out_stat.modify_time;
            out_stat.create_time = out_stat.modify_time;
        }
    }
    return SCE_FIOS_OK;
}

std::int32_t Fios2Manager::sceFiosStatSync(SceFiosOp* op, const char* path, SceFiosStat* out_stat) {
    if (!path || !out_stat) {
        impl_->update_op(op, 0, SCE_FIOS_ERROR_INVALID_ARGUMENT);
        return SCE_FIOS_ERROR_INVALID_ARGUMENT;
    }
    std::int32_t res = sceFiosStatSync(path, *out_stat);
    impl_->update_op(op, res == SCE_FIOS_OK ? 1 : 0, res);
    return res;
}

std::int32_t Fios2Manager::sceFiosStatSync(const std::string& path, GuestMemory& memory, std::uint64_t guest_addr) {
    SceFiosStat st{};
    std::int32_t res = sceFiosStatSync(path, st);
    if (res != SCE_FIOS_OK) return res;
    MemoryError mem_err = memory.store(guest_addr, &st, sizeof(st));
    if (mem_err != MemoryError::ok) {
        return SCE_FIOS_ERROR_INVALID_ARGUMENT;
    }
    return SCE_FIOS_OK;
}

std::int32_t Fios2Manager::sceFiosStatSync(SceFiosOp* op, const char* path, GuestMemory& memory, std::uint64_t guest_addr) {
    if (!path) {
        impl_->update_op(op, 0, SCE_FIOS_ERROR_INVALID_ARGUMENT);
        return SCE_FIOS_ERROR_INVALID_ARGUMENT;
    }
    std::int32_t res = sceFiosStatSync(path, memory, guest_addr);
    impl_->update_op(op, res == SCE_FIOS_OK ? 1 : 0, res);
    return res;
}

std::int32_t Fios2Manager::sceFiosFileDeleteSync(const std::string& path) {
    if (!impl_->vfs) return SCE_FIOS_ERROR_NOT_INITIALIZED;
    if (path.empty() || path.size() > SCE_FIOS_MAX_PATH) return SCE_FIOS_ERROR_INVALID_ARGUMENT;

    std::string norm = VirtualFileSystem::normalize_path(path);
    if (VirtualFileSystem::contains_traversal(norm)) return SCE_FIOS_ERROR_PATH_TRAVERSAL;

    std::string resolved = impl_->resolve_overlay(norm, true);
    VfsError err = impl_->vfs->remove_file(resolved);
    if (err == VfsError::ok) return SCE_FIOS_OK;
    if (err == VfsError::not_found) return SCE_FIOS_ERROR_NOT_FOUND;
    if (err == VfsError::access_denied) return SCE_FIOS_ERROR_ACCESS_DENIED;
    if (err == VfsError::traversal_detected) return SCE_FIOS_ERROR_PATH_TRAVERSAL;
    return SCE_FIOS_ERROR_IO;
}

std::int32_t Fios2Manager::sceFiosFileDeleteSync(SceFiosOp* op, const char* path) {
    if (!path) {
        impl_->update_op(op, 0, SCE_FIOS_ERROR_INVALID_ARGUMENT);
        return SCE_FIOS_ERROR_INVALID_ARGUMENT;
    }
    std::int32_t res = sceFiosFileDeleteSync(path);
    impl_->update_op(op, res == SCE_FIOS_OK ? 1 : 0, res);
    return res;
}

std::int32_t Fios2Manager::sceFiosDirectoryCreate(const std::string& path) {
    if (!impl_->vfs) return SCE_FIOS_ERROR_NOT_INITIALIZED;
    if (path.empty() || path.size() > SCE_FIOS_MAX_PATH) return SCE_FIOS_ERROR_INVALID_ARGUMENT;

    std::string norm = VirtualFileSystem::normalize_path(path);
    if (VirtualFileSystem::contains_traversal(norm)) return SCE_FIOS_ERROR_PATH_TRAVERSAL;

    std::string resolved = impl_->resolve_overlay(norm, true);
    std::string host_path;
    bool is_ro = true;
    VfsError err = impl_->vfs->resolve(resolved, host_path, is_ro);
    if (err != VfsError::ok) return SCE_FIOS_ERROR_NOT_MOUNTED;
    if (is_ro) return SCE_FIOS_ERROR_ACCESS_DENIED;

    std::error_code ec;
    std::filesystem::create_directories(host_path, ec);
    if (ec) return SCE_FIOS_ERROR_IO;
    return SCE_FIOS_OK;
}

std::int32_t Fios2Manager::sceFiosDirectoryCreate(SceFiosOp* op, const char* path) {
    if (!path) {
        impl_->update_op(op, 0, SCE_FIOS_ERROR_INVALID_ARGUMENT);
        return SCE_FIOS_ERROR_INVALID_ARGUMENT;
    }
    std::int32_t res = sceFiosDirectoryCreate(path);
    impl_->update_op(op, res == SCE_FIOS_OK ? 1 : 0, res);
    return res;
}

std::int32_t Fios2Manager::sceFiosRenameSync(const std::string& old_path, const std::string& new_path) {
    if (!impl_->vfs) return SCE_FIOS_ERROR_NOT_INITIALIZED;
    if (old_path.empty() || new_path.empty()) return SCE_FIOS_ERROR_INVALID_ARGUMENT;
    if (old_path.size() > SCE_FIOS_MAX_PATH || new_path.size() > SCE_FIOS_MAX_PATH) {
        return SCE_FIOS_ERROR_INVALID_ARGUMENT;
    }

    std::string old_norm = VirtualFileSystem::normalize_path(old_path);
    std::string new_norm = VirtualFileSystem::normalize_path(new_path);
    if (VirtualFileSystem::contains_traversal(old_norm) || VirtualFileSystem::contains_traversal(new_norm)) {
        return SCE_FIOS_ERROR_PATH_TRAVERSAL;
    }

    std::string old_res = impl_->resolve_overlay(old_norm, true);
    std::string new_res = impl_->resolve_overlay(new_norm, true);

    std::string old_host;
    bool old_ro = true;
    VfsError err_old = impl_->vfs->resolve(old_res, old_host, old_ro);
    if (err_old != VfsError::ok) return SCE_FIOS_ERROR_NOT_MOUNTED;
    if (old_ro) return SCE_FIOS_ERROR_ACCESS_DENIED;

    std::string new_host;
    bool new_ro = true;
    VfsError err_new = impl_->vfs->resolve(new_res, new_host, new_ro);
    if (err_new != VfsError::ok) return SCE_FIOS_ERROR_NOT_MOUNTED;
    if (new_ro) return SCE_FIOS_ERROR_ACCESS_DENIED;

    std::error_code ec;
    if (!std::filesystem::exists(old_host, ec)) return SCE_FIOS_ERROR_NOT_FOUND;

    std::filesystem::path new_p(new_host);
    if (new_p.has_parent_path()) {
        std::filesystem::create_directories(new_p.parent_path(), ec);
        if (ec) return SCE_FIOS_ERROR_IO;
    }

    std::filesystem::path bak_host;
    bool has_existing = std::filesystem::exists(new_host, ec);
    if (has_existing) {
        auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
        bak_host = new_p.string() + ".fios_bak." + std::to_string(now_ns);
        std::filesystem::rename(new_host, bak_host, ec);
        if (ec) return SCE_FIOS_ERROR_IO;
    }

    std::filesystem::rename(old_host, new_host, ec);
    if (ec) {
        if (has_existing) {
            std::error_code r_ec;
            std::filesystem::rename(bak_host, new_host, r_ec);
        }
        return SCE_FIOS_ERROR_IO;
    }

    if (has_existing) {
        std::filesystem::remove(bak_host, ec);
    }
    return SCE_FIOS_OK;
}

std::int32_t Fios2Manager::sceFiosRenameSync(SceFiosOp* op, const char* old_path, const char* new_path) {
    if (!old_path || !new_path) {
        impl_->update_op(op, 0, SCE_FIOS_ERROR_INVALID_ARGUMENT);
        return SCE_FIOS_ERROR_INVALID_ARGUMENT;
    }
    std::int32_t res = sceFiosRenameSync(old_path, new_path);
    impl_->update_op(op, res == SCE_FIOS_OK ? 1 : 0, res);
    return res;
}

// ----------------------------------------------------------------------------
// 4. Operações assíncronas / Ops
// ----------------------------------------------------------------------------

SceFiosOp Fios2Manager::sceFiosOpCreate(std::int32_t result, std::int64_t count) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto op = std::make_shared<OpEntry>();
    op->id = impl_->next_op++;
    if (impl_->next_op <= 0) impl_->next_op = 1;
    op->completed = true;
    op->result = result;
    op->actual_count = count;
    impl_->ops[op->id] = op;
    return op->id;
}

std::int32_t Fios2Manager::sceFiosOpWait(SceFiosOp op) {
    std::shared_ptr<OpEntry> entry;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->ops.find(op);
        if (it == impl_->ops.end()) return SCE_FIOS_ERROR_INVALID_HANDLE;
        entry = it->second;
    }
    return entry->result;
}

std::int32_t Fios2Manager::sceFiosOpDelete(SceFiosOp op) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->ops.find(op);
    if (it == impl_->ops.end()) return SCE_FIOS_ERROR_INVALID_HANDLE;
    impl_->ops.erase(it);
    return SCE_FIOS_OK;
}

std::int64_t Fios2Manager::sceFiosOpGetActualCount(SceFiosOp op) {
    std::shared_ptr<OpEntry> entry;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->ops.find(op);
        if (it == impl_->ops.end()) return -1;
        entry = it->second;
    }
    return entry->actual_count;
}

// ----------------------------------------------------------------------------
// 5. Overlay e Montagem de Arquivos/Pastas
// ----------------------------------------------------------------------------

std::int32_t Fios2Manager::sceFiosOverlayAddSync(const std::string& src_path, const std::string& dst_path,
                                                 std::int32_t* out_overlay_id, std::int32_t order) {
    if (src_path.empty() || dst_path.empty()) return SCE_FIOS_ERROR_INVALID_ARGUMENT;
    std::string norm_src = VirtualFileSystem::normalize_path(src_path);
    std::string norm_dst = VirtualFileSystem::normalize_path(dst_path);
    if (VirtualFileSystem::contains_traversal(norm_src) || VirtualFileSystem::contains_traversal(norm_dst)) {
        return SCE_FIOS_ERROR_PATH_TRAVERSAL;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    OverlayEntry ov{};
    ov.id = impl_->next_overlay_id++;
    if (impl_->next_overlay_id <= 0) impl_->next_overlay_id = 1;
    ov.src_path = norm_src;
    ov.dst_path = norm_dst;
    ov.order = order;

    impl_->overlays.push_back(ov);
    std::stable_sort(impl_->overlays.begin(), impl_->overlays.end(), [](const OverlayEntry& a, const OverlayEntry& b) {
        return a.order > b.order;
    });

    if (out_overlay_id) {
        *out_overlay_id = ov.id;
    }
    return SCE_FIOS_OK;
}

std::int32_t Fios2Manager::sceFiosOverlayAddSync(SceFiosOp* op, std::int32_t* out_overlay_id,
                                                 const char* src_path, const char* dst_path,
                                                 std::int32_t order) {
    if (!src_path || !dst_path) {
        impl_->update_op(op, 0, SCE_FIOS_ERROR_INVALID_ARGUMENT);
        return SCE_FIOS_ERROR_INVALID_ARGUMENT;
    }
    std::int32_t res = sceFiosOverlayAddSync(src_path, dst_path, out_overlay_id, order);
    impl_->update_op(op, res == SCE_FIOS_OK ? 1 : 0, res);
    return res;
}

std::int32_t Fios2Manager::sceFiosOverlayRemoveSync(std::int32_t overlay_id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = std::remove_if(impl_->overlays.begin(), impl_->overlays.end(), [overlay_id](const OverlayEntry& o) {
        return o.id == overlay_id;
    });
    if (it == impl_->overlays.end()) {
        return SCE_FIOS_ERROR_INVALID_HANDLE;
    }
    impl_->overlays.erase(it, impl_->overlays.end());
    return SCE_FIOS_OK;
}

std::int32_t Fios2Manager::sceFiosOverlayRemoveSync(SceFiosOp* op, std::int32_t overlay_id) {
    std::int32_t res = sceFiosOverlayRemoveSync(overlay_id);
    impl_->update_op(op, res == SCE_FIOS_OK ? 1 : 0, res);
    return res;
}

std::int32_t Fios2Manager::sceFiosArchiveMountSync(const std::string& archive_path, const std::string& mount_path,
                                                   SceFiosFH* out_fh, std::uint32_t flags) {
    (void)flags;
    if (!impl_->vfs) return SCE_FIOS_ERROR_NOT_INITIALIZED;
    if (archive_path.empty() || mount_path.empty()) return SCE_FIOS_ERROR_INVALID_ARGUMENT;

    std::string norm_arc = VirtualFileSystem::normalize_path(archive_path);
    std::string norm_mnt = VirtualFileSystem::normalize_path(mount_path);
    if (VirtualFileSystem::contains_traversal(norm_arc) || VirtualFileSystem::contains_traversal(norm_mnt)) {
        return SCE_FIOS_ERROR_PATH_TRAVERSAL;
    }

    std::string host_arc;
    bool is_ro = true;
    VfsError v_err = impl_->vfs->resolve(norm_arc, host_arc, is_ro);
    if (v_err != VfsError::ok) {
        return SCE_FIOS_ERROR_NOT_FOUND;
    }

    std::error_code ec;
    bool exists = std::filesystem::exists(host_arc, ec);
    if (!exists) {
        return SCE_FIOS_ERROR_NOT_FOUND;
    }

    if (std::filesystem::is_directory(host_arc, ec)) {
        bool ok = impl_->vfs->mount(norm_mnt, host_arc, true);
        if (!ok) return SCE_FIOS_ERROR_IO;
    } else {
        std::string lower_arc = host_arc;
        for (char& c : lower_arc) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        if (lower_arc.rfind(".bhd5") != std::string::npos || lower_arc.rfind(".bhd") != std::string::npos) {
            auto reader = std::make_shared<Bhd5Reader>();
            BinderError berr = reader->open(host_arc);
            if (berr != BinderError::ok) return SCE_FIOS_ERROR_IO;
            bool ok = impl_->vfs->mount_bhd5(norm_mnt, reader);
            if (!ok) return SCE_FIOS_ERROR_IO;
        } else if (lower_arc.rfind(".bnd4") != std::string::npos || lower_arc.rfind(".bnd") != std::string::npos) {
            auto reader = std::make_shared<Bnd4Reader>();
            BinderError berr = reader->open(host_arc);
            if (berr != BinderError::ok) return SCE_FIOS_ERROR_IO;
            bool ok = impl_->vfs->mount_bnd4(norm_mnt, reader);
            if (!ok) return SCE_FIOS_ERROR_IO;
        } else {
            return SCE_FIOS_ERROR_INVALID_ARGUMENT;
        }
    }

    if (out_fh) {
        *out_fh = sceFiosFHOpenSync(archive_path, SCE_FIOS_O_RDONLY);
    }
    return SCE_FIOS_OK;
}

std::int32_t Fios2Manager::sceFiosArchiveMountSync(SceFiosOp* op, SceFiosFH* out_fh,
                                                   const char* archive_path, const char* mount_path,
                                                   std::uint32_t flags) {
    if (!archive_path || !mount_path) {
        impl_->update_op(op, 0, SCE_FIOS_ERROR_INVALID_ARGUMENT);
        return SCE_FIOS_ERROR_INVALID_ARGUMENT;
    }
    std::int32_t res = sceFiosArchiveMountSync(archive_path, mount_path, out_fh, flags);
    impl_->update_op(op, res == SCE_FIOS_OK ? 1 : 0, res);
    return res;
}

std::string Fios2Manager::resolve_virtual_path(const std::string& virtual_path) const {
    if (virtual_path.empty()) return "/";
    std::string norm = VirtualFileSystem::normalize_path(virtual_path);
    return impl_->resolve_overlay(norm, false);
}

std::size_t Fios2Manager::open_file_count() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->file_handles.size();
}

std::size_t Fios2Manager::open_dir_count() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->dir_handles.size();
}

std::size_t Fios2Manager::total_handle_count() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->file_handles.size() + impl_->dir_handles.size();
}

std::size_t Fios2Manager::overlay_count() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->overlays.size();
}

} // namespace bb
