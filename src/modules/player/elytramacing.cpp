#include "elytramacing.hpp"

#include "ElytraMacingPolicy.hpp"
#include "aimassist.hpp"
#include "autohit.hpp"
#include "../../config/ConfigManager.hpp"
#include "../../core/Diagnostics.hpp"
#include "../../core/InventoryAccess.hpp"
#include "../../core/KeyMapperBridge.hpp"
#include "../ModuleRegistry.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <pl/ModMenu.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <limits>
#include <string>
#include <thread>
#include <utility>

namespace {
using bedrocktools::sdk::AABB;
using bedrocktools::sdk::Player;
using bedrocktools::sdk::Vec3;
using bedrocktools::core::KeyMapperDispatchResult;
using Clock = std::chrono::steady_clock;

bool finiteVec(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool validBounds(const AABB& b) {
    if (!finiteVec(b.min) || !finiteVec(b.max)) return false;
    const float w = b.max.x - b.min.x;
    const float h = b.max.y - b.min.y;
    const float d = b.max.z - b.min.z;
    return w > 0.0f && w <= 5.0f && h >= 0.25f && h <= 8.0f &&
           d > 0.0f && d <= 5.0f;
}

float distanceSquared(const Vec3& a, const Vec3& b) {
    const float x = a.x - b.x;
    const float y = a.y - b.y;
    const float z = a.z - b.z;
    return x * x + y * y + z * z;
}

Vec3 eyePosition(Player* player) {
    const Vec3 position = player->position();
    const AABB bounds = player->bounds();
    if (validBounds(bounds)) {
        return {(bounds.min.x + bounds.max.x) * 0.5f,
                bounds.min.y + (bounds.max.y - bounds.min.y) * 0.85f,
                (bounds.min.z + bounds.max.z) * 0.5f};
    }
    Vec3 eye = position;
    eye.y += 1.5f;
    return eye;
}

float distanceToBounds(Player* player, const AABB& targetBounds) {
    if (!player || !validBounds(targetBounds)) return std::numeric_limits<float>::infinity();
    const Vec3 eye = eyePosition(player);
    if (!finiteVec(eye)) return std::numeric_limits<float>::infinity();
    const Vec3 nearest{
        std::clamp(eye.x, targetBounds.min.x, targetBounds.max.x),
        std::clamp(eye.y, targetBounds.min.y, targetBounds.max.y),
        std::clamp(eye.z, targetBounds.min.z, targetBounds.max.z)
    };
    const float d2 = distanceSquared(eye, nearest);
    return std::isfinite(d2) && d2 >= 0.0f
        ? std::sqrt(d2) : std::numeric_limits<float>::infinity();
}

bool validUid(std::string_view uid) {
    if (uid.empty()) return true;
    if (uid.size() != 36) return false;
    for (std::size_t i = 0; i < uid.size(); ++i) {
        const char c = uid[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') ||
                     (c >= 'a' && c <= 'f') ||
                     (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

bool parseBool(std::string_view value, bool& out) {
    if (value == "true" || value == "1" || value == "on" || value == "yes") {
        out = true;
        return true;
    }
    if (value == "false" || value == "0" || value == "off" || value == "no") {
        out = false;
        return true;
    }
    return false;
}

const char* actionName(ElytraMacingModule::Action action) {
    switch (action) {
        case ElytraMacingModule::Action::StartGlide: return "start_glide";
        case ElytraMacingModule::Action::Firework: return "firework";
        case ElytraMacingModule::Action::Unglide: return "unglide";
        case ElytraMacingModule::Action::LookUp: return "look_up";
        case ElytraMacingModule::Action::None: default: return "none";
    }
}
} // namespace

ElytraMacingModule::ElytraMacingModule()
    : Module("ElytraMacing",
             "Coordinates Elytra approach, firework boosts, mace timing and recovery using Key Mapper.",
             "ElytraMacing") {
    keybind = 0;
}

void ElytraMacingModule::onInit() {
    bedrocktools::core::InventoryAccess::get().initialize();
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [this](auto& event) { processPlayerTick(event.player); });
    note("lifecycle", "initialized",
         "Uses Aim Assist for target/glide state, AutoHit for attacks and Key Mapper for key sequences.");
}

void ElytraMacingModule::onEnable() {
    if (auto* base = ModuleRegistry::get().find("bedrocktools.Aim Assist"))
        static_cast<AimAssistModule*>(base)->setExternalAimOffsetY(0.0f);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    // Leave slot ownership intact if a disable/enable happened before the next tick;
    // the pending cleanup will safely restore the slot against a live player pointer.
    m_phase = Phase::Chase;
    m_phaseStarted = {};
    m_nextRocket = {};
    m_nextGlideAttempt = {};
    m_cycleTarget = nullptr;
    m_hasLastPlayerY = false;
    m_sawFallDuringAttack = false;
    m_stationaryTicks = 0;
    m_glideAttempts = 0;
    m_recoveryGlideAttempts = 0;
    m_unglideAttempts = 0;
    m_boostImmediatelyAfterGlide = false;
    m_waitForSeparation = false;
    m_recoveryLookUpSent = false;
    m_recoveryGlideRequested = false;
    m_cleanupRequested = false;
    note("lifecycle", "enabled");
}

void ElytraMacingModule::onDisable() {
    bool wasSuspended = false;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_cleanupRequested = true;
        ++m_dispatchToken; // stale asynchronous callbacks must not update a new run
        m_dispatchInProgress = false;
        wasSuspended = m_aimSuspended;
        m_aimSuspended = false;
        m_phase = Phase::Chase;
        m_cycleTarget = nullptr;
        m_boostImmediatelyAfterGlide = false;
        m_waitForSeparation = false;
    }
    if (auto* base = ModuleRegistry::get().find("bedrocktools.Aim Assist")) {
        auto* aim = static_cast<AimAssistModule*>(base);
        aim->setExternalAimOffsetY(0.0f);
        if (wasSuspended) aim->setExternalControlSuspended(false);
    }
    note("lifecycle", "disabled", "Slot restoration is deferred to a live player tick.");
}

void ElytraMacingModule::note(std::string_view field, std::string_view state,
                              std::string_view detail) const {
    bedrocktools::diagnostics::probe(
        bedrocktools::diagnostics::Level::Info, "ElytraMacing",
        field, state, detail);
}

void ElytraMacingModule::setAimSuspendedLocked(bool suspended) {
    if (m_aimSuspended == suspended) return;
    m_aimSuspended = suspended;
    auto* base = ModuleRegistry::get().find("bedrocktools.Aim Assist");
    if (base) static_cast<AimAssistModule*>(base)->setExternalControlSuspended(suspended);
    note("aim_assist", suspended ? "temporarily_suspended" : "resumed",
         "Only used around the configured recovery look-up keymap.");
}

void ElytraMacingModule::cleanupManagedSlotLocked(Player* player) {
    if (!m_slotOwned || !player) {
        m_slotOwned = false;
        m_originalSlot = -1;
        m_ownedSlot = -1;
        return;
    }
    auto& inventory = bedrocktools::core::InventoryAccess::get();
    const int selected = inventory.selectedHotbarSlot(player);
    if (m_restoreSlotOnDisable && m_originalSlot >= 0 && selected == m_ownedSlot) {
        const bool restored = inventory.selectHotbarSlot(player, m_originalSlot);
        note("hotbar_restore", restored ? "restored" : "restore_failed");
    } else if (selected != m_ownedSlot) {
        note("hotbar_restore", "user_selection_preserved");
    }
    m_slotOwned = false;
    m_originalSlot = -1;
    m_ownedSlot = -1;
}

bool ElytraMacingModule::selectManagedItemLocked(Player* player,
                                                 std::string_view identifierPart) {
    if (!player || identifierPart.empty()) return false;
    auto& inventory = bedrocktools::core::InventoryAccess::get();
    const int desiredSlot = inventory.findHotbarSlot(player, identifierPart);
    if (desiredSlot < 0) {
        note("inventory", "required_item_missing", identifierPart);
        return false;
    }

    const int selected = inventory.selectedHotbarSlot(player);
    if (selected < 0) return false;
    if (desiredSlot == selected) {
        if (m_slotOwned) m_ownedSlot = selected;
        return true;
    }

    // If the player changed slots manually after our previous selection, relinquish
    // ownership and use their new slot as the restoration point.
    if (m_slotOwned && selected != m_ownedSlot) {
        m_slotOwned = false;
        m_originalSlot = -1;
        m_ownedSlot = -1;
    }
    if (!m_slotOwned) {
        m_originalSlot = selected;
        m_slotOwned = true;
    }

    if (!inventory.selectHotbarSlot(player, desiredSlot)) {
        note("inventory", "hotbar_selection_failed", identifierPart);
        return false;
    }
    m_ownedSlot = desiredSlot;
    note("inventory", "hotbar_item_selected", identifierPart);
    return true;
}

bool ElytraMacingModule::requestKeymapLocked(
    Action action, const std::string& uid, Clock::time_point now) {
    if (m_dispatchInProgress) return false;
    if (!validUid(uid) || uid.empty()) {
        note("keymapper", "missing_or_invalid_uid", actionName(action));
        return false;
    }

    m_dispatchInProgress = true;
    const std::uint64_t token = ++m_dispatchToken;
    try {
        std::thread([this, action, uid, token, now]() {
            const auto result = bedrocktools::core::dispatchKeyMapperKeymap(uid);
            const auto finishedAt = Clock::now();
            bool accepted = result == KeyMapperDispatchResult::Sent;
            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                if (token != m_dispatchToken) return;
                m_dispatchInProgress = false;
                m_lastCompletedAction = action;
                m_lastDispatchSucceeded = accepted;
                m_dispatchCompletedAt = finishedAt;
                if (!accepted) {
                    if (action == Action::Firework)
                        m_nextRocket = finishedAt + std::chrono::milliseconds(750);
                    if (action == Action::StartGlide)
                        m_nextGlideAttempt = finishedAt + std::chrono::milliseconds(800);
                    // Keep the failed lookup marked as completed; the recovery phase
                    // will skip the camera action and continue to the double-jump attempt.
                } else if (action == Action::Firework) {
                    m_nextRocket = finishedAt +
                        std::chrono::milliseconds(std::clamp(m_rocketIntervalMs, 100, 1000));
                    if (m_aimSuspended) setAimSuspendedLocked(false);
                }
            }
            bedrocktools::diagnostics::probe(
                accepted ? bedrocktools::diagnostics::Level::Info :
                           bedrocktools::diagnostics::Level::Warning,
                "ElytraMacing", "keymapper_dispatch",
                bedrocktools::core::keyMapperDispatchResultName(result),
                std::string("action=") + actionName(action) +
                    "; uid=" + uid +
                    "; broadcast accepted is not proof of in-game input delivery");
        }).detach();
    } catch (...) {
        m_dispatchInProgress = false;
        note("keymapper", "thread_creation_failed", actionName(action));
        return false;
    }
    (void)now;
    return true;
}

void ElytraMacingModule::startRecoveryLookUpLocked(Clock::time_point now) {
    m_phase = Phase::RecoveryLookUp;
    m_phaseStarted = now;
    m_recoveryLookUpSent = false;
    m_recoveryGlideRequested = false;
    m_recoveryGlideAttempts = 0;
    if (m_suspendAimDuringRecovery && !m_lookUpKeymapUid.empty())
        setAimSuspendedLocked(true);
    note("recovery", "landing_detected",
         "Beginning look-up and double-jump keymap sequence.");
}

void ElytraMacingModule::startRecoveryGlideLocked(Clock::time_point now) {
    m_phase = Phase::RecoveryGlide;
    m_phaseStarted = now;
    m_recoveryGlideRequested = false;
    if (m_startGlideKeymapUid.empty()) {
        m_recoveryGlideAttempts = 2;
    } else if (!m_dispatchInProgress) {
        m_recoveryGlideRequested = requestKeymapLocked(
            Action::StartGlide, m_startGlideKeymapUid, now);
        if (m_recoveryGlideRequested) ++m_recoveryGlideAttempts;
    }
    note("recovery", "glide_requested",
         m_startGlideKeymapUid.empty() ? "Configure the start-glide UID in Key Mapper settings."
                                       : "Waiting for actual gliding state before the recovery rocket.");
}

void ElytraMacingModule::processPlayerTick(Player* player) {
    using namespace std::chrono_literals;
    if (!player) return;

    std::lock_guard<std::mutex> lock(m_stateMutex);
    const auto now = Clock::now();

    if (m_lastPlayer && player != m_lastPlayer) {
        // A player/world change invalidates queued callbacks. Do not write an old
        // player's inventory slot through a pointer that may already be stale.
        ++m_dispatchToken;
        m_dispatchInProgress = false;
        m_slotOwned = false;
        m_originalSlot = -1;
        m_ownedSlot = -1;
        m_phase = Phase::Chase;
        m_cycleTarget = nullptr;
        m_hasLastPlayerY = false;
        m_sawFallDuringAttack = false;
        m_stationaryTicks = 0;
        if (m_aimSuspended) setAimSuspendedLocked(false);
        note("player", "player_pointer_changed", "Reset flight state; old inventory is not dereferenced.");
    }
    m_lastPlayer = player;

    if (m_cleanupRequested) {
        cleanupManagedSlotLocked(player);
        m_cleanupRequested = false;
        m_phase = Phase::Chase;
        m_cycleTarget = nullptr;
        m_hasLastPlayerY = false;
        m_sawFallDuringAttack = false;
        m_stationaryTicks = 0;
        if (m_aimSuspended) setAimSuspendedLocked(false);
        if (!enabled) return;
    }
    if (!enabled) return;

    const Vec3 position = player->position();
    if (!finiteVec(position)) {
        note("player_motion", "invalid_position");
        return;
    }

    m_verticalDelta = m_hasLastPlayerY ? position.y - m_lastPlayerY : 0.0f;
    m_lastPlayerY = position.y;
    m_hasLastPlayerY = true;

    if (m_phase == Phase::AttackWindow) {
        if (m_verticalDelta < -0.025f) {
            m_sawFallDuringAttack = true;
            m_stationaryTicks = 0;
        } else if (m_sawFallDuringAttack && std::abs(m_verticalDelta) < 0.02f) {
            ++m_stationaryTicks;
        } else if (m_verticalDelta > 0.025f) {
            m_stationaryTicks = 0;
        }
    } else if (m_phase != Phase::RecoveryLookUp && m_phase != Phase::RecoveryGlide) {
        m_stationaryTicks = 0;
    }

    auto* aimBase = ModuleRegistry::get().find("bedrocktools.Aim Assist");
    if (!aimBase || !aimBase->enabled) {
        if (m_aimSuspended) setAimSuspendedLocked(false);
        note("dependency", "aim_assist_disabled",
             "ElytraMacing waits for Aim Assist to be enabled.");
        return;
    }
    auto* aim = static_cast<AimAssistModule*>(aimBase);
    const bool gliding = aim->localPlayerIsGliding();

    void* target = nullptr;
    AABB targetBounds{};
    float aimRange = 0.0f;
    bool targetValid = aim->currentTargetSnapshot(target, targetBounds, aimRange) &&
        target && aim->currentTargetIsPlayerSnapshot(target) &&
        validBounds(targetBounds) && std::isfinite(aimRange) && aimRange > 0.0f;
    const float targetDistance = targetValid
        ? distanceToBounds(player, targetBounds)
        : std::numeric_limits<float>::infinity();
    targetValid = targetValid && std::isfinite(targetDistance);

    const bool shouldAimAbove = bedaim::elytramacing::shouldAimAboveTarget(
        enabled, gliding, targetValid, targetDistance, m_engageDistance,
        m_phase == Phase::Chase);
    aim->setExternalAimOffsetY(shouldAimAbove
        ? std::clamp(m_approachAimOffsetY, 0.0f, 8.0f) : 0.0f);

    if (targetValid) {
        if (m_cycleTarget && m_cycleTarget != target &&
            m_phase == Phase::AttackWindow) {
            m_phase = Phase::Chase;
            m_sawFallDuringAttack = false;
            m_stationaryTicks = 0;
            m_waitForSeparation = false;
            note("target", "target_changed_during_attack_window",
                 "Cancelled the previous target's impact/recovery window.");
        }
        m_cycleTarget = target;
    }

    // In every phase, a genuine upward impulse after the fall means the mace
    // Wind Burst may have launched the player. Try to re-enter glide; the next
    // successful glide immediately schedules the rocket keymap.
    if (m_phase == Phase::AttackWindow) {
        if (m_sawFallDuringAttack &&
            bedaim::elytramacing::detectedWindBurstRise(
                true, gliding, m_verticalDelta, m_windBurstRiseThreshold)) {
            m_phase = Phase::AwaitingGlide;
            m_phaseStarted = now;
            m_glideAttempts = 0;
            m_boostImmediatelyAfterGlide = true;
            m_waitForSeparation = true;
            m_separationDeadline = now + 2500ms;
            m_nextGlideAttempt = now;
            if (!gliding && !m_startGlideKeymapUid.empty()) {
                if (requestKeymapLocked(Action::StartGlide, m_startGlideKeymapUid, now))
                    ++m_glideAttempts;
            }
            note("wind_burst", "upward_impulse_detected",
                 "Requested re-glide; a firework boost follows when gliding is confirmed.");
            return;
        }

        if (bedaim::elytramacing::detectedLandingRecovery(
                true, gliding, m_sawFallDuringAttack,
                m_stationaryTicks, m_landingStableTicks)) {
            startRecoveryLookUpLocked(now);
            return;
        }

        // A failed/missed engagement must not repeatedly send the unglide keymap
        // while remaining in the same glide. Permit one bounded retry only.
        if (gliding && now - m_phaseStarted >= 650ms &&
            m_unglideAttempts < 2 && !m_dispatchInProgress) {
            if (requestKeymapLocked(Action::Unglide, m_unglideKeymapUid, now)) {
                ++m_unglideAttempts;
                m_phaseStarted = now;
                note("mace_window", "unglide_retry", "One retry after no observed glide-state change.");
            }
            return;
        }
        if (now - m_phaseStarted > 2800ms) {
            m_phase = Phase::Chase;
            m_phaseStarted = now;
            m_waitForSeparation = true;
            m_separationDeadline = now + 1800ms;
            m_nextGlideAttempt = now + 250ms;
            note("mace_window", "window_expired",
                 "No launch/landing transition was observed; returning to chase with a re-entry delay.");
        } else {
            return;
        }
    }

    if (m_phase == Phase::RecoveryLookUp) {
        if (m_lookUpKeymapUid.empty()) {
            startRecoveryGlideLocked(now);
            return;
        }
        if (m_dispatchInProgress) return;
        if (m_recoveryLookUpSent && m_lastCompletedAction == Action::LookUp) {
            if (!m_lastDispatchSucceeded ||
                now - m_dispatchCompletedAt >=
                    std::chrono::milliseconds(std::clamp(m_recoveryLookUpWaitMs, 100, 1000))) {
                startRecoveryGlideLocked(now);
            }
            return;
        }
        if (!m_recoveryLookUpSent) {
            m_recoveryLookUpSent = requestKeymapLocked(
                Action::LookUp, m_lookUpKeymapUid, now);
            if (!m_recoveryLookUpSent) {
                // A failed camera macro must not block the double-jump recovery.
                startRecoveryGlideLocked(now);
            }
            return;
        }
        return;
    }

    if (m_phase == Phase::RecoveryGlide) {
        if (gliding) {
            m_phase = Phase::Chase;
            m_phaseStarted = now;
            m_boostImmediatelyAfterGlide = true;
            m_waitForSeparation = true;
            m_separationDeadline = now + 2500ms;
            m_nextRocket = now;
            m_sawFallDuringAttack = false;
            m_stationaryTicks = 0;
            if (!targetValid) setAimSuspendedLocked(false);
            note("recovery", "glide_confirmed", "Recovery rocket is now eligible.");
            return;
        }

        if (!m_dispatchInProgress && m_recoveryGlideRequested &&
            m_lastCompletedAction == Action::StartGlide &&
            now - m_dispatchCompletedAt >= 900ms) {
            if (m_recoveryGlideAttempts < 2) {
                m_recoveryGlideRequested = false;
                m_phaseStarted = now;
            } else {
                setAimSuspendedLocked(false);
                m_phase = Phase::Chase;
                m_nextGlideAttempt = now + 900ms;
                note("recovery", "glide_not_confirmed",
                     "Bounded recovery attempts exhausted; regular fall detection may retry.");
                return;
            }
        }

        if (!m_dispatchInProgress && !m_recoveryGlideRequested &&
            m_recoveryGlideAttempts < 2 && now - m_phaseStarted >= 100ms) {
            m_recoveryGlideRequested = requestKeymapLocked(
                Action::StartGlide, m_startGlideKeymapUid, now);
            if (m_recoveryGlideRequested) {
                ++m_recoveryGlideAttempts;
                m_phaseStarted = now;
            } else if (m_startGlideKeymapUid.empty()) {
                m_recoveryGlideAttempts = 2;
            }
            return;
        }
        if (!m_dispatchInProgress && m_recoveryGlideAttempts >= 2 &&
            now - m_phaseStarted >= 1200ms) {
            setAimSuspendedLocked(false);
            m_phase = Phase::Chase;
            m_nextGlideAttempt = now + 900ms;
            note("recovery", "glide_not_confirmed",
                 "Bounded recovery attempts exhausted; regular fall detection may retry.");
        }
        return;
    }

    if (m_phase == Phase::AwaitingGlide) {
        if (gliding) {
            m_phase = Phase::Chase;
            m_phaseStarted = now;
            m_glideAttempts = 0;
            m_nextRocket = now;
            note("glide", "gliding_confirmed",
                 "Boost is eligible immediately; no state is inferred from the broadcast alone.");
            return;
        }
        if (!m_dispatchInProgress && now - m_phaseStarted >= 1000ms &&
            m_glideAttempts < 2 && now >= m_nextGlideAttempt) {
            m_nextGlideAttempt = now + 800ms;
            m_phaseStarted = now;
            if (!m_startGlideKeymapUid.empty() &&
                requestKeymapLocked(Action::StartGlide, m_startGlideKeymapUid, now)) {
                ++m_glideAttempts;
            } else {
                m_glideAttempts = 2;
            }
            note("glide", "bounded_retry", "Retrying the start-glide keymap at most once.");
            return;
        }
        if (m_glideAttempts >= 2 && !m_dispatchInProgress &&
            now - m_phaseStarted > 1400ms) {
            m_phase = Phase::Chase;
            m_nextGlideAttempt = now + 800ms;
            note("glide", "not_confirmed",
                 "Returning to passive state; retry only after a fresh fall/cooldown.");
        }
        return;
    }

    // When no launch is in progress, a falling player gets one start-glide attempt
    // before target-dependent chase/attack decisions are considered.
    if (!gliding && bedaim::elytramacing::shouldRequestGlide(
            true, false, m_verticalDelta, !m_startGlideKeymapUid.empty(),
            now >= m_nextGlideAttempt)) {
        m_phase = Phase::AwaitingGlide;
        m_phaseStarted = now;
        m_glideAttempts = 0;
        m_boostImmediatelyAfterGlide = true;
        m_nextGlideAttempt = now + 800ms;
        if (requestKeymapLocked(Action::StartGlide, m_startGlideKeymapUid, now))
            ++m_glideAttempts;
        return;
    }

    if (!targetValid) {
        if (m_phase == Phase::Chase && m_slotOwned)
            cleanupManagedSlotLocked(player);
        return;
    }

    if (m_waitForSeparation) {
        if (targetDistance > m_engageDistance + 1.5f ||
            (m_separationDeadline.time_since_epoch().count() != 0 &&
             now >= m_separationDeadline)) {
            m_waitForSeparation = false;
            note("mace_window", "rearm_allowed",
                 "Target separation/hysteresis permits another approach.");
        }
    }

    if (gliding && m_boostImmediatelyAfterGlide && now >= m_nextRocket) {
        if (selectManagedItemLocked(player, "firework_rocket")) {
            if (requestKeymapLocked(Action::Firework, m_fireworkKeymapUid, now)) {
                m_boostImmediatelyAfterGlide = false;
                note("rocket", "recovery_boost_requested");
                return;
            }
        }
        m_nextRocket = now + 1000ms;
    }

    if (gliding && bedaim::elytramacing::shouldBoostTowardTarget(
            true, true, true, targetDistance, m_engageDistance,
            now >= m_nextRocket)) {
        if (selectManagedItemLocked(player, "firework_rocket")) {
            if (requestKeymapLocked(Action::Firework, m_fireworkKeymapUid, now)) {
                note("rocket", "approach_boost_requested",
                     "Uses the configured Firework keymap while Aim Assist tracks the selected player.");
                return;
            }
        }
        m_nextRocket = now + 1000ms;
    }

    auto* autoHitBase = ModuleRegistry::get().find("bedrocktools.Auto Hit");
    auto* autoHit = autoHitBase ? static_cast<AutoHitModule*>(autoHitBase) : nullptr;
    const bool autoHitReady = autoHit && autoHit->enabled &&
        autoHit->hasEnabledProfileFor("minecraft:mace");
    const bool crosshairConfirmed = targetValid &&
        aim->currentTargetInCrosshair(player, target, 0.22f);
    const int maceSlot = bedrocktools::core::InventoryAccess::get().findHotbarSlot(player, "mace");
    const bool maceAvailable = maceSlot >= 0 && autoHitReady;
    if (maceSlot >= 0 && !autoHitReady)
        note("dependency", "mace_profile_disabled",
             "Enable the minecraft:mace profile in AutoHit before ElytraMacing stops gliding.");

    if (bedaim::elytramacing::shouldDisengageForMace(
            true, gliding, targetValid && crosshairConfirmed,
            targetDistance, m_engageDistance, maceAvailable,
            !m_unglideKeymapUid.empty()) && !m_waitForSeparation) {
        if (m_dispatchInProgress) return;
        if (!selectManagedItemLocked(player, "mace")) return;
        m_phase = Phase::AttackWindow;
        m_phaseStarted = now;
        m_sawFallDuringAttack = false;
        m_stationaryTicks = 0;
        m_unglideAttempts = 0;
        if (requestKeymapLocked(Action::Unglide, m_unglideKeymapUid, now)) {
            m_unglideAttempts = 1;
            note("mace_window", "engagement_started",
                 "Mace selected; AutoHit remains responsible for attack timing and target validation.");
        } else {
            m_phase = Phase::Chase;
            m_nextGlideAttempt = now + 400ms;
        }
        return;
    }

    // If the target is near but the aim ray has not settled, keep the glide active
    // rather than prematurely disengaging and risking a miss.
    if (gliding && targetDistance <= m_engageDistance && !crosshairConfirmed) {
        note("target", "waiting_for_crosshair",
             "Target is close; waiting for Aim Assist before stopping glide.");
    }
}

void ElytraMacingModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_startGlideKeymapUid = j.value("m_startGlideKeymapUid", m_startGlideKeymapUid);
    m_fireworkKeymapUid = j.value("m_fireworkKeymapUid", m_fireworkKeymapUid);
    m_lookUpKeymapUid = j.value("m_lookUpKeymapUid", m_lookUpKeymapUid);
    m_unglideKeymapUid = j.value("m_unglideKeymapUid", m_unglideKeymapUid);
    if (!validUid(m_startGlideKeymapUid)) m_startGlideKeymapUid.clear();
    if (!validUid(m_fireworkKeymapUid)) m_fireworkKeymapUid.clear();
    if (!validUid(m_lookUpKeymapUid)) m_lookUpKeymapUid.clear();
    if (!validUid(m_unglideKeymapUid)) m_unglideKeymapUid.clear();
    m_engageDistance = bedaim::elytramacing::normalizeEngageDistance(j.value("m_engageDistance", m_engageDistance));
    const float aimOffset = j.value("m_approachAimOffsetY", m_approachAimOffsetY);
    m_approachAimOffsetY = std::isfinite(aimOffset) ? std::clamp(aimOffset, 0.0f, 8.0f) : 2.5f;
    const float rise = j.value("m_windBurstRiseThreshold", m_windBurstRiseThreshold);
    m_windBurstRiseThreshold = std::isfinite(rise) ? std::clamp(rise, 0.02f, 1.0f) : 0.08f;
    m_rocketIntervalMs = std::clamp(j.value("m_rocketIntervalMs", m_rocketIntervalMs), 100, 1000);
    m_recoveryLookUpWaitMs = std::clamp(j.value("m_recoveryLookUpWaitMs", m_recoveryLookUpWaitMs), 100, 1000);
    m_landingStableTicks = std::clamp(j.value("m_landingStableTicks", m_landingStableTicks), 3, 20);
    m_restoreSlotOnDisable = j.value("m_restoreSlotOnDisable", m_restoreSlotOnDisable);
    m_suspendAimDuringRecovery = j.value("m_suspendAimDuringRecovery", m_suspendAimDuringRecovery);
}

void ElytraMacingModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    j["m_startGlideKeymapUid"] = m_startGlideKeymapUid;
    j["m_fireworkKeymapUid"] = m_fireworkKeymapUid;
    j["m_lookUpKeymapUid"] = m_lookUpKeymapUid;
    j["m_unglideKeymapUid"] = m_unglideKeymapUid;
    j["m_engageDistance"] = m_engageDistance;
    j["m_approachAimOffsetY"] = m_approachAimOffsetY;
    j["m_windBurstRiseThreshold"] = m_windBurstRiseThreshold;
    j["m_rocketIntervalMs"] = m_rocketIntervalMs;
    j["m_recoveryLookUpWaitMs"] = m_recoveryLookUpWaitMs;
    j["m_landingStableTicks"] = m_landingStableTicks;
    j["m_restoreSlotOnDisable"] = m_restoreSlotOnDisable;
    j["m_suspendAimDuringRecovery"] = m_suspendAimDuringRecovery;
}

void ElytraMacingModule::onMenuRegistered() {
    using namespace pl::modmenu;
    static std::atomic<bool> uidSettingsRegistered{false};
    bool expected = false;
    if (uidSettingsRegistered.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        nlohmann::json config;
        saveConfig(config);
        auto value = [&](const char* key, const char* fallback) -> std::string {
            const auto it = config.find(key);
            if (it == config.end() || it->is_null()) return fallback;
            return it->is_string() ? it->get<std::string>() : it->dump();
        };
        ModuleBuilder settings("bedrocktools.elytramacing.keymap_settings", "ElytraMacing Keymapper UIDs");
        settings.modId("Bedaim")
            .description("Paste the UUID of each Key Mapper keymap. Empty UIDs disable that action.")
            .defaultEnabled(true)
            .config("m_startGlideKeymapUid", "Start glide keymap UID (double jump)", ConfigType::Text,
                    value("m_startGlideKeymapUid", ""))
            .config("m_fireworkKeymapUid", "Firework use keymap UID", ConfigType::Text,
                    value("m_fireworkKeymapUid", ""))
            .config("m_lookUpKeymapUid", "Recovery look-up keymap UID", ConfigType::Text,
                    value("m_lookUpKeymapUid", ""))
            .config("m_unglideKeymapUid", "Stop gliding keymap UID", ConfigType::Text,
                    value("m_unglideKeymapUid", "e2225852-1df2-4232-bb26-606f11fc8745"))
            .onConfigChanged([this](std::string_view, std::string_view key, std::string_view item) {
                if (onMenuConfigChanged(key, item))
                    bedrocktools::config::ConfigManager::get().save();
            });
        if (!settings.registerModule()) uidSettingsRegistered.store(false, std::memory_order_release);
    }

    ConfigSchemaBuilder schema;
    nlohmann::json config;
    saveConfig(config);
    auto valueString = [&](const char* key, const char* fallback) -> std::string {
        const auto it = config.find(key);
        if (it == config.end() || it->is_null()) return fallback;
        if (it->is_string()) return it->get<std::string>();
        if (it->is_boolean()) return it->get<bool>() ? "true" : "false";
        return it->dump();
    };
    schema.defaultCategory("flight")
        .category("flight", "Flight", "Approach and mace engagement")
        .category("recovery", "Recovery", "Re-glide after Wind Burst or a missed landing");

    auto slider = [&](const char* key, const char* title, const char* min,
                      const char* max, const char* step, const char* unit,
                      const char* category, const char* def) {
        ConfigNodeV2 n;
        n.id = key; n.key = key; n.title = title; n.category = category;
        n.type = ConfigControlTypeV2::SliderFloat;
        n.defaultValue = def; n.currentValue = valueString(key, def);
        n.hasCurrentValue = true; n.minValue = min; n.maxValue = max;
        n.step = step; n.unit = unit;
        schema.node(std::move(n));
    };
    auto toggle = [&](const char* key, const char* title, const char* category,
                      const char* def) {
        ConfigNodeV2 n;
        n.id = key; n.key = key; n.title = title; n.category = category;
        n.type = ConfigControlTypeV2::Toggle;
        n.defaultValue = def; n.currentValue = valueString(key, def);
        n.hasCurrentValue = true;
        schema.node(std::move(n));
    };

    slider("m_engageDistance", "Mace engagement distance", "2", "10", "0.1", " blocks", "flight", "4.5");
    slider("m_approachAimOffsetY", "Aim above target while gliding", "0", "8", "0.25", " blocks", "flight", "2.5");
    slider("m_rocketIntervalMs", "Rocket interval", "100", "1000", "25", " ms", "flight", "250");
    slider("m_windBurstRiseThreshold", "Wind Burst rise threshold", "0.02", "1.0", "0.02", " blocks/tick", "recovery", "0.08");
    slider("m_recoveryLookUpWaitMs", "Wait after look-up macro", "100", "1000", "25", " ms", "recovery", "300");
    slider("m_landingStableTicks", "Stable ticks to detect landing", "3", "20", "1", " ticks", "recovery", "5");
    toggle("m_restoreSlotOnDisable", "Restore previous hotbar slot", "recovery", "true");
    toggle("m_suspendAimDuringRecovery", "Suspend Aim Assist during look-up", "recovery", "true");

    ConfigNodeV2 keybind;
    keybind.id = "keybind"; keybind.key = "keybind"; keybind.title = "Keybind";
    keybind.category = "flight"; keybind.type = ConfigControlTypeV2::Keybind;
    schema.node(std::move(keybind));
    setConfigSchemaJson(moduleId, schema.toJson());
}

bool ElytraMacingModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    if (key == "m_startGlideKeymapUid" || key == "m_fireworkKeymapUid" ||
        key == "m_lookUpKeymapUid" || key == "m_unglideKeymapUid") {
        if (!validUid(value)) return false;
        std::lock_guard<std::mutex> lock(m_stateMutex);
        std::string next(value);
        if (key == "m_startGlideKeymapUid") m_startGlideKeymapUid = std::move(next);
        else if (key == "m_fireworkKeymapUid") m_fireworkKeymapUid = std::move(next);
        else if (key == "m_lookUpKeymapUid") m_lookUpKeymapUid = std::move(next);
        else m_unglideKeymapUid = std::move(next);
        return true;
    }

    if (key == "m_restoreSlotOnDisable" || key == "m_suspendAimDuringRecovery") {
        bool parsed = false;
        if (!parseBool(value, parsed)) return false;
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (key == "m_restoreSlotOnDisable") m_restoreSlotOnDisable = parsed;
        else m_suspendAimDuringRecovery = parsed;
        return true;
    }

    try {
        std::size_t used = 0;
        const std::string text(value);
        const float number = std::stof(text, &used);
        while (used < text.size() && std::isspace(static_cast<unsigned char>(text[used]))) ++used;
        if (used != text.size() || !std::isfinite(number)) return false;
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (key == "m_engageDistance")
            m_engageDistance = bedaim::elytramacing::normalizeEngageDistance(number);
        else if (key == "m_approachAimOffsetY")
            m_approachAimOffsetY = std::clamp(number, 0.0f, 8.0f);
        else if (key == "m_windBurstRiseThreshold")
            m_windBurstRiseThreshold = std::clamp(number, 0.02f, 1.0f);
        else if (key == "m_rocketIntervalMs")
            m_rocketIntervalMs = std::clamp(static_cast<int>(std::lround(number)), 100, 1000);
        else if (key == "m_recoveryLookUpWaitMs")
            m_recoveryLookUpWaitMs = std::clamp(static_cast<int>(std::lround(number)), 100, 1000);
        else if (key == "m_landingStableTicks")
            m_landingStableTicks = std::clamp(static_cast<int>(std::lround(number)), 3, 20);
        else return false;
        return true;
    } catch (...) {
        return false;
    }
}
