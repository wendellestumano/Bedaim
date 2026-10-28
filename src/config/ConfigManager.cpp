#include "ConfigManager.hpp"
#include "AtomicConfigWriter.hpp"
#include "../core/Diagnostics.hpp"
#include "modules/ModuleRegistry.hpp"
#include <pl/Logger.hpp>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <stdexcept>

namespace bedrocktools::config {

ConfigManager::~ConfigManager() {
    flush();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
    }
    m_condition.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

std::string ConfigManager::getConfigPath() const {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_configPath.empty()) return m_configPath;
    }
    return "/sdcard/games/BedrockTools/config.json";
}

void ConfigManager::setConfigPath(const std::string& path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_configPath = path;
}

void ConfigManager::load() {
    std::string path = getConfigPath();
    bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info, "Config", "loading configuration from " + path);
    if (!std::filesystem::exists(path)) {
        save();
        flush();
        return;
    }

    try {
        std::ifstream inFile(path);
        nlohmann::json j;
        inFile >> j;

        if (j.contains("Modules")) {
            auto& modulesObj = j["Modules"];
            for (auto* mod : ModuleRegistry::get().modules()) {
                if (modulesObj.contains(mod->name)) {
                    mod->loadConfig(modulesObj[mod->name]);
                } else if (std::string_view(mod->name) == "Hive Utils" && modulesObj.contains("AutoReQ")) {
                    mod->loadConfig(modulesObj["AutoReQ"]);
                }
            }
        }
        bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info, "Config", "configuration loaded");
    } catch (const std::exception& ex) {
        bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Error, "Config", std::string("configuration load failed: ") + ex.what());
    } catch (...) {
        bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Error, "Config", "configuration load failed with unknown exception");
    }
}

void ConfigManager::save() {
    saveTo(getConfigPath());
}

void ConfigManager::saveTo(const std::string& targetPath) {
    if (targetPath.empty()) return;
    // Serialize complete snapshots before enqueueing them. Concurrent save() calls
    // must not let an older snapshot overwrite a newer one in the debounce queue.
    std::lock_guard<std::mutex> snapshotGuard(m_saveMutex);
    nlohmann::json j;

    for (auto* mod : ModuleRegistry::get().modules()) {
        nlohmann::json modJson;
        mod->saveConfig(modJson);
        j["Modules"][mod->name] = std::move(modJson);
    }

    std::string payload = j.dump();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) return;
        ensureWorkerLocked();
        m_pendingPath = targetPath;
        m_pendingPayload = std::move(payload);
        m_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
        ++m_queuedGeneration;
        m_hasPending = true;
    }
    m_condition.notify_all();
}

bool ConfigManager::flush() {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_worker.joinable()) return true;

    // m_hasPending becomes false when the worker takes a payload, before the
    // actual disk write finishes. Wait for the requested generation and report
    // its latest write status rather than treating an empty queue as success.
    const std::uint64_t targetGeneration = m_queuedGeneration;
    if (m_completedGeneration >= targetGeneration && !m_hasPending)
        return m_lastWriteSucceeded;

    m_deadline = std::chrono::steady_clock::now();
    m_condition.notify_all();
    m_condition.wait(lock, [&] {
        return m_completedGeneration >= targetGeneration || m_stopping;
    });
    return m_completedGeneration >= targetGeneration && m_lastWriteSucceeded;
}

void ConfigManager::ensureWorkerLocked() {
    if (!m_worker.joinable()) m_worker = std::thread(&ConfigManager::workerLoop, this);
}

void ConfigManager::workerLoop() {
    std::unique_lock<std::mutex> lock(m_mutex);
    while (true) {
        m_condition.wait(lock, [&] { return m_stopping || m_hasPending; });
        if (m_stopping && !m_hasPending) break;

        const std::uint64_t generation = m_queuedGeneration;
        const auto deadline = m_deadline;
        if (!m_stopping && m_condition.wait_until(lock, deadline, [&] {
            return m_stopping || m_queuedGeneration != generation || m_deadline != deadline;
        })) {
            continue;
        }

        std::string path = std::move(m_pendingPath);
        std::string payload = std::move(m_pendingPayload);
        m_hasPending = false;
        lock.unlock();
        const bool writeSucceeded = writePayload(path, payload);
        lock.lock();
        m_lastWriteSucceeded = writeSucceeded;
        if (m_completedGeneration < generation) m_completedGeneration = generation;
        m_condition.notify_all();
    }
}

bool ConfigManager::writePayload(const std::string& pathValue, const std::string& payload) {
    const bool succeeded = writePayloadAtomically(pathValue, payload);
    if (!succeeded) {
        pl::log::Logger::getOrCreate("BedaimConfig").error(
            "Could not atomically save config to '{}'; previous config was preserved", pathValue);
        bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Error, "Config",
            "could not atomically save config to '" + pathValue + "'; previous config was preserved");
    }
    return succeeded;
}

}
