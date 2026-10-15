set_project("BedaimHostTests")
set_version("0.1.0")
set_languages("c++20")
add_rules("mode.debug", "mode.release")

add_requires("nlohmann_json v3.11.3")

target("menu_config_value_test")
    set_kind("binary")
    add_files("launcher/menu_config_value_test.cpp", "../src/launcher/MenuConfigValue.cpp")
    add_includedirs("../include", "../src")
    add_packages("nlohmann_json")
target_end()

target("aimassist_math_test")
    set_kind("binary")
    add_files("launcher/aimassist_math_test.cpp", "../src/modules/player/AimAssistMath.cpp")
    add_includedirs("../include", "../src")
target_end()
