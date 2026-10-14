#include "launcher/MenuConfigValue.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {
int failures = 0;
int checks = 0;

void check(bool condition, std::string_view name) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << name << '\n';
    }
}

void checkBoolSpellings() {
    for (const auto& [raw, expected] : {
             std::pair{"true", true}, std::pair{"1", true}, std::pair{"on", true},
             std::pair{"yes", true}, std::pair{"TRUE", true}, std::pair{"false", false},
             std::pair{"0", false}, std::pair{"off", false}, std::pair{"no", false},
             std::pair{"FALSE", false}}) {
        nlohmann::json config{{"enabled", !expected}, {"unrelated", 17}};
        const auto result = bedaim::menuconfig::applyValue(config, "enabled", raw);
        check(result == bedaim::menuconfig::ApplyResult::Applied, "valid boolean accepted");
        check(config["enabled"].get<bool>() == expected, "boolean normalized correctly");
        check(config["unrelated"] == 17, "unrelated key preserved");
    }
}

void checkMalformedBooleanPreservesValue() {
    nlohmann::json config{{"enabled", true}, {"unrelated", "preserve"}};
    const auto result = bedaim::menuconfig::applyValue(config, "enabled", "perhaps");
    check(result == bedaim::menuconfig::ApplyResult::InvalidValue, "malformed boolean rejected");
    check(config["enabled"] == true, "malformed boolean does not mutate original");
    check(config["unrelated"] == "preserve", "malformed boolean leaves unrelated key unchanged");
}

void checkStrictIntegerParsing() {
    nlohmann::json config{{"count", 4}};
    check(bedaim::menuconfig::applyValue(config, "count", "-12") ==
              bedaim::menuconfig::ApplyResult::Applied,
          "signed integer accepted");
    check(config["count"] == -12, "signed integer parsed exactly");

    for (const auto raw : {"", "12x", " 12", "999999999999999999999999999999999"}) {
        nlohmann::json value{{"count", 7}};
        check(bedaim::menuconfig::applyValue(value, "count", raw) ==
                  bedaim::menuconfig::ApplyResult::InvalidValue,
              "malformed or overflow integer rejected");
        check(value["count"] == 7, "invalid integer leaves old value unchanged");
    }
}

void checkStrictFloatParsing() {
    nlohmann::json config{{"strength", 1.0}};
    check(bedaim::menuconfig::applyValue(config, "strength", "-2.75") ==
              bedaim::menuconfig::ApplyResult::Applied,
          "decimal float accepted");
    check(std::abs(config["strength"].get<double>() + 2.75) < 1e-9,
          "decimal float parsed exactly");

    for (const auto raw : {"", "1.2x", "NaN", "nan", "Infinity", "-inf"}) {
        nlohmann::json value{{"strength", 0.5}};
        check(bedaim::menuconfig::applyValue(value, "strength", raw) ==
                  bedaim::menuconfig::ApplyResult::InvalidValue,
              "malformed or non-finite float rejected");
        check(value["strength"] == 0.5, "invalid float leaves old value unchanged");
    }
}

void checkStringAndUnknownKeys() {
    nlohmann::json config{{"mode", "Classic"}, {"unrelated", 99}};
    check(bedaim::menuconfig::applyValue(config, "mode", "2") ==
              bedaim::menuconfig::ApplyResult::Applied,
          "radio/select index accepted as string payload");
    check(config["mode"] == "2", "radio/select payload remains a string for module normalization");
    check(bedaim::menuconfig::applyValue(config, "newKey", "hello") ==
              bedaim::menuconfig::ApplyResult::Applied,
          "missing field accepts string payload");
    check(config["newKey"] == "hello", "missing field is created as string");
    check(config["unrelated"] == 99, "unrelated setting remains unchanged");

    config["optional"] = nullptr;
    check(bedaim::menuconfig::applyValue(config, "optional", "enabled") ==
              bedaim::menuconfig::ApplyResult::Applied,
          "null field accepts string payload");
    check(config["optional"] == "enabled", "null field becomes string");
}
}

int main() {
    checkBoolSpellings();
    checkMalformedBooleanPreservesValue();
    checkStrictIntegerParsing();
    checkStrictFloatParsing();
    checkStringAndUnknownKeys();

    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
