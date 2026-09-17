#include "LeashMovementConstraint.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <numbers>
#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <vector>

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
            const void* owner{};
            RE::NiPoint3 actorPosition;
            RE::NiPoint3 attachmentOffset;
            RE::NiPoint3 anchor;
            RE::NiPoint3 goal;
            float heading{};
            float ropeLength{};
            float maxLength{};
            bool holder{};
            bool player{};
        };

        struct Circle {
            RE::NiPoint3 offset;
            float radius{};
        };

        REL::Relocation<void (*)(RE::MovementControllerNPC*, float, NativeMovementOutput&)> originalMovement;
        REL::Relocation<void (*)(RE::Actor*, float, RE::NiPoint3&, RE::NiPoint3&)> originalNPCMovement;
        std::shared_mutex boundaryMutex;
        std::unordered_map<const RE::MovementControllerNPC*, std::vector<BoundarySample>> boundaries;
        std::atomic_size_t boundaryCount{};
        bool preventHolderOverstretch{};
        float holderStretchAllowance{};
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

        [[nodiscard]] bool CanFilterMovement(const RE::Actor& a_actor) {
            const auto* state = a_actor.AsActorState();
            return installed && !a_actor.IsDead(false) && !a_actor.IsInRagdollState() && state->GetKnockState() == RE::KNOCK_STATE_ENUM::kNormal &&
                   !state->IsFlying() && !state->IsSwimming() && state->GetSitSleepState() == RE::SIT_SLEEP_STATE::kNormal;
        }

        void PublishBoundary(RE::MovementControllerNPC*& a_binding, RE::MovementControllerNPC* a_controller, BoundarySample a_sample) {
            if (a_binding != a_controller) {
                ClearLeashMovementConstraint(a_binding);
            }
            // The binding's stable address identifies one leash, even when several share a movement controller.
            a_sample.owner = std::addressof(a_binding);
            const std::unique_lock lock{boundaryMutex};
            auto& samples = boundaries[a_controller];
            const auto entry = std::ranges::find(samples, a_sample.owner, &BoundarySample::owner);
            if (entry == samples.end()) {
                samples.push_back(a_sample);
            } else {
                *entry = a_sample;
            }
            a_binding = a_controller;
            boundaryCount.store(boundaries.size(), std::memory_order_relaxed);
        }

        template <class Visitor>
        void VisitCircles(const BoundarySample& a_sample, float a_turn, Visitor&& a_visitor) {
            const auto attachment = a_sample.actorPosition + RotateHorizontal(a_sample.attachmentOffset, a_turn);
            auto anchorOffset = attachment - a_sample.anchor;
            const auto ropeLength = a_sample.holder ? a_sample.ropeLength : WithMargin(a_sample.ropeLength);
            const auto horizontalReach = std::sqrt(std::max(ropeLength * ropeLength - anchorOffset.z * anchorOffset.z, 0.0F));
            anchorOffset.z = 0.0F;
            a_visitor(Circle{anchorOffset, horizontalReach});
            if (!a_sample.holder) {
                auto goalOffset = attachment - a_sample.goal;
                goalOffset.z = 0.0F;
                a_visitor(Circle{goalOffset, WithMargin(a_sample.maxLength)});
            }
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

        void ConstrainHorizontalVelocity(std::span<const BoundarySample> a_samples, RE::NiPoint3& a_velocity, float a_deltaTime, float a_turn) {
            const auto constrainHolder = preventHolderOverstretch;
            for (const auto& sample : a_samples) {
                if (!sample.holder || constrainHolder) {
                    VisitCircles(sample, a_turn, [&](const Circle& a_circle) { ConstrainVelocity(a_velocity, a_circle, a_deltaTime); });
                }
            }
            const auto step = a_velocity * a_deltaTime;
            float allowedStep = 1.0F;
            for (const auto& sample : a_samples) {
                if (!sample.holder || constrainHolder) {
                    VisitCircles(sample, a_turn, [&](const Circle& a_circle) { allowedStep = std::min(allowedStep, AllowedStep(step, a_circle)); });
                }
            }
            a_velocity *= allowedStep;
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
            if (entry == boundaries.end() || !entry->second.front().player) {
                return;
            }
            const auto& samples = entry->second;
            const auto turn = a_output.rotationVelocity.z * a_deltaTime;
            const auto heading = samples.front().heading + turn;
            const auto worldDirection = heading + a_output.directionAngles.z;
            RE::NiPoint3 velocity{std::sin(worldDirection) * a_output.speed, std::cos(worldDirection) * a_output.speed, 0.0F};
            const auto originalVelocity = velocity;
            ConstrainHorizontalVelocity(samples, velocity, a_deltaTime, turn);
            if ((velocity - originalVelocity).SqrLength() <= 0.000001F) {
                return;
            }

            const auto speed = velocity.Length();
            a_output.speed = speed >= kMinimumSpeed ? std::min(speed, a_output.speed) : 0.0F;
            if (a_output.speed > 0.0F) {
                a_output.directionAngles.z = std::remainder(std::atan2(velocity.x, velocity.y) - heading, 2.0F * std::numbers::pi_v<float>);
            }
        }

        void FilterNPCMovement(RE::Actor* a_actor, float a_deltaTime, RE::NiPoint3& a_translation, RE::NiPoint3& a_rotation) {
            originalNPCMovement(a_actor, a_deltaTime, a_translation, a_rotation);
            if (boundaryCount.load(std::memory_order_relaxed) == 0 || !std::isfinite(a_deltaTime) || a_deltaTime <= 0.0F ||
                !IsFinite(a_translation) || !IsFinite(a_rotation)) {
                return;
            }

            auto* controller = a_actor->GetActorRuntimeData().movementController.get();
            if (!controller || controller->unk1C7) {
                return;
            }
            const std::shared_lock lock{boundaryMutex};
            const auto entry = boundaries.find(controller);
            if (entry == boundaries.end() || entry->second.front().player) {
                return;
            }

            // ModifyMovementData receives local displacement after both controller and animation ddriven movement.
            // Skyrim applies the requested heading change before converting that displacement to world space...
            const auto& samples = entry->second;
            const auto heading = samples.front().heading + a_rotation.z;
            auto velocity = RotateHorizontal({a_translation.x, a_translation.y, 0.0F}, heading) / a_deltaTime;
            if (!IsFinite(velocity)) {
                return;
            }
            const auto originalVelocity = velocity;
            ConstrainHorizontalVelocity(samples, velocity, a_deltaTime, a_rotation.z);
            if ((velocity - originalVelocity).SqrLength() <= 0.000001F) {
                return;
            }

            const auto speed = velocity.Length();
            if (speed < kMinimumSpeed) {
                a_translation.x = 0.0F;
                a_translation.y = 0.0F;
                return;
            }
            velocity *= std::min(originalVelocity.Length() / speed, 1.0F);
            const auto displacement = RotateHorizontal(velocity * a_deltaTime, -heading);
            a_translation.x = displacement.x;
            a_translation.y = displacement.y;
        }
    }  // namespace

    HolderMovementSettings GetHolderMovementSettings() {
        return {preventHolderOverstretch, holderStretchAllowance};
    }

    void SetHolderMovementSettings(HolderMovementSettings a_settings) {
        const auto allowance = std::isfinite(a_settings.stretchAllowance) ? std::clamp(a_settings.stretchAllowance, -150.0F, 150.0F) : 10.0F;
        holderStretchAllowance = allowance;
        preventHolderOverstretch = a_settings.preventOverstretch;
    }

    void InstallLeashMovementConstraint() {
        if (installed) {
            return;
        }
        REL::Relocation<std::uintptr_t> vtable{RE::VTABLE_MovementControllerNPC[0]};
        originalMovement = vtable.write_vfunc(0x7, FilterMovement);
        REL::Relocation<std::uintptr_t> npcVtable{RE::VTABLE_Character[0]};
        originalNPCMovement = npcVtable.write_vfunc(REL::Module::IsVR() ? 0x11C : 0x11A, FilterNPCMovement);
        installed = true;
        SKSE::log::info("Installed player controller and NPC movement constraints");
    }

    bool CanConstrainNativeMovement(const RE::Actor& a_actor) {
        return CanFilterMovement(a_actor) && std::addressof(a_actor) != RE::PlayerCharacter::GetSingleton();
    }

    void UpdateLeashMovementConstraint(RE::MovementControllerNPC*& a_binding, RE::Actor& a_actor, const RE::NiPoint3& a_collar, const RE::NiPoint3& a_anchor,
        const RE::NiPoint3& a_goal, float a_ropeLength, float a_maxLength) {
        auto* controller = a_actor.GetActorRuntimeData().movementController.get();
        if (!CanConstrainNativeMovement(a_actor) || !controller || controller->unk1C7 || !IsFinite(a_collar) || !IsFinite(a_anchor) || !IsFinite(a_goal) ||
            !IsFinite(a_actor.GetPosition()) || !std::isfinite(a_actor.GetAngleZ()) || !std::isfinite(a_ropeLength) || a_ropeLength <= 0.0F || !std::isfinite(a_maxLength) || a_maxLength <= 0.0F) {
            ClearLeashMovementConstraint(a_binding);
            return;
        }

        // Movement callbacks may run off-thread. Publish geometry values; hooks never follow scene nodes or leash state.
        PublishBoundary(a_binding, controller, {.actorPosition = a_actor.GetPosition(), .attachmentOffset = a_collar - a_actor.GetPosition(),
            .anchor = a_anchor, .goal = a_goal, .heading = a_actor.GetAngleZ(), .ropeLength = a_ropeLength, .maxLength = a_maxLength});
    }

    void UpdateHolderMovementConstraint(RE::MovementControllerNPC*& a_binding, RE::Actor& a_holder, const RE::NiPoint3& a_attachment,
        const RE::NiPoint3& a_leanLimitAttachment, float a_ropeLength) {
        const auto settings = GetHolderMovementSettings();
        auto* controller = a_holder.GetActorRuntimeData().movementController.get();
        if (!settings.preventOverstretch || !CanFilterMovement(a_holder) || !controller || controller->unk1C7 ||
            !IsFinite(a_attachment) || !IsFinite(a_leanLimitAttachment) || !IsFinite(a_holder.GetPosition()) || !std::isfinite(a_holder.GetAngleZ()) ||
            !std::isfinite(a_ropeLength) || a_ropeLength <= 0.0F) {
            ClearLeashMovementConstraint(a_binding);
            return;
        }

        PublishBoundary(a_binding, controller, {.actorPosition = a_holder.GetPosition(), .attachmentOffset = a_attachment - a_holder.GetPosition(),
            .anchor = a_leanLimitAttachment, .heading = a_holder.GetAngleZ(), .ropeLength = std::max(a_ropeLength + settings.stretchAllowance, 0.001F), .holder = true,
            .player = std::addressof(a_holder) == RE::PlayerCharacter::GetSingleton()});
    }

    void ClearLeashMovementConstraint(RE::MovementControllerNPC*& a_binding) {
        if (a_binding) {
            const std::unique_lock lock{boundaryMutex};
            if (const auto entry = boundaries.find(a_binding); entry != boundaries.end()) {
                std::erase_if(entry->second, [&](const auto& a_sample) { return a_sample.owner == std::addressof(a_binding); });
                if (entry->second.empty()) {
                    boundaries.erase(entry);
                }
            }
            a_binding = nullptr;
            boundaryCount.store(boundaries.size(), std::memory_order_relaxed);
        }
    }
}  // namespace LeashFramework::Movement
