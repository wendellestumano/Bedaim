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
    const auto tailTwo = bedrocktools::diagnostics::snapshotTail(2);
    assert(tailTwo.size() == 2);
    assert(tailTwo.front().sequence == 7);
    assert(tailTwo.back().sequence == 8);
    const auto tailAll = bedrocktools::diagnostics::snapshotTail(100);
    assert(tailAll.size() == 8);
    const auto tailZero = bedrocktools::diagnostics::snapshotTail(0);
    assert(tailZero.empty());

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
        assert(exported.at("schema_version").get<int>() == 2);
        assert(exported.at("entry_count").get<std::size_t>() == 8);
        assert(exported.at("entries").size() == 8);
        assert(exported.at("buffer_capacity").get<std::size_t>() ==
               bedrocktools::diagnostics::bufferCapacity());
        assert(exported.at("dropped_entry_count").get<std::uint64_t>() == 0);
        assert(exported.at("first_sequence").get<std::uint64_t>() == 1);
        assert(exported.at("last_sequence").get<std::uint64_t>() == 8);
        assert(exported.at("entries").front().at("level") == "TRACE");
        assert(exported.at("entries").front().contains("monotonic_ns"));
        assert(exported.at("entries").front().at("monotonic_ns").get<std::uint64_t>() > 0);
        assert(exported.at("entries").front().contains("thread_id"));
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

    // The larger bounded ring retains the newest events and reports losses.
    bedrocktools::diagnostics::clear();
    const auto capacity = bedrocktools::diagnostics::bufferCapacity();
    assert(capacity == 32768);
    constexpr std::size_t overflowBy = 7;
    for (std::size_t i = 0; i < capacity + overflowBy; ++i) {
        log(Level::Trace, "OverflowTest", std::to_string(i));
    }
    assert(bedrocktools::diagnostics::count() == capacity);
    assert(bedrocktools::diagnostics::droppedCount() == overflowBy);
    const auto overflowEntries = bedrocktools::diagnostics::snapshot();
    assert(overflowEntries.front().sequence == overflowBy + 1);
    assert(overflowEntries.front().message == std::to_string(overflowBy));
    assert(overflowEntries.back().sequence == capacity + overflowBy);
    assert(overflowEntries.back().message == std::to_string(capacity + overflowBy - 1));

    const auto overflowJsonPath = std::filesystem::temp_directory_path() / "bedaim_diagnostics_overflow.json";
    assert(bedrocktools::diagnostics::exportJsonTo(overflowJsonPath.string(), &error));
    {
        std::ifstream overflowIn(overflowJsonPath, std::ios::binary);
        const nlohmann::json overflow = nlohmann::json::parse(
            std::string((std::istreambuf_iterator<char>(overflowIn)), std::istreambuf_iterator<char>()));
        assert(overflow.at("entry_count").get<std::size_t>() == capacity);
        assert(overflow.at("buffer_capacity").get<std::size_t>() == capacity);
        assert(overflow.at("dropped_entry_count").get<std::uint64_t>() == overflowBy);
        assert(overflow.at("first_sequence").get<std::uint64_t>() == overflowBy + 1);
        assert(overflow.at("last_sequence").get<std::uint64_t>() == capacity + overflowBy);
    }
    std::filesystem::remove(overflowJsonPath, ec);
    bedrocktools::diagnostics::clear();
    assert(bedrocktools::diagnostics::count() == 0);
    assert(bedrocktools::diagnostics::droppedCount() == 0);
    return 0;
}
