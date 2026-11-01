#pragma once

#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <entt/entt.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

template <std::size_t N, class T>
struct BedrockBitset {
    T value{};

    bool test(std::size_t index) const {
        return index < N && (value & (T{1} << index)) != 0;
    }

    void set(std::size_t index, bool enabled) {
        if (index >= N) return;
        const T mask = T{1} << index;
        if (enabled) value |= mask;
        else value &= static_cast<T>(~mask);
    }
};

struct MoveInputState {
    enum class Flag : std::uint8_t {
        SneakDown = 0,
        SneakToggleDown = 1,
        WantDownSlow = 2,
        WantUpSlow = 3,
        BlockSelectDown = 4,
        AscendBlock = 5,
        DescendBlock = 6,
        JumpDown = 7,
        SprintDown = 8,
        UpLeft = 9,
        UpRight = 10,
        DownLeft = 11,
        DownRight = 12,
        Up = 13,
        Down = 14,
        Left = 15,
        Right = 16,
        Ascend = 17,
        Descend = 18,
        ChangeHeight = 19,
        LookCenter = 20,
        SneakInputCurrentlyDown = 21,
        SneakInputWasReleased = 22,
        SneakInputWasPressed = 23,
        JumpInputWasReleased = 24,
        JumpInputWasPressed = 25,
        JumpInputCurrentlyDown = 26
    };

    BedrockBitset<27, std::uint32_t> mFlagValues;
    bedrocktools::sdk::Vec2 mAnalogMoveVector;
    std::uint8_t mLookSlightDirField;
    std::uint8_t mLookNormalDirField;
    std::uint8_t mLookSmoothDirField;
    std::uint8_t mPadding;

    bool test(Flag flag) const {
        return mFlagValues.test(static_cast<std::size_t>(flag));
    }

    void set(Flag flag, bool enabled) {
        mFlagValues.set(static_cast<std::size_t>(flag), enabled);
    }
};

struct MoveInputComponent {
    enum class Flag : std::uint8_t {
        Sneaking = 0,
        Sprinting = 1,
        WantUp = 2,
        WantDown = 3,
        Jumping = 4,
        AutoJumpingInWater = 5,
        MoveInputStateLocked = 6,
        PersistSneak = 7,
        AutoJumpEnabled = 8,
        IsCameraRelativeMovementEnabled = 9,
        IsRotControlledByMoveDirection = 10
    };

    MoveInputState mInputState;
    MoveInputState mRawInputState;
    std::uint8_t mHoldAutoJumpInWaterTicks;
    std::uint8_t mPadding[3];
    bedrocktools::sdk::Vec2 mMove;
    bedrocktools::sdk::Vec2 mLookDelta;
    bedrocktools::sdk::Vec2 mInteractDir;
    bedrocktools::sdk::Vec3 mDisplacement;
    bedrocktools::sdk::Vec3 mDisplacementDelta;
    bedrocktools::sdk::Vec3 mCameraOrientation;
    BedrockBitset<11, std::uint16_t> mFlagValues;
    std::array<bool, 2> mIsPaddling;
};

enum class EntityId : std::uint32_t {};

struct EntityIdTraits {
    using value_type = EntityId;
    using entity_type = std::uint32_t;
    using version_type = std::uint16_t;
    static constexpr std::uint32_t entity_mask = 0x3FFFF;
    static constexpr std::uint32_t version_mask = 0x3FFF;
};

namespace entt {
template <>
struct entt_traits<EntityId> : basic_entt_traits<EntityIdTraits> {
    static constexpr std::size_t page_size = ENTT_SPARSE_PAGE;
};
}

struct GlidingTravelFlagComponent final {};
struct AirTravelFlagComponent final {};
struct GroundTravelFlagComponent final {};
struct WaterTravelFlagComponent final {};
struct LavaTravelFlagComponent final {};
struct LiquidTravelFlagComponent final {};
struct MobTravelComponent final {};
struct PlayerFlyingTravelComponent final {};
struct FallFlyTicksComponent final {};
struct ElytraFlightTimeTicksComponent final {};
struct ActorDataFlagComponent final {};
struct PlayerActionComponent final {};
struct MovementAbilitiesComponent final {};
struct ArmorFlyEnabledFlagComponent final {};
struct ActorMovementTickNeededComponent final {};
struct PlayerInputRequestComponent final {};
struct OnGroundFlagComponent final {};
struct WasInWaterFlagComponent final {};
struct PlayerComponent final {};
struct LocalPlayerComponent final {};

static_assert(entt::type_hash<GlidingTravelFlagComponent>::value() == 0x27a011eeu,
              "GlidingTravelFlagComponent EnTT hash must match Minecraft 26.60");

class EntityRegistry;

class EntityContext {
public:
    entt::basic_registry<EntityId>& getRegistry() {
        return mEnTTRegistry;
    }

    template <class T>
    T* tryGetComponent() {
        return getRegistry().try_get<T>(mEntity);
    }

    EntityRegistry& mRegistry;
    entt::basic_registry<EntityId>& mEnTTRegistry;
    EntityId const mEntity;
};

static_assert(sizeof(MoveInputState) == 0x10);
static_assert(offsetof(MoveInputState, mAnalogMoveVector) == 0x4);
static_assert(offsetof(MoveInputComponent, mRawInputState) == 0x10);
static_assert(offsetof(MoveInputComponent, mMove) == 0x24);
static_assert(offsetof(MoveInputComponent, mFlagValues) == 0x60);
static_assert(sizeof(MoveInputComponent) == 0x64);

namespace bedrocktools::sdk {

struct EntityContextProbe {
    bool context_address_valid = false;
    bool move_input_pool_present = false;
    bool move_input_entity_present = false;
    bool move_input_try_get_present = false;
    bool gliding_pool_present = false;
    bool gliding_entity_present = false;
    bool air_travel_pool_present = false;
    bool air_travel_entity_present = false;
    bool ground_travel_pool_present = false;
    bool ground_travel_entity_present = false;
    bool water_travel_pool_present = false;
    bool water_travel_entity_present = false;
    bool lava_travel_pool_present = false;
    bool lava_travel_entity_present = false;
    bool liquid_travel_pool_present = false;
    bool liquid_travel_entity_present = false;
    bool mob_travel_pool_present = false;
    bool mob_travel_entity_present = false;
    bool player_flying_pool_present = false;
    bool player_flying_entity_present = false;
    bool fall_fly_ticks_pool_present = false;
    bool fall_fly_ticks_entity_present = false;
    bool elytra_flight_ticks_pool_present = false;
    bool elytra_flight_ticks_entity_present = false;
    bool actor_data_flags_pool_present = false;
    bool actor_data_flags_entity_present = false;
    bool player_action_pool_present = false;
    bool player_action_entity_present = false;
    bool movement_abilities_pool_present = false;
    bool movement_abilities_entity_present = false;
    bool armor_fly_pool_present = false;
    bool armor_fly_entity_present = false;
    bool movement_tick_pool_present = false;
    bool movement_tick_entity_present = false;
    bool input_request_pool_present = false;
    bool input_request_entity_present = false;
    bool on_ground_pool_present = false;
    bool on_ground_entity_present = false;
    bool was_in_water_pool_present = false;
    bool was_in_water_entity_present = false;
    bool player_pool_present = false;
    bool player_entity_present = false;
    bool local_player_pool_present = false;
    bool local_player_entity_present = false;
    std::uint32_t entity_value = 0;
};

inline EntityContextProbe probeEntityContext(void* actor) {
    EntityContextProbe result{};
    if (!actor) return result;

    const auto actorAddress = reinterpret_cast<std::uintptr_t>(actor);
    constexpr auto contextOffset = offsets::Actor::mEntityContext;
    if (actorAddress <= 0x1000 ||
        actorAddress > std::numeric_limits<std::uintptr_t>::max() - contextOffset - sizeof(EntityContext)) {
        return result;
    }

    auto* context = reinterpret_cast<EntityContext*>(actorAddress + contextOffset);
    result.context_address_valid = true;
    result.entity_value = static_cast<std::uint32_t>(context->mEntity);

    auto& registry = context->getRegistry();
    constexpr auto moveInputTypeId = entt::type_hash<MoveInputComponent>::value();
    const auto* moveInputPool = registry.storage(moveInputTypeId);
    result.move_input_pool_present = moveInputPool != nullptr;
    result.move_input_entity_present = moveInputPool && moveInputPool->contains(context->mEntity);
    result.move_input_try_get_present = context->tryGetComponent<MoveInputComponent>() != nullptr;

    constexpr auto glidingTypeId = entt::type_hash<GlidingTravelFlagComponent>::value();
    const auto* glidingPool = registry.storage(glidingTypeId);
    result.gliding_pool_present = glidingPool != nullptr;
    result.gliding_entity_present = glidingPool && glidingPool->contains(context->mEntity);

#define BEDROCK_PROBE_ENTITY_COMPONENT(Type, prefix) \
    do { \
        constexpr auto typeId = entt::type_hash<Type>::value(); \
        const auto* pool = registry.storage(typeId); \
        result.prefix##_pool_present = pool != nullptr; \
        result.prefix##_entity_present = pool && pool->contains(context->mEntity); \
    } while (false)

    BEDROCK_PROBE_ENTITY_COMPONENT(AirTravelFlagComponent, air_travel);
    BEDROCK_PROBE_ENTITY_COMPONENT(GroundTravelFlagComponent, ground_travel);
    BEDROCK_PROBE_ENTITY_COMPONENT(WaterTravelFlagComponent, water_travel);
    BEDROCK_PROBE_ENTITY_COMPONENT(LavaTravelFlagComponent, lava_travel);
    BEDROCK_PROBE_ENTITY_COMPONENT(LiquidTravelFlagComponent, liquid_travel);
    BEDROCK_PROBE_ENTITY_COMPONENT(MobTravelComponent, mob_travel);
    BEDROCK_PROBE_ENTITY_COMPONENT(PlayerFlyingTravelComponent, player_flying);
    BEDROCK_PROBE_ENTITY_COMPONENT(FallFlyTicksComponent, fall_fly_ticks);
    BEDROCK_PROBE_ENTITY_COMPONENT(ElytraFlightTimeTicksComponent, elytra_flight_ticks);
    BEDROCK_PROBE_ENTITY_COMPONENT(ActorDataFlagComponent, actor_data_flags);
    BEDROCK_PROBE_ENTITY_COMPONENT(PlayerActionComponent, player_action);
    BEDROCK_PROBE_ENTITY_COMPONENT(MovementAbilitiesComponent, movement_abilities);
    BEDROCK_PROBE_ENTITY_COMPONENT(ArmorFlyEnabledFlagComponent, armor_fly);
    BEDROCK_PROBE_ENTITY_COMPONENT(ActorMovementTickNeededComponent, movement_tick);
    BEDROCK_PROBE_ENTITY_COMPONENT(PlayerInputRequestComponent, input_request);
    BEDROCK_PROBE_ENTITY_COMPONENT(OnGroundFlagComponent, on_ground);
    BEDROCK_PROBE_ENTITY_COMPONENT(WasInWaterFlagComponent, was_in_water);
    BEDROCK_PROBE_ENTITY_COMPONENT(PlayerComponent, player);
    BEDROCK_PROBE_ENTITY_COMPONENT(LocalPlayerComponent, local_player);
#undef BEDROCK_PROBE_ENTITY_COMPONENT

    return result;
}

inline MoveInputComponent* moveInputComponent(void* actor) {
    if (!actor) return nullptr;
    auto* context = reinterpret_cast<EntityContext*>(
        reinterpret_cast<std::uintptr_t>(actor) + offsets::Actor::mEntityContext
    );
    return context->tryGetComponent<MoveInputComponent>();
}

// 26.60 stores the active travel mode as an ECS tag component. Query the
// existing pool by EnTT's type hash instead of using storage<T>(), which can
// create an empty pool when the runtime has not registered this component yet.
// The global type name intentionally matches the one embedded in libminecraftpe.so.
inline std::optional<bool> glidingTravelFlagComponent(void* actor) {
    if (!actor) return std::nullopt;

    const auto actorAddress = reinterpret_cast<std::uintptr_t>(actor);
    constexpr auto contextOffset = offsets::Actor::mEntityContext;
    if (actorAddress <= 0x1000 ||
        actorAddress > std::numeric_limits<std::uintptr_t>::max() - contextOffset - sizeof(EntityContext)) {
        return std::nullopt;
    }

    auto* context = reinterpret_cast<EntityContext*>(actorAddress + contextOffset);
    auto& registry = context->getRegistry();
    constexpr auto componentTypeId = entt::type_hash<GlidingTravelFlagComponent>::value();
    const auto* componentPool = registry.storage(componentTypeId);
    if (!componentPool) return std::nullopt;

    return componentPool->contains(context->mEntity);
}

}
