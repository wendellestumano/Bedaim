#include "autosprint.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/VerifiedLocalPlayerPreTickEvent.hpp>
#include <bedrocktools/sdk/input/MoveInput.hpp>

AutoSprintModule::AutoSprintModule()
    : Module("AutoSprint", "Holds Minecraft's native sprint input for you.") {}

void AutoSprintModule::onInit() {
    bedrocktools::events::bus().subscribe<bedrocktools::events::VerifiedLocalPlayerPreTickEvent>([this](auto& event) {
        if (!enabled || !event.player) return;
        auto* input = bedrocktools::sdk::moveInputComponent(event.player);
        if (!input) return;
        input->mRawInputState.set(MoveInputState::Flag::SprintDown, true);
    }, bedrocktools::events::EventPriority::First);
}
