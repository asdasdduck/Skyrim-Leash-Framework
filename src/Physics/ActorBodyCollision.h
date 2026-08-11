#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "../PCH.h"
#include "SimulationSettings.h"

namespace LeashFramework::Physics {
    class ActorBodyCollision {
    public:
        struct ShapeKey {
            std::uint32_t actorFormID{};
            std::uint8_t shapeIndex{};

            [[nodiscard]] bool operator==(const ShapeKey&) const = default;
        };

        struct Hit {
            ShapeKey shape;
            RE::NiPoint3 normal;
            float fraction{};
            float penetration{};
        };

        void Update(const ActorBodyCollisionSettings& a_settings);
        void DrawDebug() const;
        void Clear();
        [[nodiscard]] std::optional<Hit> FindDeepestOverlap(const RE::bhkWorld* a_world, const RE::NiPoint3& a_position, float a_radius, float a_interpolation,
            std::span<const ShapeKey> a_preferredShapes) const;
        [[nodiscard]] std::optional<Hit> SweepSphere(const RE::bhkWorld* a_world, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, float a_radius, float a_interpolation,
            std::span<const ShapeKey> a_preferredShapes) const;

    private:
        enum class Bone : std::size_t { kSpine, kSpine1, kSpine2, kNeck, kTotal };

        struct Capsule {
            RE::NiPoint3 start;
            RE::NiPoint3 end;
            float radius{};
        };

        struct Sphere {
            RE::NiPoint3 center;
            float radius{};
        };

        struct Bounds {
            RE::NiPoint3 minimum;
            RE::NiPoint3 maximum;
        };

        struct Body {
            static constexpr std::size_t kMaximumCapsules = 4;
            static constexpr std::size_t kMaximumSpheres = 2;

            std::array<Capsule, kMaximumCapsules> capsules{};
            std::array<Sphere, kMaximumSpheres> spheres{};
            Bounds bounds{};
            std::size_t capsuleCount{};
            std::size_t sphereCount{};
        };

        struct ActorProxy {
            std::uint32_t formID{};
            RE::ActorHandle actor;
            RE::bhkWorld* world{};
            Bounds bounds{};
            bool hasWorldBound{};
            std::size_t previousBodyIndex{};
            bool hasPreviousBody{};
            mutable std::size_t bodyIndex{};
            mutable bool bodyInitializationAttempted{};
            mutable bool bodyInitialized{};
        };

        struct PreviousBody {
            std::uint32_t formID{};
            RE::bhkWorld* world{};
            Body body;
        };

        void UpdateActor(RE::Actor* a_actor);
        [[nodiscard]] const Body* GetBody(const ActorProxy& a_proxy) const;
        [[nodiscard]] std::optional<Body> BuildBody(RE::Actor& a_actor, RE::NiAVObject& a_root) const;
        [[nodiscard]] static bool Intersects(const Bounds& a_bounds, const RE::NiPoint3& a_minimum, const RE::NiPoint3& a_maximum);

        std::vector<ActorProxy> _actors;
        mutable std::vector<Body> _bodies;
        std::vector<PreviousBody> _previousBodies;
        ActorBodyCollisionSettings _settings;
    };
}  // namespace LeashFramework::Physics
