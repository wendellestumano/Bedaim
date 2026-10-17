#include "core/Diagnostics.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

int main() {
    using bedrocktools::diagnostics::Level;
    using bedrocktools::diagnostics::log;

    bedrocktools::diagnostics::clear();
    log(Level::Trace, "Test", "trace message");
    log(Level::Debug, "Test", "debug message");
    log(Level::Info, "Test", "info message");
    log(Level::Warning, "Test", "warning message");
    log(Level::Error, "Test", "error message");
    log(Level::Critical, "Test", "critical message");

    assert(bedrocktools::diagnostics::count() == 6);

    bedrocktools::diagnostics::probe(Level::Debug, "ProbeTest", "target", "none");
    assert(bedrocktools::diagnostics::count() == 7);
    bedrocktools::diagnostics::probe(Level::Debug, "ProbeTest", "target", "none");
    assert(bedrocktools::diagnostics::count() == 7);
    bedrocktools::diagnostics::probe(Level::Warning, "ProbeTest", "target", "missing",
                                     "native query returned no actor");
    assert(bedrocktools::diagnostics::count() == 8);
    const auto entries = bedrocktools::diagnostics::snapshot();
    assert(entries.size() == 6);
    assert(entries.front().level == Level::Trace);
    assert(entries[6].message == "target=none");
    assert(entries.back().message == "target=missing: native query returned no actor");
    assert(entries.back().level == Level::Warning);

    const auto path = std::filesystem::temp_directory_path() / "bedaim_diagnostics_test.log";
    std::string error;
    assert(bedrocktools::diagnostics::exportTo(path.string(), &error));

    const auto blockedPath = std::filesystem::temp_directory_path() / "bedaim_diagnostics_blocked";
    std::error_code ec;
    std::filesystem::remove_all(blockedPath, ec);
    ec.clear();
    assert(std::filesystem::create_directory(blockedPath, ec));
    assert(!bedrocktools::diagnostics::exportTo(blockedPath.string(), &error));
    assert(std::filesystem::is_directory(blockedPath));
    assert(!std::filesystem::exists(blockedPath.string() + ".tmp"));
    std::filesystem::remove_all(blockedPath, ec);
    std::ifstream in(path, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    assert(content.find("TRACE") != std::string::npos);
    assert(content.find("DEBUG") != std::string::npos);
    assert(content.find("INFO") != std::string::npos);
    assert(content.find("WARN") != std::string::npos);
    assert(content.find("ERROR") != std::string::npos);
    assert(content.find("CRITICAL") != std::string::npos);
    std::filesystem::remove(path, ec);
    bedrocktools::diagnostics::clear();
    assert(bedrocktools::diagnostics::count() == 0);
    bedrocktools::diagnostics::probe(Level::Debug, "ProbeTest", "target", "none");
    assert(bedrocktools::diagnostics::count() == 1);
    return 0;
}
