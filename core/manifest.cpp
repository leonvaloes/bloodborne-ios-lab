#include "manifest.h"
#include <sstream>

namespace bb {

std::string TranslationManifest::to_json() const {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"schema_version\": " << schema_version << ",\n";
    ss << "  \"guest_arch\": \"" << guest_arch << "\",\n";
    ss << "  \"host_arch\": \"" << host_arch << "\",\n";
    ss << "  \"binary_sha256\": \"" << binary_sha256 << "\",\n";
    ss << "  \"functions\": [\n";
    for (std::size_t i = 0; i < functions.size(); ++i) {
        const auto& fn = functions[i];
        ss << "    {\n";
        ss << "      \"guest_rva\": \"" << fn.guest_rva << "\",\n";
        ss << "      \"size\": " << fn.size << ",\n";
        ss << "      \"entry_symbol\": \"" << fn.entry_symbol << "\",\n";
        ss << "      \"indirect_targets_resolved\": " << (fn.indirect_targets_resolved ? "true" : "false") << ",\n";
        ss << "      \"status\": \"" << fn.status << "\"\n";
        ss << "    }" << (i + 1 < functions.size() ? "," : "") << "\n";
    }
    ss << "  ]\n";
    ss << "}\n";
    return ss.str();
}

bool TranslationManifest::validate() const {
    if (schema_version != 1) return false;
    if (guest_arch != "x86_64") return false;
    if (host_arch != "arm64-apple-ios" && host_arch != "x86_64-pc-windows" && host_arch != "host") return false;
    for (const auto& fn : functions) {
        if (fn.guest_rva.empty() || fn.entry_symbol.empty()) return false;
        if (fn.size == 0) return false;
    }
    return true;
}

} // namespace bb
