#pragma once
#include <bedrocktools/events/AttackEvent.hpp>
namespace bedrocktools::events {
struct AttackResultEvent {
    static constexpr EventType type = EventType::AttackResult;
    AttackResultEvent(AttackKind attackKind, sdk::Actor* attackTarget, bool succeeded)
        : kind(attackKind), target(attackTarget), result(succeeded) {}
    AttackKind kind;
    sdk::Actor* target;
    bool result;
};
}
