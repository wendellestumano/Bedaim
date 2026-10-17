#include "AtomicConfigWriter.hpp"

#include <filesystem>
#include <fstream>

namespace bedrocktools::config {

bool writePayloadAtomically(const std::string& pathValue, const std::string& payload) noexcept {
    std::filesystem::path tempPath;
    try {
        const std::filesystem::path path(pathValue);
        if (path.empty()) return false;
        tempPath = path;
        tempPath += ".tmp";

        std::error_code ec;
        if (!path.parent_path().empty()) {
            std::filesystem::create_directories(path.parent_path(), ec);
            if (ec) return false;
        }

        {
            std::ofstream outFile(tempPath, std::ios::binary | std::ios::trunc);
            if (!outFile) return false;
            outFile.write(payload.data(), static_cast<std::streamsize>(payload.size()));
            outFile.flush();
            if (!outFile.good()) {
                outFile.close();
                std::filesystem::remove(tempPath, ec);
                return false;
            }
        }

        // On Android/Linux rename replaces an existing file atomically when the
        // source and destination are on the same filesystem. Never delete the
        // destination as a fallback; preserving old settings beats data loss.
        ec.clear();
        std::filesystem::rename(tempPath, path, ec);
        if (ec) {
            std::error_code cleanup;
            std::filesystem::remove(tempPath, cleanup);
            return false;
        }
        return true;
    } catch (...) {
        if (!tempPath.empty()) {
            std::error_code cleanup;
            std::filesystem::remove(tempPath, cleanup);
        }
        return false;
    }
}

} // namespace bedrocktools::config
