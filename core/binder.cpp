#include "binder.h"
#include "miniz/miniz_tinfl.h"

#include <cstring>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace bb {

const char *binder_error_string(BinderError err) {
    switch (err) {
        case BinderError::ok: return "ok";
        case BinderError::file_not_found: return "file_not_found";
        case BinderError::invalid_magic: return "invalid_magic";
        case BinderError::invalid_header: return "invalid_header";
        case BinderError::decompression_failed: return "decompression_failed";
        case BinderError::unsupported_algorithm: return "unsupported_algorithm";
        case BinderError::io_error: return "io_error";
        case BinderError::out_of_bounds: return "out_of_bounds";
        case BinderError::corrupted_data: return "corrupted_data";
        case BinderError::not_open: return "not_open";
        default: return "unknown_error";
    }
}

// Helpers para leitura e escrita de inteiros com endianness explícito
static inline std::uint32_t read_be32(const std::uint8_t *p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

static inline void write_be32(std::uint8_t *p, std::uint32_t val) {
    p[0] = static_cast<std::uint8_t>((val >> 24) & 0xFF);
    p[1] = static_cast<std::uint8_t>((val >> 16) & 0xFF);
    p[2] = static_cast<std::uint8_t>((val >> 8) & 0xFF);
    p[3] = static_cast<std::uint8_t>(val & 0xFF);
}

static inline std::uint32_t read_le32(const std::uint8_t *p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

static inline std::uint64_t read_le64(const std::uint8_t *p) {
    return static_cast<std::uint64_t>(read_le32(p)) |
           (static_cast<std::uint64_t>(read_le32(p + 4)) << 32);
}

static std::uint32_t compute_adler32(const std::uint8_t *data, std::size_t len) {
    std::uint32_t a = 1, b = 0;
    for (std::size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

static std::string utf16le_to_utf8(const std::uint8_t *p, std::size_t max_bytes) {
    std::string out;
    std::size_t i = 0;
    while (i + 1 < max_bytes) {
        std::uint16_t ch = static_cast<std::uint16_t>(p[i]) | (static_cast<std::uint16_t>(p[i + 1]) << 8);
        if (ch == 0) break;
        if (ch < 0x80) {
            out.push_back(static_cast<char>(ch));
        } else if (ch < 0x800) {
            out.push_back(static_cast<char>(0xC0 | ((ch >> 6) & 0x1F)));
            out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | ((ch >> 12) & 0x0F)));
            out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        }
        i += 2;
    }
    return out;
}

// ============================================================================
// 1. DCX DECOMPRESSOR IMPLEMENTATION
// ============================================================================

bool DcxDecompressor::is_dcx(const void *data, std::size_t size) {
    if (!data || size < 0x4C) return false;
    return std::memcmp(data, "DCX\0", 4) == 0;
}

bool DcxDecompressor::is_dcx_file(const std::string &path) {
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    if (!file.is_open()) return false;
    char magic[4]{0};
    file.read(magic, 4);
    return file.gcount() == 4 && std::memcmp(magic, "DCX\0", 4) == 0;
}

BinderError DcxDecompressor::parse_header(const void *data, std::size_t size, DcxHeader &out_header) {
    if (!data || size < 0x4C) {
        return BinderError::out_of_bounds;
    }
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    if (std::memcmp(bytes, "DCX\0", 4) != 0) {
        return BinderError::invalid_magic;
    }

    out_header.version = read_be32(bytes + 0x04);
    out_header.dcs_offset = read_be32(bytes + 0x08);
    out_header.dcp_offset = read_be32(bytes + 0x0C);
    out_header.dca_offset = read_be32(bytes + 0x10);
    out_header.data_offset = read_be32(bytes + 0x14);

    // Validação estrita de limites dos offsets
    if (out_header.dcs_offset >= size || out_header.dcp_offset >= size ||
        out_header.dca_offset >= size || out_header.data_offset > size) {
        return BinderError::corrupted_data;
    }

    // Bloco DCS: Uncompressed Size e Compressed Size
    if (out_header.dcs_offset + 12 > size) return BinderError::corrupted_data;
    const auto *dcs = bytes + out_header.dcs_offset;
    if (std::memcmp(dcs, "DCS\0", 4) != 0) return BinderError::invalid_header;
    out_header.uncompressed_size = read_be32(dcs + 0x04);
    out_header.compressed_size = read_be32(dcs + 0x08);

    // Bloco DCP: Algoritmo de Compressão
    if (out_header.dcp_offset + 8 > size) return BinderError::corrupted_data;
    const auto *dcp = bytes + out_header.dcp_offset;
    if (std::memcmp(dcp, "DCP\0", 4) != 0) return BinderError::invalid_header;
    std::memcpy(out_header.algorithm_tag, dcp + 0x04, 4);
    out_header.algorithm_tag[4] = '\0';

    if (std::strcmp(out_header.algorithm_tag, "DFLT") == 0 ||
        std::strcmp(out_header.algorithm_tag, "EDGE") == 0 ||
        std::strcmp(out_header.algorithm_tag, "ZLIB") == 0) {
        out_header.algorithm = DcxAlgorithm::deflate;
    } else if (std::strcmp(out_header.algorithm_tag, "KRAK") == 0) {
        out_header.algorithm = DcxAlgorithm::kraken;
    } else {
        out_header.algorithm = DcxAlgorithm::unknown;
    }

    // Bloco DCA: Cabeçalho de Dados
    if (out_header.dca_offset + 8 > size) return BinderError::corrupted_data;
    const auto *dca = bytes + out_header.dca_offset;
    if (std::memcmp(dca, "DCA\0", 4) != 0) return BinderError::invalid_header;

    return BinderError::ok;
}

BinderError DcxDecompressor::decompress(const void *in_data, std::size_t in_size, std::vector<std::uint8_t> &out_data) {
    DcxHeader header{};
    BinderError err = parse_header(in_data, in_size, header);
    if (err != BinderError::ok) return err;

    if (header.data_offset >= in_size) {
        return BinderError::corrupted_data;
    }

    std::size_t payload_size = in_size - header.data_offset;
    if (header.compressed_size > 0 && header.compressed_size < payload_size) {
        payload_size = header.compressed_size;
    }

    const auto *src = static_cast<const std::uint8_t *>(in_data) + header.data_offset;

    if (header.algorithm == DcxAlgorithm::deflate) {
        out_data.resize(header.uncompressed_size);
        if (header.uncompressed_size == 0) {
            return BinderError::ok;
        }

        // Tenta descompressão com parsing de cabeçalho zlib padrão
        std::size_t res = tinfl_decompress_mem_to_mem(
            out_data.data(), out_data.size(),
            src, payload_size,
            TINFL_FLAG_PARSE_ZLIB_HEADER);

        // Se falhar, tenta como raw Deflate sem cabeçalho zlib
        if (res == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
            res = tinfl_decompress_mem_to_mem(
                out_data.data(), out_data.size(),
                src, payload_size,
                0);
        }

        if (res != header.uncompressed_size) {
            out_data.clear();
            return BinderError::decompression_failed;
        }
        return BinderError::ok;
    }

    if (header.algorithm == DcxAlgorithm::kraken) {
        return BinderError::unsupported_algorithm;
    }

    return BinderError::unsupported_algorithm;
}

BinderError DcxDecompressor::decompress_file(const std::string &path, std::vector<std::uint8_t> &out_data) {
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
    if (!file.is_open()) return BinderError::file_not_found;

    auto size = file.tellg();
    if (size < 0x4C) return BinderError::out_of_bounds;

    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char *>(buffer.data()), size)) {
        return BinderError::io_error;
    }

    return decompress(buffer.data(), buffer.size(), out_data);
}

BinderError DcxDecompressor::create_dcx(const void *in_data, std::size_t in_size, std::vector<std::uint8_t> &out_dcx, DcxAlgorithm algo) {
    if (algo != DcxAlgorithm::deflate) {
        return BinderError::unsupported_algorithm;
    }
    const auto *src = static_cast<const std::uint8_t *>(in_data);
    std::uint32_t adler = compute_adler32(src, in_size);

    // Constrói payload com cabeçalho zlib e blocos Deflate stored (máx 65535 bytes por bloco)
    std::vector<std::uint8_t> payload;
    payload.reserve(in_size + 64);
    // Cabeçalho zlib: CM=8 (Deflate), CINFO=7 (32K), FCHECK para (0x7800 + 0x01) % 31 == 0
    payload.push_back(0x78);
    payload.push_back(0x01);

    std::size_t remaining = in_size;
    std::size_t offset = 0;
    if (in_size == 0) {
        payload.push_back(0x01); // BFINAL=1, BTYPE=00
        payload.push_back(0x00); payload.push_back(0x00);
        payload.push_back(0xFF); payload.push_back(0xFF);
    } else {
        while (remaining > 0) {
            std::size_t chunk = std::min<std::size_t>(remaining, 65535);
            bool is_final = (chunk == remaining);
            std::uint8_t header_byte = is_final ? 0x01 : 0x00;
            payload.push_back(header_byte);

            auto len = static_cast<std::uint16_t>(chunk);
            auto nlen = static_cast<std::uint16_t>(~len);
            payload.push_back(static_cast<std::uint8_t>(len & 0xFF));
            payload.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFF));
            payload.push_back(static_cast<std::uint8_t>(nlen & 0xFF));
            payload.push_back(static_cast<std::uint8_t>((nlen >> 8) & 0xFF));

            payload.insert(payload.end(), src + offset, src + offset + chunk);
            offset += chunk;
            remaining -= chunk;
        }
    }

    // Trailer Adler-32 em big-endian
    payload.push_back(static_cast<std::uint8_t>((adler >> 24) & 0xFF));
    payload.push_back(static_cast<std::uint8_t>((adler >> 16) & 0xFF));
    payload.push_back(static_cast<std::uint8_t>((adler >> 8) & 0xFF));
    payload.push_back(static_cast<std::uint8_t>(adler & 0xFF));

    // Monta cabeçalhos DCX
    out_dcx.resize(0x4C + payload.size(), 0);
    std::memcpy(out_dcx.data(), "DCX\0", 4);
    write_be32(out_dcx.data() + 0x04, 0x00010000); // Version
    write_be32(out_dcx.data() + 0x08, 0x18);       // dcs_offset
    write_be32(out_dcx.data() + 0x0C, 0x24);       // dcp_offset
    write_be32(out_dcx.data() + 0x10, 0x44);       // dca_offset
    write_be32(out_dcx.data() + 0x14, 0x4C);       // data_offset

    // DCS
    std::memcpy(out_dcx.data() + 0x18, "DCS\0", 4);
    write_be32(out_dcx.data() + 0x1C, static_cast<std::uint32_t>(in_size));
    write_be32(out_dcx.data() + 0x20, static_cast<std::uint32_t>(payload.size()));

    // DCP
    std::memcpy(out_dcx.data() + 0x24, "DCP\0", 4);
    std::memcpy(out_dcx.data() + 0x28, "DFLT", 4);
    write_be32(out_dcx.data() + 0x2C, 0x20); // dcp header size
    out_dcx[0x30] = 0x09;                    // compression level 9

    // DCA
    std::memcpy(out_dcx.data() + 0x44, "DCA\0", 4);
    write_be32(out_dcx.data() + 0x48, 0x08);

    // Dados comprimidos
    std::memcpy(out_dcx.data() + 0x4C, payload.data(), payload.size());
    return BinderError::ok;
}

// ============================================================================
// 2. BHD5 / BDT READER IMPLEMENTATION
// ============================================================================

struct Bhd5Reader::Impl {
    std::string bhd_path;
    std::string bdt_path;
    std::ifstream bdt_stream;
    mutable std::mutex bdt_mutex;
    std::vector<Bhd5FileEntry> entries;
    std::unordered_map<std::uint64_t, std::size_t> hash_to_index;
    std::size_t bucket_count{0};
    bool is_open{false};
};

Bhd5Reader::Bhd5Reader() : impl_(std::make_unique<Impl>()) {}
Bhd5Reader::~Bhd5Reader() { close(); }

std::string Bhd5Reader::normalize_archive_path(const std::string &path) {
    std::string norm;
    norm.reserve(path.size());
    for (char c : path) {
        if (c == '/') {
            norm.push_back('\\');
        } else {
            norm.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return norm;
}

std::uint64_t Bhd5Reader::hash_path(const std::string &path, std::uint64_t prime) {
    std::string norm = normalize_archive_path(path);
    std::uint64_t hash = 0;
    for (char c : norm) {
        hash = hash * prime + static_cast<std::uint8_t>(c);
    }
    return hash;
}

std::uint32_t Bhd5Reader::hash_path_32(const std::string &path, std::uint32_t prime) {
    std::string norm = normalize_archive_path(path);
    std::uint32_t hash = 0;
    for (char c : norm) {
        hash = hash * prime + static_cast<std::uint8_t>(c);
    }
    return hash;
}

void Bhd5Reader::close() {
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->bdt_mutex);
        if (impl_->bdt_stream.is_open()) {
            impl_->bdt_stream.close();
        }
        impl_->entries.clear();
        impl_->hash_to_index.clear();
        impl_->bucket_count = 0;
        impl_->is_open = false;
    }
}

bool Bhd5Reader::is_open() const {
    return impl_ && impl_->is_open;
}

std::size_t Bhd5Reader::file_count() const {
    return impl_ ? impl_->entries.size() : 0;
}

std::size_t Bhd5Reader::bucket_count() const {
    return impl_ ? impl_->bucket_count : 0;
}

BinderError Bhd5Reader::open(const std::string &bhd_path, const std::string &bdt_path) {
    close();

    std::ifstream bhd_file(std::filesystem::u8path(bhd_path), std::ios::binary | std::ios::ate);
    if (!bhd_file.is_open()) return BinderError::file_not_found;

    auto bhd_size = bhd_file.tellg();
    if (bhd_size < 24) return BinderError::invalid_header;

    std::vector<std::uint8_t> bhd_data(static_cast<std::size_t>(bhd_size));
    bhd_file.seekg(0, std::ios::beg);
    if (!bhd_file.read(reinterpret_cast<char *>(bhd_data.data()), bhd_size)) {
        return BinderError::io_error;
    }

    std::string target_bdt = bdt_path;
    if (target_bdt.empty()) {
        // Tenta deduzir o caminho do .bdt trocando a extensão do .bhd5
        if (bhd_path.size() >= 5 && bhd_path.substr(bhd_path.size() - 5) == ".bhd5") {
            target_bdt = bhd_path.substr(0, bhd_path.size() - 5) + ".bdt";
        } else if (bhd_path.size() >= 4 && bhd_path.substr(bhd_path.size() - 4) == ".bhd") {
            target_bdt = bhd_path.substr(0, bhd_path.size() - 4) + ".bdt";
        }
    }

    return load_header(bhd_data.data(), bhd_data.size(), target_bdt);
}

BinderError Bhd5Reader::load_header(const void *bhd_data, std::size_t bhd_size, const std::string &bdt_path) {
    close();
    if (!bhd_data || bhd_size < 24) return BinderError::invalid_header;

    const auto *bytes = static_cast<const std::uint8_t *>(bhd_data);
    if (std::memcmp(bytes, "BHD5", 4) != 0) {
        return BinderError::invalid_magic;
    }

    std::uint32_t bucket_count = read_le32(bytes + 12);
    std::uint32_t bucket_offset = read_le32(bytes + 16);

    if (bucket_count > 1000000 || bucket_offset < 16 || bucket_offset >= bhd_size) {
        return BinderError::invalid_header;
    }

    if (bucket_offset + static_cast<std::uint64_t>(bucket_count) * 8 > bhd_size) {
        return BinderError::invalid_header;
    }

    impl_->bucket_count = bucket_count;
    impl_->entries.reserve(bucket_count * 2);

    for (std::uint32_t b = 0; b < bucket_count; ++b) {
        const std::uint8_t *b_ptr = bytes + bucket_offset + b * 8;
        std::uint32_t count = read_le32(b_ptr);
        std::uint32_t offset = read_le32(b_ptr + 4);

        if (count == 0) continue;
        if (offset >= bhd_size) return BinderError::corrupted_data;

        // Verifica layout de 24 bytes (padrão Bloodborne 64-bit)
        if (static_cast<std::uint64_t>(offset) + static_cast<std::uint64_t>(count) * 24 <= bhd_size) {
            for (std::uint32_t f = 0; f < count; ++f) {
                const std::uint8_t *rec = bytes + offset + f * 24;
                Bhd5FileEntry entry;
                entry.hash = read_le64(rec);
                entry.padded_size = read_le32(rec + 8);
                entry.file_size = read_le32(rec + 12);
                entry.file_offset = read_le64(rec + 16);
                entry.bucket_index = b;

                impl_->entries.push_back(entry);
                impl_->hash_to_index[entry.hash] = impl_->entries.size() - 1;
            }
        } else if (static_cast<std::uint64_t>(offset) + static_cast<std::uint64_t>(count) * 16 <= bhd_size) {
            // Compatibilidade com arquivos de 16 bytes (32-bit offsets)
            for (std::uint32_t f = 0; f < count; ++f) {
                const std::uint8_t *rec = bytes + offset + f * 16;
                Bhd5FileEntry entry;
                entry.hash = read_le32(rec);
                entry.padded_size = read_le32(rec + 4);
                entry.file_size = read_le32(rec + 8);
                entry.file_offset = read_le32(rec + 12);
                entry.bucket_index = b;

                impl_->entries.push_back(entry);
                impl_->hash_to_index[entry.hash] = impl_->entries.size() - 1;
            }
        } else {
            return BinderError::corrupted_data;
        }
    }

    if (!bdt_path.empty()) {
        impl_->bdt_path = bdt_path;
        impl_->bdt_stream.open(std::filesystem::u8path(bdt_path), std::ios::binary);
    }

    impl_->is_open = true;
    return BinderError::ok;
}

bool Bhd5Reader::has_file(std::uint64_t hash) const {
    if (!impl_ || !impl_->is_open) return false;
    return impl_->hash_to_index.find(hash) != impl_->hash_to_index.end();
}

bool Bhd5Reader::has_file(const std::string &path) const {
    if (!impl_ || !impl_->is_open) return false;
    std::uint64_t h64 = hash_path(path);
    if (has_file(h64)) return true;
    std::uint32_t h32 = hash_path_32(path);
    return has_file(static_cast<std::uint64_t>(h32));
}

const Bhd5FileEntry *Bhd5Reader::get_entry(std::uint64_t hash) const {
    if (!impl_ || !impl_->is_open) return nullptr;
    auto it = impl_->hash_to_index.find(hash);
    if (it == impl_->hash_to_index.end()) return nullptr;
    return &impl_->entries[it->second];
}

const Bhd5FileEntry *Bhd5Reader::get_entry(const std::string &path) const {
    if (!impl_ || !impl_->is_open) return nullptr;
    std::uint64_t h64 = hash_path(path);
    const auto *entry = get_entry(h64);
    if (entry) return entry;
    std::uint32_t h32 = hash_path_32(path);
    return get_entry(static_cast<std::uint64_t>(h32));
}

std::vector<Bhd5FileEntry> Bhd5Reader::entries() const {
    if (!impl_ || !impl_->is_open) return {};
    return impl_->entries;
}

BinderError Bhd5Reader::stream_file(std::uint64_t hash, std::uint64_t offset_in_file, void *dest, std::size_t bytes_to_read, std::size_t &out_bytes_read) {
    out_bytes_read = 0;
    if (!impl_ || !impl_->is_open) return BinderError::not_open;
    const auto *entry = get_entry(hash);
    if (!entry) return BinderError::file_not_found;

    if (offset_in_file >= entry->file_size) {
        return BinderError::ok;
    }

    std::size_t available = static_cast<std::size_t>(entry->file_size - offset_in_file);
    std::size_t to_read = std::min(bytes_to_read, available);
    if (to_read == 0) return BinderError::ok;

    std::lock_guard<std::mutex> lock(impl_->bdt_mutex);
    if (!impl_->bdt_stream.is_open()) {
        return BinderError::io_error;
    }

    std::uint64_t seek_pos = entry->file_offset + offset_in_file;
    impl_->bdt_stream.seekg(static_cast<std::streamoff>(seek_pos), std::ios::beg);
    if (!impl_->bdt_stream.good()) {
        return BinderError::io_error;
    }

    impl_->bdt_stream.read(static_cast<char *>(dest), static_cast<std::streamsize>(to_read));
    if (!impl_->bdt_stream) {
        return BinderError::io_error;
    }

    out_bytes_read = to_read;
    return BinderError::ok;
}

BinderError Bhd5Reader::stream_file(const std::string &path, std::uint64_t offset_in_file, void *dest, std::size_t bytes_to_read, std::size_t &out_bytes_read) {
    const auto *entry = get_entry(path);
    if (!entry) return BinderError::file_not_found;
    return stream_file(entry->hash, offset_in_file, dest, bytes_to_read, out_bytes_read);
}

BinderError Bhd5Reader::read_file(std::uint64_t hash, std::vector<std::uint8_t> &out_data) {
    const auto *entry = get_entry(hash);
    if (!entry) return BinderError::file_not_found;

    out_data.resize(entry->file_size);
    if (entry->file_size == 0) return BinderError::ok;

    std::size_t read_bytes = 0;
    BinderError err = stream_file(hash, 0, out_data.data(), entry->file_size, read_bytes);
    if (err != BinderError::ok) {
        out_data.clear();
        return err;
    }
    if (read_bytes != entry->file_size) {
        out_data.clear();
        return BinderError::io_error;
    }
    return BinderError::ok;
}

BinderError Bhd5Reader::read_file(const std::string &path, std::vector<std::uint8_t> &out_data) {
    const auto *entry = get_entry(path);
    if (!entry) return BinderError::file_not_found;
    return read_file(entry->hash, out_data);
}

// ============================================================================
// 3. BND4 READER IMPLEMENTATION
// ============================================================================

struct Bnd4Reader::Impl {
    std::vector<std::uint8_t> buffer;
    std::vector<Bnd4FileEntry> entries;
    std::unordered_map<std::int32_t, std::size_t> id_to_index;
    std::unordered_map<std::string, std::size_t> name_to_index;
    std::string version;
    bool is_open{false};
};

Bnd4Reader::Bnd4Reader() : impl_(std::make_unique<Impl>()) {}
Bnd4Reader::~Bnd4Reader() { close(); }

void Bnd4Reader::close() {
    if (impl_) {
        impl_->buffer.clear();
        impl_->entries.clear();
        impl_->id_to_index.clear();
        impl_->name_to_index.clear();
        impl_->version.clear();
        impl_->is_open = false;
    }
}

bool Bnd4Reader::is_open() const {
    return impl_ && impl_->is_open;
}

std::size_t Bnd4Reader::file_count() const {
    return impl_ ? impl_->entries.size() : 0;
}

const std::string &Bnd4Reader::version() const {
    static const std::string empty;
    return impl_ ? impl_->version : empty;
}

BinderError Bnd4Reader::open(const std::string &bnd_path) {
    close();

    std::ifstream file(std::filesystem::u8path(bnd_path), std::ios::binary | std::ios::ate);
    if (!file.is_open()) return BinderError::file_not_found;

    auto size = file.tellg();
    if (size < 64) return BinderError::invalid_header;

    std::vector<std::uint8_t> buf(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char *>(buf.data()), size)) {
        return BinderError::io_error;
    }

    return load(buf.data(), buf.size());
}

BinderError Bnd4Reader::load(const void *data, std::size_t size) {
    close();
    if (!data || size < 64) return BinderError::invalid_header;

    const auto *bytes = static_cast<const std::uint8_t *>(data);
    if (std::memcmp(bytes, "BND4", 4) != 0) {
        return BinderError::invalid_magic;
    }

    std::uint32_t file_count = read_le32(bytes + 12);
    std::uint64_t header_size = read_le64(bytes + 16);
    char ver[9]{0};
    std::memcpy(ver, bytes + 24, 8);
    impl_->version = ver;
    std::uint64_t file_header_size = read_le64(bytes + 32);
    bool is_unicode = (bytes[0x0A] == 1);

    if (file_header_size != 36 && file_header_size != 40 && file_header_size != 48) {
        return BinderError::invalid_header;
    }

    if (header_size + static_cast<std::uint64_t>(file_count) * file_header_size > size) {
        return BinderError::corrupted_data;
    }

    // Copia o buffer para manter os dados residentes para extrações
    impl_->buffer.assign(bytes, bytes + size);
    impl_->entries.reserve(file_count);

    for (std::uint32_t i = 0; i < file_count; ++i) {
        const std::uint8_t *entry_ptr = bytes + header_size + i * file_header_size;
        Bnd4FileEntry entry;
        entry.flags = entry_ptr[0];
        entry.compressed_size = read_le64(entry_ptr + 8);
        entry.uncompressed_size = read_le64(entry_ptr + 16);

        std::uint32_t name_offset = 0;
        if (file_header_size == 36) {
            entry.data_offset = read_le32(entry_ptr + 24);
            entry.id = static_cast<std::int32_t>(read_le32(entry_ptr + 28));
            name_offset = read_le32(entry_ptr + 32);
        } else {
            entry.data_offset = read_le64(entry_ptr + 24);
            entry.id = static_cast<std::int32_t>(read_le32(entry_ptr + 32));
            name_offset = read_le32(entry_ptr + 36);
        }

        if (name_offset > 0 && name_offset < size) {
            bool use_utf16 = is_unicode;
            if (!use_utf16 && name_offset + 1 < size) {
                if (bytes[name_offset] != 0 && bytes[name_offset + 1] == 0) {
                    use_utf16 = true;
                }
            }
            if (use_utf16) {
                entry.name = utf16le_to_utf8(bytes + name_offset, size - name_offset);
            } else {
                entry.name = reinterpret_cast<const char *>(bytes + name_offset);
            }
        }

        impl_->entries.push_back(entry);
        impl_->id_to_index[entry.id] = impl_->entries.size() - 1;
        if (!entry.name.empty()) {
            impl_->name_to_index[entry.name] = impl_->entries.size() - 1;
        }
    }

    impl_->is_open = true;
    return BinderError::ok;
}

bool Bnd4Reader::has_file(std::int32_t id) const {
    if (!impl_ || !impl_->is_open) return false;
    return impl_->id_to_index.find(id) != impl_->id_to_index.end();
}

bool Bnd4Reader::has_file(const std::string &name) const {
    if (!impl_ || !impl_->is_open) return false;
    return get_entry(name) != nullptr;
}

const Bnd4FileEntry *Bnd4Reader::get_entry(std::int32_t id) const {
    if (!impl_ || !impl_->is_open) return nullptr;
    auto it = impl_->id_to_index.find(id);
    if (it == impl_->id_to_index.end()) return nullptr;
    return &impl_->entries[it->second];
}

const Bnd4FileEntry *Bnd4Reader::get_entry(const std::string &name) const {
    if (!impl_ || !impl_->is_open || name.empty()) return nullptr;

    auto it = impl_->name_to_index.find(name);
    if (it != impl_->name_to_index.end()) {
        return &impl_->entries[it->second];
    }

    auto normalize_for_match = [](const std::string &s) {
        std::string n;
        n.reserve(s.size());
        for (char c : s) {
            if (c == '/') n.push_back('\\');
            else n.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        return n;
    };

    std::string norm_target = normalize_for_match(name);

    for (const auto &entry : impl_->entries) {
        std::string norm_entry = normalize_for_match(entry.name);
        if (norm_entry == norm_target) {
            return &entry;
        }
        if (norm_entry.size() >= norm_target.size()) {
            std::size_t offset = norm_entry.size() - norm_target.size();
            if (norm_entry.compare(offset, norm_target.size(), norm_target) == 0) {
                if (offset == 0 || norm_entry[offset - 1] == '\\') {
                    return &entry;
                }
            }
        }
    }
    return nullptr;
}

std::vector<Bnd4FileEntry> Bnd4Reader::entries() const {
    if (!impl_ || !impl_->is_open) return {};
    return impl_->entries;
}

BinderError Bnd4Reader::read_file(std::int32_t id, std::vector<std::uint8_t> &out_data) const {
    const auto *entry = get_entry(id);
    if (!entry) return BinderError::file_not_found;

    if (entry->data_offset + entry->compressed_size > impl_->buffer.size()) {
        return BinderError::corrupted_data;
    }

    out_data.assign(
        impl_->buffer.begin() + static_cast<std::ptrdiff_t>(entry->data_offset),
        impl_->buffer.begin() + static_cast<std::ptrdiff_t>(entry->data_offset + entry->compressed_size));
    return BinderError::ok;
}

BinderError Bnd4Reader::read_file(const std::string &name, std::vector<std::uint8_t> &out_data) const {
    const auto *entry = get_entry(name);
    if (!entry) return BinderError::file_not_found;
    return read_file(entry->id, out_data);
}

} // namespace bb
