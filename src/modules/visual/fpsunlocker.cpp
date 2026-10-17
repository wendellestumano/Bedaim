#include "fpsunlocker.hpp"
#include <EGL/egl.h>
#include <time.h>
#include <errno.h>
#include <algorithm>

FPSUnlockerModule::FPSUnlockerModule()
    : Module("FPS Unlocker", "Unlocks frame rate beyond VSync. Set target FPS or 0 for unlimited.") {}

void FPSUnlockerModule::onInit() {}

void FPSUnlockerModule::onEnable() {
    m_appliedDisplay = EGL_NO_DISPLAY;
    m_appliedSurface = EGL_NO_SURFACE;
    m_hasLastFrame = false;
}

void FPSUnlockerModule::onDisable() {
    EGLDisplay display = eglGetCurrentDisplay();
    if (display != EGL_NO_DISPLAY) {
        eglSwapInterval(display, 1);
    }
    m_appliedDisplay = EGL_NO_DISPLAY;
    m_appliedSurface = EGL_NO_SURFACE;
    m_hasLastFrame = false;
}

void FPSUnlockerModule::onFrame() {
    EGLDisplay display = eglGetCurrentDisplay();
    EGLSurface surface = eglGetCurrentSurface(EGL_DRAW);
    if (display != EGL_NO_DISPLAY && surface != EGL_NO_SURFACE &&
        (display != m_appliedDisplay || surface != m_appliedSurface)) {
        if (eglSwapInterval(display, 0) == EGL_TRUE) {
            m_appliedDisplay = display;
            m_appliedSurface = surface;
        }
    }

    if (targetFPS <= 0) {
        m_hasLastFrame = false;
        return;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (m_hasLastFrame) {
        // Keep pacing stable without trying to "catch up" from a deadline that
        // is already in the past. Catch-up deadlines can cause several frames to
        // run back-to-back after a slow frame and produce uneven pacing.
        const int safeTargetFPS = std::clamp(targetFPS, 1, 1000);
        const long long frameDurationNs = 1000000000LL / safeTargetFPS;

        struct timespec deadline = m_lastFrameTime;
        deadline.tv_nsec += frameDurationNs;
        while (deadline.tv_nsec >= 1000000000L) {
            ++deadline.tv_sec;
            deadline.tv_nsec -= 1000000000L;
        }

        const bool deadlinePassed = deadline.tv_sec < now.tv_sec ||
            (deadline.tv_sec == now.tv_sec && deadline.tv_nsec <= now.tv_nsec);
        if (deadlinePassed) {
            // This frame has already exceeded its budget; rebase at the current
            // time rather than repeatedly issuing sleeps for past deadlines.
            m_lastFrameTime = now;
            return;
        }

        int sleepResult = 0;
        do {
            sleepResult = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr);
        } while (sleepResult == EINTR);

        m_lastFrameTime = deadline;
    } else {
        m_lastFrameTime = now;
        m_hasLastFrame = true;
    }
}

void FPSUnlockerModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("targetFPS") && j["targetFPS"].is_number_integer()) {
        targetFPS = std::clamp(j["targetFPS"].get<int>(), 0, 1000);
    }
}

void FPSUnlockerModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["targetFPS"] = targetFPS;
}
