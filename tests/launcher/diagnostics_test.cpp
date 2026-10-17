#include "core/Diagnostics.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <nlohmann/json.hpp>

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
    assert(entries.size() == 8);
    assert(entries.front().level == Level::Trace);
    assert(entries[6].message == "target=none");
    assert(entries.back().message == "target=missing: native query returned no actor");
    assert(entries.back().level == Level::Warning);

    const auto path = std::filesystem::temp_directory_path() / "bedaim_diagnostics_test.log";
    std::string error;
    assert(bedrocktools::diagnostics::exportTo(path.string(), &error));

    const auto jsonPath = std::filesystem::temp_directory_path() / "bedaim_diagnostics_test.json";
    assert(bedrocktools::diagnostics::exportJsonTo(jsonPath.string(), &error));
    {
        std::ifstream jsonIn(jsonPath, std::ios::binary);
        const nlohmann::json exported = nlohmann::json::parse(
            std::string((std::istreambuf_iterator<char>(jsonIn)), std::istreambuf_iterator<char>()));
        assert(exported.at("schema_version").get<int>() == 1);
        assert(exported.at("entry_count").get<std::size_t>() == 8);
        assert(exported.at("entries").size() == 8);
        assert(exported.at("entries").front().at("level") == "TRACE");
        assert(exported.at("entries").back().at("message") ==
               "target=missing: native query returned no actor");
    }

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

    // A rolling export must replace the previous session, not append old entries.
    bedrocktools::diagnostics::clear();
    log(Level::Info, "Test", "fresh gameplay session only");
    const auto freshEntries = bedrocktools::diagnostics::snapshot();
    assert(freshEntries.size() == 1);
    assert(freshEntries.front().sequence == 1);
    assert(bedrocktools::diagnostics::exportTo(path.string(), &error));
    {
        std::ifstream freshIn(path, std::ios::binary);
        const std::string freshContent(
            (std::istreambuf_iterator<char>(freshIn)), std::istreambuf_iterator<char>());
        assert(freshContent.find("fresh gameplay session only") != std::string::npos);
        assert(freshContent.find("trace message") == std::string::npos);
        assert(freshContent.find("entries=1") != std::string::npos);
    }

    std::filesystem::remove(path, ec);
    std::filesystem::remove(jsonPath, ec);
    bedrocktools::diagnostics::clear();
    assert(bedrocktools::diagnostics::count() == 0);
    bedrocktools::diagnostics::probe(Level::Debug, "ProbeTest", "target", "none");
    assert(bedrocktools::diagnostics::count() == 1);
    return 0;
}
