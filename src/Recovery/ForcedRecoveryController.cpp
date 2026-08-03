#include "ForcedRecoveryController.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "../Actor/ActorRestrictions.h"
#include "../PCH.h"

namespace LeashFramework::Recovery {
    namespace {
        constexpr float kInsideDistanceDelay = 0.25F;
        constexpr float kRagdollRetryInterval = 0.5F;
        constexpr float kGetUpRetryInterval = 1.0F;
        constexpr float kRagdollRequestTimeout = 3.0F;
        constexpr float kPendingRagdollGrace = 1.0F;
        constexpr float kRagdollRetryCooldown = 5.0F;
        constexpr float kRecoveryTimeout = 15.0F;
        constexpr float kMinimumPullSpeed = 150.0F;
        constexpr float kMaximumPullSpeed = 1000.0F;
        constexpr float kPullSpeedPerUnit = 1.5F;
        constexpr float kPullAcceleration = 1800.0F;
        constexpr float kMaximumPhysicsStep = 0.05F;

        struct RagdollSnapshot {
            RE::NiPointer<RE::bhkWorld> world;
            std::vector<RE::hkRefPtr<RE::hkpRigidBody>> bodies;
        };

        [[nodiscard]] bool CanBeginRecovery(const RE::Actor& a_actor) {
            const auto* actorState = a_actor.AsActorState();
            const auto* process = a_actor.GetActorRuntimeData().currentProcess;
            const auto* race = a_actor.GetRace();
            return !ActorRestrictions::IsRagdollOrTeleportBlocked(a_actor) && a_actor.Is3DLoaded() && !a_actor.IsDead(true) && !a_actor.IsInRagdollState() && race &&
                   !race->data.flags.any(RE::RACE_DATA::Flag::kImmobile) && !race->data.flags.any(RE::RACE_DATA::Flag::kNoKnockdowns) && actorState->GetLifeState() != RE::ACTOR_LIFE_STATE::kRestrained && process &&
                   process->middleHigh && (!process->high || static_cast<std::uint16_t>(process->high->animAction) != static_cast<std::uint16_t>(RE::CombatAnimation::ANIM::kActionActivate));
        }

        [[nodiscard]] bool IsEnabledFor(const ForcedRecoverySettings& a_settings, const RE::Actor& a_actor) { return a_actor.IsPlayerRef() ? a_settings.enablePlayer : a_settings.enableNPCs; }

        [[nodiscard]] RagdollSnapshot CaptureRagdoll(RE::Actor& a_actor) {
            RagdollSnapshot snapshot;
            RE::BSAnimationGraphManagerPtr manager;
            if (!a_actor.GetAnimationGraphManager(manager) || !manager) {
                return snapshot;
            }

            RE::BSSpinLockGuard graphLock{manager->GetRuntimeData().updateLock};
            const auto captureGraph = [&](std::size_t a_index) {
                if (a_index >= manager->graphs.size()) {
                    return false;
                }
                const auto& graph = manager->graphs[a_index];
                if (!graph || !graph->physicsWorld) {
                    return false;
                }
                const auto driver = graph->characterInstance.ragdollDriver;
                const auto* ragdoll = driver ? driver->ragdoll : nullptr;
                if (!ragdoll || ragdoll->rigidBodies.empty()) {
                    return false;
                }

                snapshot.world = RE::NiPointer<RE::bhkWorld>{graph->physicsWorld};
                snapshot.bodies.reserve(ragdoll->rigidBodies.size());
                for (auto* body : ragdoll->rigidBodies) {
                    if (body) {
                        snapshot.bodies.emplace_back(body);
                    }
                }
                return !snapshot.bodies.empty();
            };

            const auto activeGraph = static_cast<std::size_t>(manager->GetRuntimeData().activeGraph);
            if (captureGraph(activeGraph)) {
                return snapshot;
            }
            for (std::size_t index = 0; index < manager->graphs.size(); ++index) {
                if (index != activeGraph && captureGraph(index)) {
                    break;
                }
            }
            return snapshot;
        }

        void PullRagdoll(RE::Actor& a_actor, const RE::NiPoint3& a_collarAnchor, const RE::NiPoint3& a_holderAnchor, float a_maxLength, float a_deltaTime) {
            auto direction = a_holderAnchor - a_collarAnchor;
            const auto distance = direction.Unitize();
            if (distance <= 0.001F) {
                return;
            }

            auto snapshot = CaptureRagdoll(a_actor);
            if (!snapshot.world || snapshot.bodies.empty()) {
                return;
            }

            const auto worldScale = RE::bhkWorld::GetWorldScale();
            const auto excessDistance = std::max(distance - a_maxLength, 0.0F);
            const auto desiredSpeed = std::clamp(kMinimumPullSpeed + excessDistance * kPullSpeedPerUnit, kMinimumPullSpeed, kMaximumPullSpeed) * worldScale;
            const auto maximumVelocityChange = kPullAcceleration * worldScale * std::min(a_deltaTime, kMaximumPhysicsStep);
            const RE::hkVector4 havokDirection{direction};
            RE::BSWriteLockGuard worldLock{snapshot.world->worldLock};
            auto* havokWorld = snapshot.world->GetWorld1();
            if (!havokWorld) {
                return;
            }

            for (const auto& bodyPointer : snapshot.bodies) {
                auto* body = bodyPointer.get();
                if (!body || body->world != havokWorld) {
                    continue;
                }
                const auto motionType = body->motion.type.get();
                if (motionType == RE::hkpMotion::MotionType::kInvalid || motionType == RE::hkpMotion::MotionType::kKeyframed || motionType == RE::hkpMotion::MotionType::kFixed) {
                    continue;
                }
                const auto mass = body->motion.GetMass();
                if (!std::isfinite(mass) || mass <= 0.0F) {
                    continue;
                }

                const auto speedTowardHolder = body->motion.linearVelocity.Dot3(havokDirection);
                const auto velocityChange = std::clamp(desiredSpeed - speedTowardHolder, 0.0F, maximumVelocityChange);
                if (velocityChange <= 0.0F) {
                    continue;
                }
                body->ApplyLinearImpulse(RE::hkVector4{direction.x * velocityChange * mass, direction.y * velocityChange * mass, direction.z * velocityChange * mass, 0.0F});
            }
        }

        [[nodiscard]] bool RequestRagdoll(RE::Actor& a_actor, const RE::NiPoint3& a_source) {
            auto* process = a_actor.GetActorRuntimeData().currentProcess;
            if (!process || !process->middleHigh) {
                return false;
            }
            process->KnockExplosion(std::addressof(a_actor), a_source, 0.0F);
            return true;
        }

        [[nodiscard]] bool RequestGetUp(RE::Actor& a_actor) { return RE::SourceActionMap::DoAction(std::addressof(a_actor), RE::DEFAULT_OBJECT::kActionGetUp); }

        void QueueGetUpEnd(RE::Actor& a_actor) {
            using GetUpEndHandler = bool (*)(void*, RE::Actor*);
            static REL::Relocation<GetUpEndHandler> getUpEndHandler{REL::VariantID(41799, 42880, 0x722DC0)};
            static_cast<void>(getUpEndHandler(nullptr, std::addressof(a_actor)));
        }
    }  // namespace

    void ForcedRecoveryController::SetSettings(ForcedRecoverySettings a_settings) noexcept {
        a_settings.distanceMultiplier = std::isfinite(a_settings.distanceMultiplier) ? std::clamp(a_settings.distanceMultiplier, 1.0F, 10.0F) : ForcedRecoverySettings{}.distanceMultiplier;
        _settings = a_settings;
    }

    bool ForcedRecoveryController::Update(State& a_state, RE::Actor& a_actor, const RE::NiPoint3& a_collarAnchor, const RE::NiPoint3& a_anchor, const RE::NiPoint3& a_source, float a_maxLength, float a_deltaTime) {
        LF_PROFILE_SCOPE("Controller/ForcedRecovery");
        const auto distance = a_collarAnchor.GetDistance(a_anchor);
        const auto formID = a_actor.GetFormID();
        const auto enabled = IsEnabledFor(_settings, a_actor);
        const auto actorRestricted = ActorRestrictions::IsRagdollOrTeleportBlocked(a_actor);
        const auto triggerDistance = a_maxLength * _settings.distanceMultiplier;
        if (!a_state.active) {
            if (!enabled || distance <= triggerDistance || !CanBeginRecovery(a_actor)) {
                return false;
            }

            a_state = {};
            a_state.active = true;
            SKSE::log::info("Started forced recovery for {:08X} at distance {:.1f}", formID, distance);
            return true;
        }

        if (a_state.mode == Mode::kRecovering) {
            if (UpdateRecovery(a_state, a_actor, a_deltaTime)) {
                a_state = {};
                return false;
            }
            return true;
        }

        if (a_state.mode == Mode::kCooldown) {
            if (!enabled || actorRestricted || distance <= triggerDistance) {
                a_state = {};
                return false;
            }
            a_state.actionRetryDelay = std::max(a_state.actionRetryDelay - a_deltaTime, 0.0F);
            if (a_state.actionRetryDelay <= 0.0F && CanBeginRecovery(a_actor)) {
                a_state.mode = Mode::kRequestingRagdoll;
                a_state.modeElapsed = 0.0F;
                return true;
            }
            return false;
        }

        if (!enabled || actorRestricted || a_actor.IsDead(true)) {
            if (a_actor.IsDead(true)) {
                a_state = {};
                return false;
            }
            BeginRecovery(a_state);
            if (UpdateRecovery(a_state, a_actor, a_deltaTime)) {
                a_state = {};
                return false;
            }
            return true;
        }

        if (a_state.mode == Mode::kRequestingRagdoll) {
            if (!a_state.ownsRagdoll && distance <= triggerDistance) {
                if (a_state.requestIssued) {
                    BeginRecovery(a_state);
                    if (!UpdateRecovery(a_state, a_actor, a_deltaTime)) {
                        return true;
                    }
                }
                a_state = {};
                return false;
            }

            const auto requestingKnockState = a_actor.AsActorState()->GetKnockState();
            const auto requestingRagdoll = a_actor.IsInRagdollState();
            if (a_state.requestIssued && (requestingRagdoll || requestingKnockState != RE::KNOCK_STATE_ENUM::kNormal)) {
                a_state.ownsRagdoll = true;
                a_state.knockdownObserved = true;
            }
            if (requestingRagdoll && requestingKnockState != RE::KNOCK_STATE_ENUM::kGetUp) {
                if (!a_state.ownsRagdoll) {
                    BeginCooldown(a_state);
                    return false;
                }
                a_state.mode = Mode::kPulling;
                a_state.modeElapsed = 0.0F;
                if (auto* eventSource = SKSE::GetModCallbackEventSource()) {
                    const SKSE::ModCallbackEvent event{.eventName = RE::BSFixedString{"LeashFramework_OnActorRagdollPulled"}, .strArg = {}, .numArg = distance, .sender = std::addressof(a_actor)};
                    eventSource->SendEvent(std::addressof(event));
                    SKSE::log::info("Sent LeashFramework_OnActorRagdollPulled for {:08X} at distance {:.1f}", formID, distance);
                }
            } else {
                a_state.modeElapsed += a_deltaTime;
                if (a_state.modeElapsed >= kRagdollRequestTimeout) {
                    if (a_state.ownsRagdoll) {
                        BeginRecovery(a_state);
                        if (UpdateRecovery(a_state, a_actor, a_deltaTime)) {
                            a_state = {};
                            return false;
                        }
                        return true;
                    }
                    SKSE::log::warn("Forced recovery could not ragdoll {:08X}; restoring locomotion", formID);
                    BeginCooldown(a_state);
                    return false;
                }

                a_state.actionRetryDelay = std::max(a_state.actionRetryDelay - a_deltaTime, 0.0F);
                if (a_state.actionRetryDelay <= 0.0F && (!a_state.requestIssued || requestingKnockState == RE::KNOCK_STATE_ENUM::kNormal)) {
                    if (RequestRagdoll(a_actor, a_source)) {
                        a_state.requestIssued = true;
                    }
                    a_state.actionRetryDelay = kRagdollRetryInterval;
                }
                return true;
            }
        }

        if (distance <= a_maxLength) {
            a_state.insideDistanceTime += a_deltaTime;
            if (a_state.insideDistanceTime >= kInsideDistanceDelay) {
                BeginRecovery(a_state);
                if (UpdateRecovery(a_state, a_actor, a_deltaTime)) {
                    a_state = {};
                    return false;
                }
            }
            return true;
        }
        a_state.insideDistanceTime = 0.0F;

        const auto knockState = a_actor.AsActorState()->GetKnockState();
        const auto isRagdolled = a_actor.IsInRagdollState();
        if (isRagdolled) {
            a_state.knockdownObserved = true;
        }
        a_state.actionRetryDelay = std::max(a_state.actionRetryDelay - a_deltaTime, 0.0F);
        if (!isRagdolled || knockState == RE::KNOCK_STATE_ENUM::kGetUp) {
            a_state.mode = Mode::kRequestingRagdoll;
            a_state.modeElapsed = 0.0F;
            a_state.actionRetryDelay = 0.0F;
            a_state.requestIssued = false;
            return true;
        }

        PullRagdoll(a_actor, a_collarAnchor, a_anchor, a_maxLength, a_deltaTime);
        return true;
    }

    bool ForcedRecoveryController::Release(State& a_state) {
        if (!a_state.active) {
            return false;
        }
        // Just stop pulling here. Skyrim owns the ragdoll and will get the actor up.
        a_state = {};
        return true;
    }

    void ForcedRecoveryController::BeginRecovery(State& a_state) {
        if (a_state.mode == Mode::kRecovering) {
            return;
        }
        a_state.mode = Mode::kRecovering;
        a_state.actionRetryDelay = 0.0F;
        a_state.modeElapsed = 0.0F;
        a_state.recoveryElapsed = 0.0F;
        a_state.getUpEndQueued = false;
    }

    void ForcedRecoveryController::BeginCooldown(State& a_state) {
        a_state.mode = Mode::kCooldown;
        a_state.actionRetryDelay = kRagdollRetryCooldown;
        a_state.modeElapsed = 0.0F;
        a_state.recoveryElapsed = 0.0F;
        a_state.ownsRagdoll = false;
        a_state.requestIssued = false;
        a_state.knockdownObserved = false;
        a_state.getUpEndQueued = false;
    }

    bool ForcedRecoveryController::UpdateRecovery(State& a_state, RE::Actor& a_actor, float a_deltaTime) {
        const auto* process = a_actor.GetActorRuntimeData().currentProcess;
        if (!a_actor.Is3DLoaded() || !process || !process->middleHigh) {
            return false;
        }

        const auto isRagdolled = a_actor.IsInRagdollState();
        const auto knockState = a_actor.AsActorState()->GetKnockState();
        if (!a_state.ownsRagdoll) {
            if (!a_state.requestIssued) {
                return true;
            }
            if (isRagdolled || knockState != RE::KNOCK_STATE_ENUM::kNormal) {
                a_state.ownsRagdoll = true;
                a_state.knockdownObserved = true;
            } else {
                a_state.modeElapsed += a_deltaTime;
                return a_state.modeElapsed >= kPendingRagdollGrace;
            }
        }

        a_state.recoveryElapsed += a_deltaTime;
        if (a_state.recoveryElapsed >= kRecoveryTimeout) {
            SKSE::log::error("Forced recovery timed out for {:08X}", a_actor.GetFormID());
            if (a_actor.IsInRagdollState() || a_actor.AsActorState()->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal) {
                a_actor.PotentiallyFixRagdollState();
            }
            return true;
        }

        if (isRagdolled || knockState != RE::KNOCK_STATE_ENUM::kNormal) {
            a_state.knockdownObserved = true;
        }
        if (knockState == RE::KNOCK_STATE_ENUM::kNormal && !isRagdolled) {
            if (!a_state.knockdownObserved) {
                return false;
            }
            SKSE::log::info("Completed forced recovery for {:08X}", a_actor.GetFormID());
            return true;
        }
        a_state.actionRetryDelay = std::max(a_state.actionRetryDelay - a_deltaTime, 0.0F);
        if (knockState == RE::KNOCK_STATE_ENUM::kGetUp) {
            if (!a_state.getUpEndQueued && a_state.actionRetryDelay <= 0.0F) {
                QueueGetUpEnd(a_actor);
                a_state.getUpEndQueued = true;
                a_state.actionRetryDelay = kGetUpRetryInterval;
            }
        } else if (isRagdolled && a_state.actionRetryDelay <= 0.0F) {
            static_cast<void>(RequestGetUp(a_actor));
            a_state.actionRetryDelay = kGetUpRetryInterval;
        }
        return false;
    }
}  // namespace LeashFramework::Recovery
