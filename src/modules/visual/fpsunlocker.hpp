#pragma once

#include "../Module.hpp"
#include <EGL/egl.h>
#include <cstdint>
#include <time.h>
#include <chrono>

class FPSUnlockerModule : public Module {
public:
    FPSUnlockerModule();
    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void requestTemporaryTargetFPS(int fps, int durationMs);

    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

private:
    int targetFPS = 0;
    int m_temporaryTargetFPS = 0;
    std::chrono::steady_clock::time_point m_temporaryTargetUntil{};
    bool m_restoreTemporaryModuleState = false;
    bool m_previousMasterEnabled = false;
    bool m_previousKeybindActive = true;

    struct timespec m_lastFrameTime = {};
    bool m_hasLastFrame = false;
    EGLDisplay m_appliedDisplay = EGL_NO_DISPLAY;
    EGLSurface m_appliedSurface = EGL_NO_SURFACE;
};