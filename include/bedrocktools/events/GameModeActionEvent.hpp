#pragma once

#include <bedrocktools/events/Event.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <cstdint>

namespace bedrocktools::events {

enum class GameModeType : std::uint8_t {
    GameMode,
    SurvivalMode
};

enum class GameModeAction : std::uint8_t {
    StartDestroyBlock,
    StopDestroyBlock,
    StartBuildBlock,
    UseItem,
    UseItemAsAttack,
    UseItemOn,
    Interact,
    Attack
};

struct GameModeActionEvent {
    static constexpr EventType type = EventType::GameModeAction;

    GameModeActionEvent(GameModeType modeType, GameModeAction actionType, void* mode,
                        bool hasResult = false, bool result = false,
                        const sdk::BlockPos* position = nullptr)
        : modeKind(modeType), action(actionType), gameMode(mode),
          hasNativeResult(hasResult), nativeResult(result),
          hasBlockPosition(position != nullptr),
          blockPosition(position ? *position : sdk::BlockPos{}) {}

    GameModeType modeKind;
    GameModeAction action;
    void* gameMode;
    bool hasNativeResult;
    bool nativeResult;
    bool hasBlockPosition;
    sdk::BlockPos blockPosition;
};

}