#include "config/AtomicConfigWriter.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {
int checks = 0;
int failures = 0;
void check(bool condition, std::string_view name) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << name << '\\n'; }
}
std::string readAll(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}

int main() {
    namespace fs = std::filesystem;
    using bedrocktools::config::writePayloadAtomically;
    const auto root = fs::temp_directory_path() / "bedaim-atomic-config-writer-test";
    std::error_code ec;
    fs::remove_all(root, ec);
    ec.clear();
    fs::create_directories(root, ec);
    check(!ec, "temporary test folder is created");
    if (ec) return EXIT_FAILURE;

    const auto config = root / "config.json";
    check(writePayloadAtomically(config.string(), "{\\\"value\\\":1}"), "first snapshot is written");
    check(readAll(config) == "{\\\"value\\\":1}", "first snapshot content matches");
    check(writePayloadAtomically(config.string(), "{\\\"value\\\":2}"), "second snapshot replaces existing file");
    check(readAll(config) == "{\\\"value\\\":2}", "replacement content matches");
    check(!fs::exists(config.string() + ".tmp"), "temporary file removed after successful write");

    const auto blocked = root / "directory-destination";
    fs::create_directories(blocked, ec);
    check(!ec, "blocked destination directory is created");
    check(!writePayloadAtomically(blocked.string(), "must-not-replace-a-directory"), "rename failure is reported");
    check(fs::is_directory(blocked), "failed write never deletes the existing destination");
    check(!fs::exists(blocked.string() + ".tmp"), "failed write cleans up only temporary file");

    fs::remove_all(root, ec);
    std::cout << checks << " checks, " << failures << " failures\\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
