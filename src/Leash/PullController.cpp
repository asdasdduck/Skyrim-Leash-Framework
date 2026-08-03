#include "PullController.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <unordered_map>
#include <utility>

#include "../Movement/DirectLocomotion.h"
#include "../PCH.h"
#include "../Pathing/NavMeshPathfinder.h"

namespace LeashFramework {
    namespace {
        constexpr float kReplanInterval = 0.25F;
        constexpr float kGoalMoveThreshold = 64.0F;
        constexpr float kMinimumNormalizedSpeed = 0.65F;
        constexpr float kMaximumNormalizedSpeed = 2.0F;
        constexpr float kPlayerAssistSpeed = 1.0F;
        constexpr float kPlayerResistanceSpeed = 1.5F;
        constexpr float kMaximumForcedSpeedRatio = 0.5F;
        constexpr float kPlayerEffortResponseRate = 8.0F;
        constexpr float kDefaultPathingRadius = 24.0F;
        constexpr RE::FormID kPlayerFormID = 0x14;
        constexpr std::size_t kStableDirectFrames = 3;

        [[nodiscard]] bool CanPull(const RE::Actor& a_actor) {
            const auto* actorState = a_actor.AsActorState();
            return !a_actor.IsDead(false) && !a_actor.IsInRagdollState() && !actorState->IsUnconscious() && actorState->GetLifeState() != RE::ACTOR_LIFE_STATE::kRestrained &&
                   !a_actor.GetActorRuntimeData().boolFlags.any(RE::Actor::BOOL_FLAGS::kMovementBlocked);
        }

        [[nodiscard]] float HorizontalDistanceSquared(const RE::NiPoint3& a_left, const RE::NiPoint3& a_right) {
            const auto deltaX = a_left.x - a_right.x;
            const auto deltaY = a_left.y - a_right.y;
            return deltaX * deltaX + deltaY * deltaY;
        }

        enum class PullDiagnosticStage : std::uint8_t {
            kCanPullBlocked,
            kTurnHandoffBlocked,
            kDirectActiveBlocked,
            kControlsBlocked,
            kAttemptingStart,
            kStartFailed,
            kStarted,
            kStabilizing,
            kStateInvalidated,
            kDriveFailed,
            kTotal
        };

        struct PullDiagnosticSignature {
            PullDiagnosticStage stage{};
            Movement::DirectLocomotionState movement;
            bool dead{};
            bool ragdoll{};
            bool bleedingOut{};
            bool unconscious{};
            bool restrained{};
            bool movementBlocked{};
            bool animationDriven{};
            bool allowRotation{};

            bool operator==(const PullDiagnosticSignature&) const = default;
        };

        void LogPullDecision(bool a_diagnosticsEnabled, PullDiagnosticStage a_stage, std::string_view a_decision, RE::Actor& a_actor, float a_distance, float a_minLength, float a_maxLength) {
            if (!a_diagnosticsEnabled) {
                return;
            }
            const auto* actorState = a_actor.AsActorState();
            static std::unordered_map<RE::FormID, std::array<std::optional<PullDiagnosticSignature>, std::to_underlying(PullDiagnosticStage::kTotal)>> lastSignatures;
            const PullDiagnosticSignature signature{.stage = a_stage,
                .movement = Movement::CaptureDirectLocomotionState(a_actor),
                .dead = a_actor.IsDead(false),
                .ragdoll = a_actor.IsInRagdollState(),
                .bleedingOut = actorState->IsBleedingOut(),
                .unconscious = actorState->IsUnconscious(),
                .restrained = actorState->GetLifeState() == RE::ACTOR_LIFE_STATE::kRestrained,
                .movementBlocked = a_actor.GetActorRuntimeData().boolFlags.any(RE::Actor::BOOL_FLAGS::kMovementBlocked),
                .animationDriven = a_actor.IsAnimationDriven(),
                .allowRotation = a_actor.IsAllowRotation()};
            auto& lastSignature = lastSignatures[a_actor.GetFormID()][std::to_underlying(a_stage)];
            if (lastSignature && *lastSignature == signature) {
                return;
            }
            lastSignature = signature;
            SKSE::log::info(
                "[PullDiag] actor={:08X} decision={} distance={:.2f} min={:.2f} max={:.2f} canPull={} dead={} ragdoll={} bleedingOut={} unconscious={} restrained={} movementBlocked={} animationDriven={} "
                "allowRotation={}",
                a_actor.GetFormID(), a_decision, a_distance, a_minLength, a_maxLength, CanPull(a_actor), signature.dead, signature.ragdoll, signature.bleedingOut, signature.unconscious, signature.restrained,
                signature.movementBlocked, signature.animationDriven, signature.allowRotation);
            Movement::LogDirectLocomotionState(a_actor, a_decision);
        }
    }  // namespace

    void PullController::Update(State& a_state, RE::Actor& a_actor, const RE::NiPoint3& a_collarAnchor, const RE::NiPoint3& a_goal, RE::TESObjectCELL* a_goalCell, float a_minLength, float a_maxLength,
        float a_deltaTime) {
        LF_PROFILE_SCOPE("Controller/Pull");
        const auto distance = std::sqrt(HorizontalDistanceSquared(a_collarAnchor, a_goal));
        const auto formID = a_actor.GetFormID();
        if (!a_state.active) {
            if (distance <= a_maxLength) {
                return;
            }

            if (!CanPull(a_actor)) {
                LogPullDecision(_diagnosticsEnabled, PullDiagnosticStage::kCanPullBlocked, "blocked by CanPull", a_actor, distance, a_minLength, a_maxLength);
                return;
            }
            if (a_actor.IsAllowRotation()) {
                LogPullDecision(_diagnosticsEnabled, PullDiagnosticStage::kTurnHandoffBlocked, "waiting for animation-driven turn", a_actor, distance, a_minLength, a_maxLength);
                return;
            }
            if (Movement::IsDirectLocomotionActive(a_actor)) {
                LogPullDecision(_diagnosticsEnabled, PullDiagnosticStage::kDirectActiveBlocked, "blocked by existing direct locomotion", a_actor, distance, a_minLength, a_maxLength);
                return;
            }

            bool restorePlayerControls{};
            if (auto* player = RE::PlayerCharacter::GetSingleton(); player == std::addressof(a_actor)) {
                if (!a_actor.GetPlayerControls()) {
                    LogPullDecision(_diagnosticsEnabled, PullDiagnosticStage::kControlsBlocked, "blocked by player controls", a_actor, distance, a_minLength, a_maxLength);
                    return;
                }
                LogPullDecision(_diagnosticsEnabled, PullDiagnosticStage::kAttemptingStart, "attempting direct locomotion", a_actor, distance, a_minLength, a_maxLength);
                player->SetAIDriven(true);
                restorePlayerControls = true;
            } else {
                LogPullDecision(_diagnosticsEnabled, PullDiagnosticStage::kAttemptingStart, "attempting direct locomotion", a_actor, distance, a_minLength, a_maxLength);
            }
            if (!Movement::StartDirectLocomotion(a_actor, _diagnosticsEnabled)) {
                if (restorePlayerControls) {
                    RE::PlayerCharacter::GetSingleton()->SetAIDriven(false);
                }
                LogPullDecision(_diagnosticsEnabled, PullDiagnosticStage::kStartFailed, "direct locomotion start failed", a_actor, distance, a_minLength, a_maxLength);
                return;
            }

            a_state = State{.active = true, .restorePlayerControls = restorePlayerControls};
            LogPullDecision(_diagnosticsEnabled, PullDiagnosticStage::kStarted, "direct locomotion started", a_actor, distance, a_minLength, a_maxLength);
            if (auto* eventSource = SKSE::GetModCallbackEventSource()) {
                const SKSE::ModCallbackEvent event{.eventName = RE::BSFixedString{"LeashFramework_OnActorPulled"}, .strArg = {}, .numArg = distance, .sender = std::addressof(a_actor)};
                eventSource->SendEvent(std::addressof(event));
                SKSE::log::info("Sent LeashFramework_OnActorPulled for {:08X} at distance {:.1f}", formID, distance);
            }
        }

        if (distance <= a_minLength || !CanPull(a_actor) || !Movement::IsDirectLocomotionActive(a_actor)) {
            LogPullDecision(_diagnosticsEnabled, PullDiagnosticStage::kStateInvalidated, "pull state invalidated", a_actor, distance, a_minLength, a_maxLength);
            Release(a_state, std::addressof(a_actor));
            return;
        }

        if (a_state.stableDirectFrames < kStableDirectFrames) {
            if (!Movement::IsDirectLocomotionDriving(a_actor)) {
                a_state.stableDirectFrames = 0;
                LogPullDecision(_diagnosticsEnabled, PullDiagnosticStage::kStabilizing, "waiting for temporary movement handoff", a_actor, distance, a_minLength, a_maxLength);
                return;
            }
            (void)Movement::DriveDirectLocomotion(a_actor, a_actor.GetPosition(), 0.0F);
            if (a_actor.IsAnimationDriven() || a_actor.IsAllowRotation()) {
                a_state.stableDirectFrames = 0;
                LogPullDecision(_diagnosticsEnabled, PullDiagnosticStage::kStabilizing, "waiting for direct locomotion to stabilize", a_actor, distance, a_minLength, a_maxLength);
                return;
            }
            ++a_state.stableDirectFrames;
            if (a_state.stableDirectFrames < kStableDirectFrames) {
                LogPullDecision(_diagnosticsEnabled, PullDiagnosticStage::kStabilizing, "stabilizing direct locomotion", a_actor, distance, a_minLength, a_maxLength);
                return;
            }
        }

        a_state.replanDelay -= a_deltaTime;
        const auto goalMoved = HorizontalDistanceSquared(a_goal, a_state.lastGoal) >= kGoalMoveThreshold * kGoalMoveThreshold;
        const auto* characterController = a_actor.GetCharController();
        const auto controllerRadius = characterController ? characterController->radius * RE::bhkWorld::GetWorldScaleInverse() : kDefaultPathingRadius;
        const auto actorRadius = std::isfinite(controllerRadius) && controllerRadius > 0.0F ? controllerRadius : kDefaultPathingRadius;
        if (a_state.replanDelay <= 0.0F || goalMoved || a_state.waypointIndex >= a_state.path.size()) {
            static const Pathing::NavMeshPathfinder pathfinder;
            a_state.path = pathfinder.FindPath(a_actor.GetPosition(), a_actor.GetParentCell(), a_goal, a_goalCell, actorRadius);
            a_state.waypointIndex = 0;
            a_state.lastGoal = a_goal;
            a_state.replanDelay = kReplanInterval;
        }

        const auto waypointRadiusSquared = actorRadius * actorRadius;
        if (a_state.waypointIndex < a_state.path.size() && HorizontalDistanceSquared(a_actor.GetPosition(), a_state.path[a_state.waypointIndex]) <= waypointRadiusSquared) {
            ++a_state.waypointIndex;
        }

        if (a_state.waypointIndex >= a_state.path.size()) {
            (void)Movement::DriveDirectLocomotion(a_actor, a_actor.GetPosition(), 0.0F);
            return;
        }

        const auto tensionRange = std::max(a_maxLength - a_minLength, 1.0F);
        const auto tension = std::clamp((distance - a_minLength) / tensionRange, 0.0F, 1.0F);
        auto normalizedSpeed = std::lerp(kMinimumNormalizedSpeed, kMaximumNormalizedSpeed, tension);
        if (RE::PlayerCharacter::GetSingleton() == std::addressof(a_actor)) {
            float playerEffort{};
            if (const auto* controls = RE::PlayerControls::GetSingleton()) {
                const auto& moveInput = controls->data.moveInputVec;
                const auto inputStrength = std::min(std::sqrt(moveInput.SqrLength()), 1.0F);
                if (inputStrength > 0.001F) {
                    RE::NiPoint3 inputDirection{moveInput.x, moveInput.y, 0.0F};
                    if (const auto* camera = RE::PlayerCamera::GetSingleton(); camera && camera->cameraRoot) {
                        inputDirection = camera->cameraRoot->world.rotate * inputDirection;
                    } else {
                        const auto heading = a_actor.GetAngleZ();
                        inputDirection = {moveInput.x * std::cos(heading) + moveInput.y * std::sin(heading), -moveInput.x * std::sin(heading) + moveInput.y * std::cos(heading), 0.0F};
                    }
                    inputDirection.z = 0.0F;
                    auto pathDirection = a_state.path[a_state.waypointIndex] - a_actor.GetPosition();
                    pathDirection.z = 0.0F;
                    if (inputDirection.Unitize() > 0.001F && pathDirection.Unitize() > 0.001F) {
                        playerEffort = std::clamp(inputDirection.Dot(pathDirection), -1.0F, 1.0F) * inputStrength;
                    }
                }
            }
            const auto effortBlend = 1.0F - std::exp(-kPlayerEffortResponseRate * a_deltaTime);
            a_state.smoothedPlayerEffort = std::lerp(a_state.smoothedPlayerEffort, playerEffort, effortBlend);
            const auto assist = std::max(a_state.smoothedPlayerEffort, 0.0F) * kPlayerAssistSpeed;
            const auto resistance = std::max(-a_state.smoothedPlayerEffort, 0.0F) * kPlayerResistanceSpeed;
            const auto forcedSpeed = normalizedSpeed * tension * kMaximumForcedSpeedRatio;
            normalizedSpeed = std::max(forcedSpeed, normalizedSpeed + assist - resistance);
        }
        if (!Movement::DriveDirectLocomotion(a_actor, a_state.path[a_state.waypointIndex], normalizedSpeed)) {
            LogPullDecision(_diagnosticsEnabled, PullDiagnosticStage::kDriveFailed, "direct locomotion drive failed", a_actor, distance, a_minLength, a_maxLength);
            Release(a_state, std::addressof(a_actor));
        }
    }

    bool PullController::Release(State& a_state, RE::Actor* a_actor) {
        if (!a_state.active) {
            return false;
        }
        if (a_actor) {
            Movement::StopDirectLocomotion(*a_actor);
            if (a_state.restorePlayerControls) {
                RE::PlayerCharacter::GetSingleton()->SetAIDriven(false);
            }
        }
        a_state = {};
        return true;
    }
}  // namespace LeashFramework
