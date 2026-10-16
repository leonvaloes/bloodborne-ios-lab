#ifndef BB_GAME_PACKAGE_H
#define BB_GAME_PACKAGE_H

#include "binder.h"
#include "vfs.h"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>

namespace bb {

enum class PackageType {
    unknown = 0,
    base,   // Arquivos base do jogo (ex: data0, data1, ...)
    patch,  // Atualizações e patches (ex: patch, patch0, data0_patch, ...)
    dlc     // Conteúdos adicionais e expansões (ex: dlc, dlc0, ...)
};

const char *package_type_to_string(PackageType type);

// Metadados e leitor de um pacote individual (.bhd5 + .bdt)
struct PackageInfo {
    std::string name;                   // Identificador do pacote (ex: "data0", "patch")
    PackageType type{PackageType::unknown};
    int priority{0};                    // Maior número = maior precedência
    std::string bhd_path;               // Caminho host para o arquivo .bhd5
    std::string bdt_path;               // Caminho host para o arquivo .bdt
    std::uint64_t bdt_size{0};          // Tamanho do contêiner BDT em bytes
    std::uint64_t bhd_size{0};          // Tamanho do cabeçalho BHD5 em bytes
    std::size_t file_count{0};          // Quantidade de arquivos no BHD5
    std::shared_ptr<Bhd5Reader> reader; // Leitor BHD5 aberto
};

// Relatório diagnóstico estruturado de inspeção e integridade
struct PackageInspectionReport {
    bool valid{false};                          // True se inventário e arquivos essenciais estão válidos
    std::size_t package_count{0};               // Quantidade de contêineres carregados
    std::size_t total_files_indexed{0};         // Total de arquivos catalogados nos BHDs
    std::size_t unique_files_count{0};          // Total de arquivos únicos (deduplicando overrides)
    std::uint64_t total_bdt_size{0};            // Tamanho agregado dos arquivos BDT em bytes
    std::uint64_t total_bhd_size{0};            // Tamanho agregado dos arquivos BHD5 em bytes
    std::vector<std::string> packages_loaded;   // Nomes dos pacotes carregados (ordem decrescente de prioridade)
    std::vector<std::string> essential_files_found;   // Arquivos essenciais localizados
    std::vector<std::string> essential_files_missing; // Arquivos essenciais ausentes
    std::unordered_map<std::string, std::string> essential_file_package_map; // Caminho -> Pacote resolvedor
    std::string diagnostic_summary;             // Mensagem estruturada legível do diagnóstico
};

class GamePackageManager {
public:
    GamePackageManager();
    ~GamePackageManager();

    // Limpa todos os contêineres gerenciados
    void clear();

    // ------------------------------------------------------------------------
    // Varredura estruturada de diretórios
    // ------------------------------------------------------------------------
    // Varre diretório host localizando pares correspondentes .bhd5 e .bdt.
    // Retorna a quantidade de novos contêineres adicionados com sucesso.
    std::size_t scan_directory(const std::string &host_dir, bool recursive = false);

    // Registra manualmente um contêiner a partir de seus arquivos .bhd5 e .bdt.
    // Se priority for negativa, calcula a prioridade padrão com base no PackageType.
    bool add_package(const std::string &bhd_path, const std::string &bdt_path = "",
                     PackageType type = PackageType::unknown, int priority = -1);

    // ------------------------------------------------------------------------
    // Inventário e Estatísticas
    // ------------------------------------------------------------------------
    std::size_t package_count() const;
    std::vector<PackageInfo> packages() const;
    bool has_package(const std::string &name) const;
    bool get_package_info(const std::string &name, PackageInfo &out_info) const;
    const PackageInfo *get_package(const std::string &name) const;

    std::size_t total_files_indexed() const;
    std::size_t unique_files_count() const;
    std::uint64_t total_bdt_size() const;
    std::uint64_t total_bhd_size() const;

    // ------------------------------------------------------------------------
    // Consulta e Leitura Hierárquica (Patch > DLC > Base)
    // ------------------------------------------------------------------------
    bool has_file(const std::string &path) const;
    bool has_file(std::uint64_t hash) const;

    const Bhd5FileEntry *find_entry(const std::string &path, std::string *out_package_name = nullptr) const;
    const Bhd5FileEntry *find_entry(std::uint64_t hash, std::string *out_package_name = nullptr) const;

    BinderError read_file(const std::string &path, std::vector<std::uint8_t> &out_data) const;
    BinderError read_file(std::uint64_t hash, std::vector<std::uint8_t> &out_data) const;

    BinderError stream_file(const std::string &path, std::uint64_t offset,
                            void *dest, std::size_t bytes_to_read, std::size_t &out_bytes_read);
    BinderError stream_file(std::uint64_t hash, std::uint64_t offset,
                            void *dest, std::size_t bytes_to_read, std::size_t &out_bytes_read);

    // ------------------------------------------------------------------------
    // Montagem no VirtualFileSystem
    // ------------------------------------------------------------------------
    // Monta todos os pacotes carregados no VFS sob os pontos de montagem fornecidos
    // (por padrão: {"/dvdroot", "/app0/dvdroot_ps4"}) preservando as prioridades relativas.
    bool mount_all(VirtualFileSystem &vfs, const std::vector<std::string> &mount_points = {"/dvdroot", "/app0/dvdroot_ps4"});
    bool unmount_all(VirtualFileSystem &vfs, const std::vector<std::string> &mount_points = {"/dvdroot", "/app0/dvdroot_ps4"});

    // ------------------------------------------------------------------------
    // Validação de Integridade Essencial e Contêineres Sintéticos Defensivos
    // ------------------------------------------------------------------------
    // Cria e registra contêineres sintéticos defensivos (shaders/gui) no VFS quando ausentes.
    void synthesize_defensive_stubs(VirtualFileSystem &vfs);

    // Verifica se os arquivos essenciais para o boot da engine estão presentes nos contêineres.
    // Se essential_paths for vazio, utiliza default_essential_paths().
    PackageInspectionReport verify_essential_assets(const std::vector<std::string> &essential_paths = {}) const;

    // Valida diretamente por lista de pares (rótulo/caminho, hash FromSoftware)
    PackageInspectionReport verify_essential_hashes(const std::vector<std::pair<std::string, std::uint64_t>> &essential_items) const;

    // Lista de arquivos fundamentais para o boot da engine Bloodborne
    static std::vector<std::string> default_essential_paths();

    // Utilitários de classificação e priorização
    static PackageType detect_package_type(const std::string &package_name);
    static int default_priority_for(PackageType type, const std::string &package_name);

private:
    void sort_packages_locked();

    mutable std::mutex mutex_;
    std::vector<PackageInfo> packages_; // Ordenado por prioridade decrescente
};

} // namespace bb

#endif // BB_GAME_PACKAGE_H
