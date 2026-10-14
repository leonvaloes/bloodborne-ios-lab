#ifndef BB_BINDER_H
#define BB_BINDER_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>

namespace bb {

enum class BinderError {
    ok = 0,
    file_not_found,
    invalid_magic,
    invalid_header,
    decompression_failed,
    unsupported_algorithm,
    io_error,
    out_of_bounds,
    corrupted_data,
    not_open
};

const char *binder_error_string(BinderError err);

// ============================================================================
// 1. DESCOMPRESSOR DCX (DFLT / EDGE / ZLIB / KRAK)
// ============================================================================

enum class DcxAlgorithm {
    unknown = 0,
    deflate,    // DFLT, EDGE, ZLIB
    kraken      // KRAK (Oodle)
};

struct DcxHeader {
    std::uint32_t version{0};
    std::uint32_t uncompressed_size{0};
    std::uint32_t compressed_size{0};
    DcxAlgorithm algorithm{DcxAlgorithm::unknown};
    char algorithm_tag[5]{0, 0, 0, 0, 0};
    std::uint32_t dcs_offset{0};
    std::uint32_t dcp_offset{0};
    std::uint32_t dca_offset{0};
    std::uint32_t data_offset{0};
};

class DcxDecompressor {
public:
    // Identificação de assinatura mágica "DCX\0"
    static bool is_dcx(const void *data, std::size_t size);
    static bool is_dcx_file(const std::string &path);

    // Parsing e descompressão
    static BinderError parse_header(const void *data, std::size_t size, DcxHeader &out_header);
    static BinderError decompress(const void *in_data, std::size_t in_size, std::vector<std::uint8_t> &out_data);
    static BinderError decompress_file(const std::string &path, std::vector<std::uint8_t> &out_data);

    // Utilitário para sintetizar contêineres DCX em testes unitários e ferramentas
    static BinderError create_dcx(const void *in_data, std::size_t in_size, std::vector<std::uint8_t> &out_dcx, DcxAlgorithm algo = DcxAlgorithm::deflate);
};

// ============================================================================
// 2. LEITOR DE PACOTES BHD5 / BDT (BIG HEADER DATA & BIG DATA TABLE)
// ============================================================================

struct Bhd5FileEntry {
    std::uint64_t hash{0};
    std::uint32_t padded_size{0};
    std::uint32_t file_size{0};
    std::uint64_t file_offset{0};
    std::uint32_t bucket_index{0};
};

class Bhd5Reader {
public:
    Bhd5Reader();
    ~Bhd5Reader();

    // Hashing e normalização de caminhos no estilo FromSoftware
    static std::uint64_t hash_path(const std::string &path, std::uint64_t prime = 37);
    static std::uint32_t hash_path_32(const std::string &path, std::uint32_t prime = 37);
    static std::string normalize_archive_path(const std::string &path);

    // Abertura de arquivos .bhd5 e .bdt no disco
    BinderError open(const std::string &bhd_path, const std::string &bdt_path = "");

    // Carga de cabeçalho .bhd5 a partir da memória
    BinderError load_header(const void *bhd_data, std::size_t bhd_size, const std::string &bdt_path = "");

    void close();
    bool is_open() const;

    std::size_t file_count() const;
    std::size_t bucket_count() const;

    // Busca e inspeção
    bool has_file(std::uint64_t hash) const;
    bool has_file(const std::string &path) const;
    const Bhd5FileEntry *get_entry(std::uint64_t hash) const;
    const Bhd5FileEntry *get_entry(const std::string &path) const;
    std::vector<Bhd5FileEntry> entries() const;

    // Leitura completa de arquivo a partir do .bdt
    BinderError read_file(std::uint64_t hash, std::vector<std::uint8_t> &out_data);
    BinderError read_file(const std::string &path, std::vector<std::uint8_t> &out_data);

    // Streaming aleatório sob demanda (leitura de pedaços sem carregar 30 GB na memória)
    BinderError stream_file(std::uint64_t hash, std::uint64_t offset_in_file, void *dest, std::size_t bytes_to_read, std::size_t &out_bytes_read);
    BinderError stream_file(const std::string &path, std::uint64_t offset_in_file, void *dest, std::size_t bytes_to_read, std::size_t &out_bytes_read);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================================
// 3. LEITOR DE CONTÊINERES BND4 (FROMSOFTWARE BINDER V4)
// ============================================================================

struct Bnd4FileEntry {
    std::int32_t id{0};
    std::string name;
    std::uint64_t compressed_size{0};
    std::uint64_t uncompressed_size{0};
    std::uint64_t data_offset{0};
    std::uint8_t flags{0};
};

class Bnd4Reader {
public:
    Bnd4Reader();
    ~Bnd4Reader();

    BinderError open(const std::string &bnd_path);
    BinderError load(const void *data, std::size_t size);
    void close();

    bool is_open() const;
    std::size_t file_count() const;
    const std::string &version() const;

    bool has_file(std::int32_t id) const;
    bool has_file(const std::string &name) const;

    const Bnd4FileEntry *get_entry(std::int32_t id) const;
    const Bnd4FileEntry *get_entry(const std::string &name) const;
    std::vector<Bnd4FileEntry> entries() const;

    BinderError read_file(std::int32_t id, std::vector<std::uint8_t> &out_data) const;
    BinderError read_file(const std::string &name, std::vector<std::uint8_t> &out_data) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bb

#endif // BB_BINDER_H
