#pragma once

#include <nlohmann/json.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bedrocktools::config {

class ConfigManager {
public:
    static ConfigManager& get() {
        static ConfigManager instance;
        return instance;
    }

    void load();
    void save();
    void saveTo(const std::string& path);
    bool flush();

    std::string getConfigPath() const;
    void setConfigPath(const std::string& path);

private:
    ConfigManager() = default;
    ~ConfigManager();

    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    void ensureWorkerLocked();
    void workerLoop();
    static bool writePayload(const std::string& path, const std::string& payload);

    // Serialize complete snapshots before enqueueing them. Concurrent save() calls
    // must not let an older snapshot overwrite a newer one in the debounce queue.
    std::mutex m_saveMutex;
    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    std::thread m_worker;
    std::string m_configPath;
    std::vector<std::pair<std::string, std::string>> m_pendingWrites;
    std::chrono::steady_clock::time_point m_deadline{};
    std::uint64_t m_queuedGeneration = 0;
    std::uint64_t m_completedGeneration = 0;
    bool m_hasPending = false;
    bool m_lastWriteSucceeded = true;
    bool m_stopping = false;
};

}
