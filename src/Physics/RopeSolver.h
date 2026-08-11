#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "../PCH.h"
#include "ActorBodyCollision.h"
#include "SimulationSettings.h"

namespace LeashFramework::Physics {
    class RopeSolver {
    public:
        void Reset();
        void Freeze();
        [[nodiscard]] const std::vector<RE::NiPoint3>& GetPositions() const;
        [[nodiscard]] const std::vector<RE::NiPoint3>& Solve(std::span<const RE::NiPoint3> a_neutralPositions, std::span<const float> a_segmentLengths, const RE::NiPoint3& a_endAnchor, float a_deltaTime,
            RE::bhkWorld* a_world, const ActorBodyCollision* a_actorCollision, const SimulationSettings& a_settings);

    private:
        static constexpr std::size_t kMaximumContactConstraints = 4;

        struct ContactConstraint {
            RE::NiPoint3 planePoint;
            RE::NiPoint3 normal;
            ActorBodyCollision::ShapeKey actorShape;
            std::uint8_t missedQueries{};
            bool movingSurface{};
        };

        void ApplyConstraints(std::span<const float> a_segmentLengths, const RE::NiPoint3& a_startAnchor, const RE::NiPoint3& a_endAnchor, float a_complianceScale, bool a_reverse);
        void ApplyContactConstraints(std::size_t a_index, const ActorBodyCollision* a_actorCollision, RE::bhkWorld* a_world, float a_actorInterpolation, float a_radius);
        void ResolveCollisions(RE::bhkWorld* a_world, const ActorBodyCollision* a_actorCollision, float a_actorInterpolation, float a_radius, std::span<const float> a_segmentLengths, float a_snagDeltaTime,
            float a_snagReleaseStrain, float a_snagBlockedDistance);

        std::vector<RE::NiPoint3> _positions;
        std::vector<RE::NiPoint3> _previousPositions;
        std::vector<RE::NiPoint3> _substepStart;
        std::vector<float> _constraintMultipliers;
        std::vector<std::array<ContactConstraint, kMaximumContactConstraints>> _contactConstraints;
        std::vector<std::size_t> _contactConstraintCounts;
        std::vector<float> _contactBlockedDistances;
        std::vector<float> _blockedContactTimes;
        std::vector<float> _collisionReleaseTimes;
        std::vector<bool> _collisionReleased;
    };
}  // namespace LeashFramework::Physics
