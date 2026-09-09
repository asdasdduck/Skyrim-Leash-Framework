#include "LeashMovementConstraint.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <numbers>
#include <shared_mutex>
#include <unordered_map>

namespace LeashFramework::Movement {
    namespace {
        constexpr float kBoundaryMargin = 8.0F;  // Reserve this many units before the leash limit, capped at 10% of its length.
        constexpr float kBrakingTime = 0.2F;     // Seconds of remaining slack used to cap outward speed; larger values brake earlier.
        constexpr float kMinimumSpeed = 0.5F;   // Constrained speeds below this many units/second become zero to prevent creeping.

        // Controller slot 07 writes 0x1C bytes, not just CommonLib's NiPoint3.
        // Actor's movement update consumes these angles and speed before movement/animation feedback.
        struct NativeMovementOutput {
            RE::NiPoint3 directionAngles;
            float speed{};
            RE::NiPoint3 rotationVelocity;
        };
        static_assert(sizeof(NativeMovementOutput) == 0x1C);
        static_assert(offsetof(NativeMovementOutput, speed) == 0xC);
        static_assert(offsetof(NativeMovementOutput, rotationVelocity) == 0x10);

        struct BoundarySample {
            RE::NiPoint3 actorPosition;
            RE::NiPoint3 collarOffset;
            RE::NiPoint3 anchor;
            RE::NiPoint3 goal;
            float heading{};
            float ropeLength{};
            float maxLength{};
        };

        struct Circle {
            RE::NiPoint3 offset;
            float radius{};
        };

        REL::Relocation<void (*)(RE::MovementControllerNPC*, float, NativeMovementOutput&)> originalMovement;
        std::shared_mutex boundaryMutex;
        std::unordered_map<const RE::MovementControllerNPC*, BoundarySample> boundaries;
        std::atomic_size_t boundaryCount{};
        bool installed{};

        [[nodiscard]] bool IsFinite(const RE::NiPoint3& a_point) {
            return std::isfinite(a_point.x) && std::isfinite(a_point.y) && std::isfinite(a_point.z);
        }

        [[nodiscard]] float WithMargin(float a_length) { return std::max(a_length - std::min(kBoundaryMargin, a_length * 0.1F), 0.0F); }

        [[nodiscard]] RE::NiPoint3 RotateHorizontal(const RE::NiPoint3& a_point, float a_angle) {
            const auto sine = std::sin(a_angle);
            const auto cosine = std::cos(a_angle);
            return {a_point.x * cosine + a_point.y * sine, -a_point.x * sine + a_point.y * cosine, a_point.z};
        }

        void ConstrainVelocity(RE::NiPoint3& a_velocity, const Circle& a_circle, float a_deltaTime) {
            const auto distance = a_circle.offset.Length();
            if (distance > 0.001F) {
                const auto outward = a_circle.offset / distance;
                const auto outwardSpeed = a_velocity.Dot(outward);
                const auto allowedSpeed = std::max(a_circle.radius - distance, 0.0F) / std::max(kBrakingTime, a_deltaTime);
                if (outwardSpeed > allowedSpeed) {
                    a_velocity -= outward * (outwardSpeed - allowedSpeed);
                }
            }

            // Project the next step onto the circle so tangential movement can follow its curvature.
            // Existing separation is left to PullController; this filter only limits requested travel.
            const auto radius = std::max(a_circle.radius, distance);
            const auto next = a_circle.offset + a_velocity * a_deltaTime;
            const auto nextDistance = next.Length();
            if (nextDistance > radius && nextDistance > 0.001F) {
                a_velocity = (next * (radius / nextDistance) - a_circle.offset) / a_deltaTime;
            }
        }

        [[nodiscard]] float AllowedStep(const RE::NiPoint3& a_step, const Circle& a_circle) {
            // Find the segment's exit from this circle after the other circle changed its direction.
            const auto radiusSquared = std::max(a_circle.radius * a_circle.radius, a_circle.offset.SqrLength());
            if ((a_circle.offset + a_step).SqrLength() <= radiusSquared) {
                return 1.0F;
            }
            const auto stepSquared = a_step.SqrLength();
            if (stepSquared <= 0.000001F) {
                return 0.0F;
            }
            const auto along = a_circle.offset.Dot(a_step);
            const auto slackSquared = std::max(radiusSquared - a_circle.offset.SqrLength(), 0.0F);
            const auto root = std::sqrt(along * along + stepSquared * slackSquared);
            const auto fraction = along > 0.0F ? slackSquared / (root + along) : (root - along) / stepSquared;
            return std::clamp(fraction, 0.0F, 1.0F);
        }

        void FilterMovement(RE::MovementControllerNPC* a_controller, float a_deltaTime, NativeMovementOutput& a_output) {
            originalMovement(a_controller, a_deltaTime, a_output);
            if (boundaryCount.load(std::memory_order_relaxed) == 0 || !std::isfinite(a_deltaTime) || a_deltaTime <= 0.0F ||
                !std::isfinite(a_output.speed) || a_output.speed <= 0.0F || !IsFinite(a_output.directionAngles) || !IsFinite(a_output.rotationVelocity) || a_controller->unk1C7) {
                return;
            }
            // Only change planar travel; preserve native pitch/roll movement.
            if (std::abs(a_output.directionAngles.x) > 0.001F || std::abs(a_output.directionAngles.y) > 0.001F) {
                return;
            }

            const std::shared_lock lock{boundaryMutex};
            const auto entry = boundaries.find(a_controller);
            if (entry == boundaries.end()) {
                return;
            }
            const auto& sample = entry->second;
            const auto turn = a_output.rotationVelocity.z * a_deltaTime;
            const auto heading = sample.heading + turn;
            const auto collar = sample.actorPosition + RotateHorizontal(sample.collarOffset, turn);
            const auto worldDirection = heading + a_output.directionAngles.z;
            RE::NiPoint3 velocity{std::sin(worldDirection) * a_output.speed, std::cos(worldDirection) * a_output.speed, 0.0F};
            const auto originalVelocity = velocity;

            auto anchorOffset = collar - sample.anchor;
            const auto ropeLength = WithMargin(sample.ropeLength);
            const auto horizontalReach = std::sqrt(std::max(ropeLength * ropeLength - anchorOffset.z * anchorOffset.z, 0.0F));
            anchorOffset.z = 0.0F;
            auto goalOffset = collar - sample.goal;
            goalOffset.z = 0.0F;
            const Circle physicalBoundary{anchorOffset, horizontalReach};
            const Circle configuredBoundary{goalOffset, WithMargin(sample.maxLength)};
            ConstrainVelocity(velocity, physicalBoundary, a_deltaTime);
            ConstrainVelocity(velocity, configuredBoundary, a_deltaTime);
            const auto step = velocity * a_deltaTime;
            velocity *= std::min(AllowedStep(step, physicalBoundary), AllowedStep(step, configuredBoundary));
            if ((velocity - originalVelocity).SqrLength() <= 0.000001F) {
                return;
            }

            const auto speed = velocity.Length();
            a_output.speed = speed >= kMinimumSpeed ? std::min(speed, a_output.speed) : 0.0F;
            if (a_output.speed > 0.0F) {
                a_output.directionAngles.z = std::remainder(std::atan2(velocity.x, velocity.y) - heading, 2.0F * std::numbers::pi_v<float>);
            }
        }
    }  // namespace

    void InstallLeashMovementConstraint() {
        if (installed) {
            return;
        }
        REL::Relocation<std::uintptr_t> vtable{RE::VTABLE_MovementControllerNPC[0]};
        originalMovement = vtable.write_vfunc(0x7, FilterMovement);
        installed = true;
        SKSE::log::info("Installed native leash movement constraint");
    }

    bool CanConstrainNativeMovement(const RE::Actor& a_actor) {
        const auto* state = a_actor.AsActorState();
        return installed && std::addressof(a_actor) != RE::PlayerCharacter::GetSingleton() && !state->IsFlying() && !state->IsSwimming() &&
               state->GetSitSleepState() == RE::SIT_SLEEP_STATE::kNormal;
    }

    void UpdateLeashMovementConstraint(RE::MovementControllerNPC*& a_binding, RE::Actor& a_actor, const RE::NiPoint3& a_collar, const RE::NiPoint3& a_anchor,
        const RE::NiPoint3& a_goal, float a_ropeLength, float a_maxLength) {
        auto* controller = a_actor.GetActorRuntimeData().movementController.get();
        if (!CanConstrainNativeMovement(a_actor) || !controller || controller->unk1C7 || !IsFinite(a_collar) || !IsFinite(a_anchor) || !IsFinite(a_goal) ||
            !IsFinite(a_actor.GetPosition()) || !std::isfinite(a_actor.GetAngleZ()) || !std::isfinite(a_ropeLength) || a_ropeLength <= 0.0F || !std::isfinite(a_maxLength) || a_maxLength <= 0.0F) {
            ClearLeashMovementConstraint(a_binding);
            return;
        }

        // Movement callbacks may run off-thread. Publish values only; never follow actors, nodes or leash state from the hook.
        const BoundarySample sample{a_actor.GetPosition(), a_collar - a_actor.GetPosition(), a_anchor, a_goal, a_actor.GetAngleZ(), a_ropeLength, a_maxLength};
        const std::unique_lock lock{boundaryMutex};
        if (a_binding && a_binding != controller) {
            boundaries.erase(a_binding);
        }
        boundaries.insert_or_assign(controller, sample);
        a_binding = controller;
        boundaryCount.store(boundaries.size(), std::memory_order_relaxed);
    }

    void ClearLeashMovementConstraint(RE::MovementControllerNPC*& a_binding) {
        if (a_binding) {
            const std::unique_lock lock{boundaryMutex};
            boundaries.erase(a_binding);
            a_binding = nullptr;
            boundaryCount.store(boundaries.size(), std::memory_order_relaxed);
        }
    }
}  // namespace LeashFramework::Movement
