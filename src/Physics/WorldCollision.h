#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "../PCH.h"
#include "ActorBodyCollision.h"

namespace LeashFramework::Physics {
    class WorldCollision {
    public:
        static constexpr std::size_t kMaximumContacts = 4;

        struct Contact {
            RE::NiPoint3 planePoint;
            RE::NiPoint3 normal;
            ActorBodyCollision::ShapeKey actorShape;
            bool movingSurface{};
        };

        struct Result {
            RE::NiPoint3 position;
            std::array<Contact, kMaximumContacts> contacts{};
            std::size_t contactCount{};
            bool collided{};
        };

        [[nodiscard]] static Result ResolveMovement(RE::bhkWorld* a_world, const ActorBodyCollision* a_actorCollision, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, float a_radius,
            float a_actorInterpolation, std::span<const ActorBodyCollision::ShapeKey> a_preferredActorShapes);
    };
}  // namespace LeashFramework::Physics
