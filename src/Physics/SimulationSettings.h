#pragma once

#include <cstdint>

#include "../PCH.h"

namespace LeashFramework::Physics {
    struct ActorBodyCapsuleSettings {
        RE::NiPoint3 offset{};
        float radius{};
        float width{};
    };

    struct ActorBodySexSettings {
        ActorBodyCapsuleSettings spine{.offset = {0.0F, 3.0F, 0.0F}, .radius = 8.0F, .width = 10.0F};
        ActorBodyCapsuleSettings spine1{.offset = {0.0F, 5.0F, 5.0F}, .radius = 9.0F, .width = 6.0F};
        ActorBodyCapsuleSettings spine2{.offset = {0.0F, 3.0F, 10.0F}, .radius = 10.0F, .width = 10.0F};
        ActorBodyCapsuleSettings neck{.offset = {0.0F, 1.0F, 0.0F}, .radius = 3.0F, .width = 3.0F};
    };

    struct ActorBodyBreastSettings {
        RE::NiPoint3 offset{0.0F, 3.0F, 0.0F};
        float radius{5.0F};
    };

    struct ActorBodyCollisionSettings {
        ActorBodySexSettings male{.spine = {.offset = {0.0F, 5.0F, 0.0F}, .radius = 11.0F, .width = 8.0F},
            .spine1 = {.offset = {0.0F, 5.0F, 5.0F}, .radius = 12.0F, .width = 6.0F},
            .spine2 = {.offset = {0.0F, 3.0F, 10.0F}, .radius = 11.0F, .width = 13.0F},
            .neck = {.offset = {0.0F, 1.0F, 0.0F}, .radius = 4.0F, .width = 4.0F}};
        ActorBodySexSettings female;
        ActorBodyBreastSettings femaleBreast;
    };

    struct SimulationSettings {
        static constexpr std::uint32_t kMinimumConstraintIterations = 1;
        static constexpr std::uint32_t kMaximumConstraintIterations = 128;

        RE::NiPoint3 gravity{0.0F, 0.0F, -980.0F};
        bool collideWithActors{true};
        ActorBodyCollisionSettings actorBodyCollision;
        float damping{0.985F};
        float collisionPadding{0.2F};
        float stretchCompliance{1.0e-7F};
        float snagReleaseStrain{0.02F};
        float snagBlockedDistance{0.1F};
        std::uint32_t constraintIterations{32};
    };
}  // namespace LeashFramework::Physics
