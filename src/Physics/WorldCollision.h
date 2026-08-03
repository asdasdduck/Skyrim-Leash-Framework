#pragma once

#include <array>
#include <cstddef>

#include "../PCH.h"

namespace LeashFramework::Physics {
    class ActorBodyCollision;

    class WorldCollision {
    public:
        static constexpr std::size_t kMaximumContacts = 4;

        struct Contact {
            RE::NiPoint3 planePoint;
            RE::NiPoint3 normal;
            bool movingSurface{};
        };

        struct Result {
            RE::NiPoint3 position;
            std::array<Contact, kMaximumContacts> contacts{};
            std::size_t contactCount{};
            bool collided{};
        };

        [[nodiscard]] static Result ResolveMovement(RE::bhkWorld* a_world, const ActorBodyCollision* a_actorCollision, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, float a_radius);
    };
}  // namespace LeashFramework::Physics
