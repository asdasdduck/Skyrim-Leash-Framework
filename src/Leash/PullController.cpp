#include "PullController.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <unordered_map>
#include <utility>

#include "../Movement/DirectLocomotion.h"
#include "../Movement/LeashMovementConstraint.h"
#include "../PCH.h"
#include "../Pathing/NavMeshPathfinder.h"

namespace LeashFramework {
    namespace {
        constexpr float kReplanInterval = 0.25F;
        constexpr float kGoalMoveThreshold = 64.0F;
        constexpr float kNormalizedRunSpeed = 2.0F;
        constexpr float kExcessResponseRate = 2.0F;
        constexpr float kNormalizedAcceleration = 4.0F;
        constexpr float kNormalizedDeceleration = 8.0F;
        constexpr float kMovingSpeedThreshold = 12.0F;
        constexpr float kStationarySpeedThreshold = 5.0F;
        constexpr float kStationaryConfirmationTime = 0.3F;
        constexpr float kMovingGapResponseRate = 6.0F;
        constexpr float kArrivalTolerance = 6.0F;
        constexpr float kRestartTolerance = 8.0F;
        constexpr float kStartPredictionTime = 0.15F;
        constexpr float kIdleReleaseTime = 0.5F;
        constexpr float kRetryDelay = 0.5F;
        constexpr float kStoppedNormalizedSpeed = 0.02F;
        constexpr float kFinalWaypointRadius = 4.0F;
        constexpr float kMinimumAnchorReturnSpeed = 0.15F;
        constexpr float kVelocityResponseRate = 20.0F;
        constexpr float kMaximumSampleTime = 0.1F;
        constexpr float kMaximumSampleDisplacement = 128.0F;
        constexpr float kMaximumGoalSpeed = 1200.0F;
        constexpr float kFallbackRunSpeed = 370.0F;
        constexpr float kPlayerEffortResponseRate = 8.0F;
        constexpr float kDefaultPathingRadius = 24.0F;
        constexpr RE::FormID kPlayerFormID = 0x14;
        constexpr std::size_t kStableDirectFrames = 3;

        [[nodiscard]] bool CanPull(const RE::Actor& a_actor) {
            const auto* actorState = a_actor.AsActorState();
            return !a_actor.IsDead(false) && !a_actor.IsInRagdollState() && actorState->GetKnockState() == RE::KNOCK_STATE_ENUM::kNormal && !actorState->IsUnconscious() &&
                   actorState->GetLifeState() != RE::ACTOR_LIFE_STATE::kRestrained &&
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
            kSettled,
            kNativeMovement,
            kWaitingForPath,
            kWaitingForSeparation,
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

    void PullController::SetSettings(LocomotionSettings a_settings) noexcept {
        const LocomotionSettings defaults;
        const auto sanitize = [](float a_value, float a_default, float a_minimum, float a_maximum) {
            return std::isfinite(a_value) ? std::clamp(a_value, a_minimum, a_maximum) : a_default;
        };
        a_settings.forwardAssistance = sanitize(a_settings.forwardAssistance, defaults.forwardAssistance, 0.0F, 3.0F);
        a_settings.backwardResistance = sanitize(a_settings.backwardResistance, defaults.backwardResistance, 0.0F, 3.0F);
        a_settings.minimumForcedPullRatio = sanitize(a_settings.minimumForcedPullRatio, defaults.minimumForcedPullRatio, 0.0F, 1.0F);
        a_settings.maximumCatchUpSpeed = sanitize(a_settings.maximumCatchUpSpeed, defaults.maximumCatchUpSpeed, 0.25F, 10.0F);
        a_settings.movingFollowGap = sanitize(a_settings.movingFollowGap, defaults.movingFollowGap, 0.0F, 1.0F);
        a_settings.distanceResponseRate = sanitize(a_settings.distanceResponseRate, defaults.distanceResponseRate, 0.1F, 10.0F);
        _settings = a_settings;
    }

    void PullController::Update(State& a_state, RE::Actor& a_actor, const RE::NiPoint3& a_collarAnchor, const RE::NiPoint3& a_anchor, float a_ropeLength, const RE::NiPoint3& a_goal, RE::TESObjectCELL* a_goalCell, bool a_hasHolder, float a_minLength, float a_maxLength, float a_deltaTime) {
        LF_PROFILE_SCOPE("Controller/Pull");
        if (!std::isfinite(a_deltaTime) || a_deltaTime <= 0.0F) {
            Movement::ClearLeashMovementConstraint(a_state.nativeMovementBinding);
            return;
        }
        const auto deltaTime = std::min(a_deltaTime, kMaximumSampleTime);
        auto& motion = a_state.motion;
        auto goalDelta = a_goal - motion.previousGoal;
        goalDelta.z = 0.0F;
        if (a_hasHolder && motion.hasSample && a_deltaTime <= kMaximumSampleTime &&
            goalDelta.SqrLength() <= kMaximumSampleDisplacement * kMaximumSampleDisplacement) {
            auto velocity = goalDelta / a_deltaTime;
            const auto speed = velocity.Length();
            if (speed > kMaximumGoalSpeed) {
                velocity *= kMaximumGoalSpeed / speed;
            }
            const auto blend = 1.0F - std::exp(-kVelocityResponseRate * a_deltaTime);
            motion.velocity += (velocity - motion.velocity) * blend;
            const auto smoothedSpeed = motion.velocity.Length();
            motion.stationaryTime = smoothedSpeed <= kStationarySpeedThreshold ? std::min(motion.stationaryTime + deltaTime, kStationaryConfirmationTime) : 0.0F;
            if (smoothedSpeed >= kMovingSpeedThreshold) {
                motion.moving = true;
            } else if (motion.stationaryTime >= kStationaryConfirmationTime) {
                motion.moving = false;
            }
        } else {
            motion = {};
        }
        motion.previousGoal = a_goal;
        motion.hasSample = true;
        motion.movingBlend = std::lerp(motion.movingBlend, motion.moving ? 1.0F : 0.0F, 1.0F - std::exp(-kMovingGapResponseRate * deltaTime));
        a_state.retryDelay = std::max(a_state.retryDelay - deltaTime, 0.0F);
        const auto distance = std::sqrt(HorizontalDistanceSquared(a_collarAnchor, a_goal));
        auto goalDirection = a_goal - a_collarAnchor;
        goalDirection.z = 0.0F;
        (void)goalDirection.Unitize();
        const auto isPlayer = RE::PlayerCharacter::GetSingleton() == std::addressof(a_actor);
        const auto followsHolder = a_hasHolder && !isPlayer;
        const auto arrivalDistance = a_minLength + (followsHolder ? std::min({kArrivalTolerance, a_maxLength * 0.1F, a_maxLength - a_minLength}) : 0.0F);
        const auto targetDistance = a_minLength + (a_maxLength - a_minLength) * _settings.movingFollowGap * motion.movingBlend;
        const auto predictedDistance = distance + std::max(motion.velocity.Dot(goalDirection), 0.0F) * kStartPredictionTime;
        const auto formID = a_actor.GetFormID();
        const auto canUseNativeMovement = followsHolder && Movement::CanConstrainNativeMovement(a_actor);
        const auto allowNativeMovement = [&] {
            if (canUseNativeMovement && !motion.moving && CanPull(a_actor)) {
                Movement::UpdateLeashMovementConstraint(a_state.nativeMovementBinding, a_actor, a_collarAnchor, a_anchor, a_goal, a_ropeLength, a_maxLength);
                if (a_state.nativeMovementBinding) {
                    LogPullDecision(_diagnosticsEnabled, PullDiagnosticStage::kNativeMovement, "native movement within leash reach", a_actor, distance, a_minLength, a_maxLength);
                }
            } else {
                Movement::ClearLeashMovementConstraint(a_state.nativeMovementBinding);
            }
        };
        if (!a_state.active) {
            const auto physicalSeparation = distance > arrivalDistance + kRestartTolerance && a_collarAnchor.GetDistance(a_anchor) > a_ropeLength + kRestartTolerance;
            const auto stationaryNeedsPull = canUseNativeMovement ? distance > a_maxLength || physicalSeparation : distance > arrivalDistance + kRestartTolerance;
            const auto shouldFollow = followsHolder ? (motion.moving ? predictedDistance > targetDistance + kRestartTolerance : stationaryNeedsPull) : distance > a_maxLength;
            if (!shouldFollow || a_state.retryDelay > 0.0F) {
                allowNativeMovement();
                return;
            }
            Movement::ClearLeashMovementConstraint(a_state.nativeMovementBinding);

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
                a_state.retryDelay = kRetryDelay;
                return;
            }

            a_state.active = true;
            a_state.restorePlayerControls = restorePlayerControls;
            LogPullDecision(_diagnosticsEnabled, PullDiagnosticStage::kStarted, "direct locomotion started", a_actor, distance, a_minLength, a_maxLength);
            if (auto* eventSource = SKSE::GetModCallbackEventSource()) {
                const SKSE::ModCallbackEvent event{.eventName = RE::BSFixedString{"LeashFramework_OnActorPulled"}, .strArg = {}, .numArg = distance, .sender = std::addressof(a_actor)};
                eventSource->SendEvent(std::addressof(event));
                SKSE::log::info("Sent LeashFramework_OnActorPulled for {:08X} at distance {:.1f}", formID, distance);
            }
        }

        if (!CanPull(a_actor) || !Movement::IsDirectLocomotionActive(a_actor)) {
            LogPullDecision(_diagnosticsEnabled, PullDiagnosticStage::kStateInvalidated, "pull state invalidated", a_actor, distance, a_minLength, a_maxLength);
            Release(a_state, std::addressof(a_actor));
            return;
        }
        Movement::ClearLeashMovementConstraint(a_state.nativeMovementBinding);
        if ((isPlayer || !motion.moving) && distance <= arrivalDistance) {
            LogPullDecision(_diagnosticsEnabled, PullDiagnosticStage::kSettled, "settled near minimum length", a_actor, distance, a_minLength, a_maxLength);
            Release(a_state, std::addressof(a_actor), true);
            allowNativeMovement();
            return;
        }

        if (a_state.stableDirectFrames < kStableDirectFrames) {
            a_state.commandedSpeed = 0.0F;
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

        a_state.replanDelay -= deltaTime;
        const auto goalMoved = HorizontalDistanceSquared(a_goal, a_state.lastGoal) >= kGoalMoveThreshold * kGoalMoveThreshold;
        const auto* characterController = a_actor.GetCharController();
        const auto controllerRadius = characterController ? characterController->radius * RE::bhkWorld::GetWorldScaleInverse() : kDefaultPathingRadius;
        const auto actorRadius = std::isfinite(controllerRadius) && controllerRadius > 0.0F ? controllerRadius : kDefaultPathingRadius;
        if (a_state.replanDelay <= 0.0F || goalMoved) {
            static const Pathing::NavMeshPathfinder pathfinder;
            a_state.path = pathfinder.FindPath(a_actor.GetPosition(), a_actor.GetParentCell(), a_goal, a_goalCell, actorRadius);
            a_state.waypointIndex = 0;
            a_state.lastGoal = a_goal;
            a_state.replanDelay = kReplanInterval;
        }

        while (a_state.waypointIndex < a_state.path.size()) {
            const auto waypointRadius = a_state.waypointIndex + 1 < a_state.path.size() ? actorRadius : kFinalWaypointRadius;
            if (HorizontalDistanceSquared(a_actor.GetPosition(), a_state.path[a_state.waypointIndex]) > waypointRadius * waypointRadius) {
                break;
            }
            ++a_state.waypointIndex;
        }

        if (a_state.waypointIndex >= a_state.path.size()) {
            a_state.commandedSpeed = 0.0F;
            a_state.idleTime += deltaTime;
            if (!Movement::DriveDirectLocomotion(a_actor, a_actor.GetPosition(), 0.0F) || a_state.idleTime >= kIdleReleaseTime) {
                LogPullDecision(_diagnosticsEnabled, PullDiagnosticStage::kWaitingForPath, "waiting for a usable path", a_actor, distance, a_minLength, a_maxLength);
                Release(a_state, std::addressof(a_actor), true);
                a_state.retryDelay = kRetryDelay;
                allowNativeMovement();
            }
            return;
        }

        const auto tensionRange = std::max(a_maxLength - a_minLength, 1.0F);
        const auto tension = std::clamp((distance - a_minLength) / tensionRange, 0.0F, 1.0F);
        const auto actorPosition = a_actor.GetPosition();
        auto previousPoint = actorPosition;
        float pathDistance{};
        for (auto index = a_state.waypointIndex; index < a_state.path.size(); ++index) {
            // Track the live goal for speed control so its movement does not arrive in replan-sized jumps.
            const auto& point = index + 1 == a_state.path.size() ? a_goal : a_state.path[index];
            pathDistance += std::sqrt(HorizontalDistanceSquared(previousPoint, point));
            previousPoint = point;
        }
        auto endDirection = a_goal - (a_state.waypointIndex + 1 < a_state.path.size() ? a_state.path[a_state.path.size() - 2] : actorPosition);
        endDirection.z = 0.0F;
        (void)endDirection.Unitize();
        // The first path segment consumes distance; motion along the last segment adds or removes it, even around a corner.
        const auto followSpeed = motion.velocity.Dot(endDirection);
        const auto controlDistance = std::max(distance, pathDistance);
        const auto excess = std::max({distance - a_maxLength, a_collarAnchor.GetDistance(a_anchor) - a_ropeLength, 0.0F});
        const auto desiredSpeed = std::max(followSpeed + (controlDistance - targetDistance) * _settings.distanceResponseRate + excess * kExcessResponseRate, 0.0F);
        const auto runSpeed = a_actor.GetRunSpeed();
        const auto speedScale = kNormalizedRunSpeed / (std::isfinite(runSpeed) && runSpeed > 1.0F ? runSpeed : kFallbackRunSpeed);
        auto normalizedSpeed = std::clamp(desiredSpeed * speedScale, a_hasHolder ? 0.0F : kMinimumAnchorReturnSpeed, _settings.maximumCatchUpSpeed);
        float playerCatchUpSpeed{};
        if (isPlayer) {
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
            const auto effortBlend = 1.0F - std::exp(-kPlayerEffortResponseRate * deltaTime);
            a_state.smoothedPlayerEffort = std::lerp(a_state.smoothedPlayerEffort, playerEffort, effortBlend);
            const auto assist = std::max(a_state.smoothedPlayerEffort, 0.0F) * _settings.forwardAssistance;
            const auto resistance = std::max(-a_state.smoothedPlayerEffort, 0.0F) * _settings.backwardResistance;
            const auto forcedSpeed = normalizedSpeed * tension * _settings.minimumForcedPullRatio;
            normalizedSpeed = std::max(forcedSpeed, normalizedSpeed + assist - resistance);
            if (assist > 0.0F) {
                // Forward effort must close the moving gap even when the follow controller wants to slow down.
                playerCatchUpSpeed = std::max(followSpeed, 0.0F) * speedScale + assist;
                normalizedSpeed = std::max(normalizedSpeed, playerCatchUpSpeed);
            }
        }
        const auto brakingSpeed = std::max(playerCatchUpSpeed,
            std::max(followSpeed, 0.0F) * speedScale + std::sqrt(2.0F * kNormalizedDeceleration * std::max(controlDistance - arrivalDistance, 0.0F) * speedScale));
        normalizedSpeed = std::clamp(normalizedSpeed, 0.0F, std::min(brakingSpeed, _settings.maximumCatchUpSpeed + _settings.forwardAssistance));
        a_state.commandedSpeed += std::clamp(normalizedSpeed - a_state.commandedSpeed, -kNormalizedDeceleration * deltaTime, kNormalizedAcceleration * deltaTime);
        a_state.commandedSpeed = std::min(a_state.commandedSpeed, brakingSpeed);
        // Keep ownership while the moving gap contracts, rather than restarting halfway through settling.
        const auto settlingGap = !motion.moving && motion.movingBlend > 0.01F;
        a_state.idleTime = a_state.commandedSpeed <= kStoppedNormalizedSpeed && !settlingGap ? a_state.idleTime + deltaTime : 0.0F;
        if ((!followsHolder || !motion.moving) && a_state.idleTime >= kIdleReleaseTime) {
            LogPullDecision(_diagnosticsEnabled, PullDiagnosticStage::kWaitingForSeparation, "waiting for room to follow", a_actor, distance, a_minLength, a_maxLength);
            Release(a_state, std::addressof(a_actor), true);
            a_state.retryDelay = kRetryDelay;
            allowNativeMovement();
            return;
        }
        const auto& driveTarget = followsHolder && a_state.commandedSpeed <= kStoppedNormalizedSpeed ? a_actor.GetPosition() : a_state.path[a_state.waypointIndex];
        if (!Movement::DriveDirectLocomotion(a_actor, driveTarget, a_state.commandedSpeed)) {
            LogPullDecision(_diagnosticsEnabled, PullDiagnosticStage::kDriveFailed, "direct locomotion drive failed", a_actor, distance, a_minLength, a_maxLength);
            Release(a_state, std::addressof(a_actor));
        }
    }

    void PullController::ResetMotion(State& a_state) {
        a_state.motion = {};
        a_state.commandedSpeed = 0.0F;
        a_state.idleTime = 0.0F;
        Movement::ClearLeashMovementConstraint(a_state.nativeMovementBinding);
    }

    bool PullController::Release(State& a_state, RE::Actor* a_actor, bool a_keepMotion) {
        Movement::ClearLeashMovementConstraint(a_state.nativeMovementBinding);
        const auto wasActive = a_state.active;
        if (wasActive && a_actor) {
            Movement::StopDirectLocomotion(*a_actor);
            if (a_state.restorePlayerControls) {
                RE::PlayerCharacter::GetSingleton()->SetAIDriven(false);
            }
        }
        a_state = State{.motion = a_keepMotion ? a_state.motion : MotionState{}};
        return wasActive;
    }
}  // namespace LeashFramework
