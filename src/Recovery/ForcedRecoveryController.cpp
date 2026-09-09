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

        [[nodiscard]] bool CanRequestRagdoll(const RE::Actor& a_actor, bool a_allowGetUp = false) {
            const auto* actorState = a_actor.AsActorState();
            const auto knockState = actorState->GetKnockState();
            const auto* process = a_actor.GetActorRuntimeData().currentProcess;
            const auto* race = a_actor.GetRace();
            return !ActorRestrictions::IsRagdollOrTeleportBlocked(a_actor) && a_actor.Is3DLoaded() && !a_actor.IsDead(true) && !a_actor.IsInRagdollState() &&
                   (knockState == RE::KNOCK_STATE_ENUM::kNormal || (a_allowGetUp && knockState == RE::KNOCK_STATE_ENUM::kGetUp)) &&
                   !a_actor.IsInKillMove() && race && !race->data.flags.any(RE::RACE_DATA::Flag::kImmobile) &&
                   !race->data.flags.any(RE::RACE_DATA::Flag::kNoKnockdowns) && actorState->GetLifeState() == RE::ACTOR_LIFE_STATE::kAlive && process &&
                   process->middleHigh && (!process->high || static_cast<std::uint16_t>(process->high->animAction) != static_cast<std::uint16_t>(RE::CombatAnimation::ANIM::kActionActivate));
        }

        [[nodiscard]] RagdollSnapshot CaptureRagdoll(RE::Actor& a_actor) {
            RagdollSnapshot snapshot;
            if (a_actor.IsDead(true)) {
                auto* cell = a_actor.GetParentCell();
                auto* world = cell ? cell->GetbhkWorld() : nullptr;
                auto* root = a_actor.Get3D();
                if (!world || !root) {
                    return snapshot;
                }
                snapshot.world = RE::NiPointer<RE::bhkWorld>{world};
                // Match Skyrim's ApplyHavokImpulse traversal; corpses need no animation ragdoll driver.
                const auto captureNode = [&](auto&& a_self, RE::NiAVObject& a_object) -> void {
                    auto* collision = netimmerse_cast<RE::bhkCollisionObject*>(a_object.collisionObject.get());
                    auto* wrapper = collision ? collision->GetRigidBody() : nullptr;
                    auto* body = wrapper ? wrapper->GetRigidBody() : nullptr;
                    if (body && std::ranges::none_of(snapshot.bodies, [body](const auto& a_body) { return a_body.get() == body; })) {
                        snapshot.bodies.emplace_back(body);
                    }
                    if (auto* node = a_object.AsNode()) {
                        for (const auto& child : node->GetChildren()) {
                            if (child) {
                                a_self(a_self, *child);
                            }
                        }
                    }
                };
                captureNode(captureNode, *root);
                return snapshot;
            }
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

        [[nodiscard]] bool PullRagdoll(RE::Actor& a_actor, const RE::NiPoint3& a_collarAnchor, const RE::NiPoint3& a_holderAnchor, float a_maxLength, float a_deltaTime) {
            auto direction = a_holderAnchor - a_collarAnchor;
            const auto distance = direction.Unitize();

            auto snapshot = CaptureRagdoll(a_actor);
            if (!snapshot.world || snapshot.bodies.empty()) {
                return false;
            }

            const auto worldScale = RE::bhkWorld::GetWorldScale();
            const auto excessDistance = std::max(distance - a_maxLength, 0.0F);
            const auto desiredSpeed = std::clamp(kMinimumPullSpeed + excessDistance * kPullSpeedPerUnit, kMinimumPullSpeed, kMaximumPullSpeed) * worldScale;
            const auto maximumVelocityChange = kPullAcceleration * worldScale * std::min(a_deltaTime, kMaximumPhysicsStep);
            const RE::hkVector4 havokDirection{direction};
            RE::BSWriteLockGuard worldLock{snapshot.world->worldLock};
            auto* havokWorld = snapshot.world->GetWorld1();
            if (!havokWorld) {
                return false;
            }

            bool hasDynamicBody{};
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

                hasDynamicBody = true;
                if (distance <= a_maxLength || distance <= 0.001F) {
                    continue;
                }
                const auto speedTowardHolder = body->motion.linearVelocity.Dot3(havokDirection);
                const auto velocityChange = std::clamp(desiredSpeed - speedTowardHolder, 0.0F, maximumVelocityChange);
                if (velocityChange <= 0.0F) {
                    continue;
                }
                body->ApplyLinearImpulse(RE::hkVector4{direction.x * velocityChange * mass, direction.y * velocityChange * mass, direction.z * velocityChange * mass, 0.0F});
            }
            return hasDynamicBody;
        }

        [[nodiscard]] bool RequestRagdoll(RE::Actor& a_actor, const RE::NiPoint3& a_source) {
            auto* process = a_actor.GetActorRuntimeData().currentProcess;
            if (!process || !process->middleHigh) {
                return false;
            }
            process->KnockExplosion(std::addressof(a_actor), a_source, 0.0F);
            return true;
        }

        void SendPullEvent(RE::Actor& a_actor, float a_distance, bool& a_sent) {
            if (!a_sent) {
                a_sent = true;
                if (auto* eventSource = SKSE::GetModCallbackEventSource()) {
                    const SKSE::ModCallbackEvent event{.eventName = RE::BSFixedString{"LeashFramework_OnActorRagdollPulled"}, .strArg = {}, .numArg = a_distance, .sender = std::addressof(a_actor)};
                    eventSource->SendEvent(std::addressof(event));
                    SKSE::log::info("Sent LeashFramework_OnActorRagdollPulled for {:08X} at distance {:.1f}", a_actor.GetFormID(), a_distance);
                }
            }
        }
    }  // namespace

    void ForcedRecoveryController::SetSettings(ForcedRecoverySettings a_settings) noexcept {
        a_settings.distanceMultiplier = std::isfinite(a_settings.distanceMultiplier) ? std::clamp(a_settings.distanceMultiplier, 1.0F, 10.0F) : ForcedRecoverySettings{}.distanceMultiplier;
        _settings = a_settings;
    }

    bool ForcedRecoveryController::Update(State& a_state, RE::Actor& a_actor, const RE::NiPoint3& a_collarAnchor, const RE::NiPoint3& a_anchor, const RE::NiPoint3& a_source, float a_maxLength, float a_deltaTime,
        bool a_enabled) {
        LF_PROFILE_SCOPE("Controller/ForcedRecovery");
        const auto distance = a_collarAnchor.GetDistance(a_anchor);
        const auto formID = a_actor.GetFormID();
        const auto actorRestricted = ActorRestrictions::IsRagdollOrTeleportBlocked(a_actor);
        const auto triggerDistance = a_maxLength * _settings.distanceMultiplier;
        const auto* process = a_actor.GetActorRuntimeData().currentProcess;
        if (!std::isfinite(distance) || a_actor.IsDisabled() || !a_actor.Is3DLoaded()) {
            Release(a_state);
            return false;
        }

        if (a_actor.IsDead(true)) {
            if (a_actor.IsPlayerRef() || !a_enabled || actorRestricted || a_actor.IsInKillMove()) {
                Release(a_state);
                return false;
            }
            if (a_state.mode != Mode::kPullingCorpse) {
                const auto continuingPull = a_state.mode == Mode::kPulling;
                const auto eventSent = continuingPull && a_state.pullEventSent;
                Release(a_state);
                if (distance <= (continuingPull ? a_maxLength : triggerDistance)) {
                    return false;
                }
                a_state.mode = Mode::kPullingCorpse;
                a_state.pullEventSent = eventSent;
                return true;
            }
            if (!PullRagdoll(a_actor, a_collarAnchor, a_anchor, a_maxLength, a_deltaTime)) {
                Release(a_state);
                return false;
            }
            SendPullEvent(a_actor, distance, a_state.pullEventSent);
            if (distance <= a_maxLength) {
                a_state.insideDistanceTime += a_deltaTime;
                if (a_state.insideDistanceTime >= kInsideDistanceDelay) {
                    Release(a_state);
                    return false;
                }
            } else {
                a_state.insideDistanceTime = 0.0F;
            }
            return true;
        }
        if (a_state.mode == Mode::kPullingCorpse) {
            Release(a_state);
        }
        if (!process || !process->middleHigh) {
            Release(a_state);
            return false;
        }

        if (a_state.mode == Mode::kInactive) {
            if (!RagdollHold::IsAvailable() || !a_enabled || distance <= triggerDistance || !CanRequestRagdoll(a_actor)) {
                return false;
            }
            // Give the leash tick a frame to release direct locomotion before requesting a knockdown
            a_state.mode = Mode::kRequestingRagdoll;
            SKSE::log::info("Started forced recovery for {:08X} at distance {:.1f}", formID, distance);
            return true;
        }

        if (a_state.mode == Mode::kRecovering) {
            if (a_state.pullEventSent && !a_state.interruptingGetUp && a_enabled && distance > a_maxLength &&
                a_actor.AsActorState()->GetKnockState() == RE::KNOCK_STATE_ENUM::kGetUp && CanRequestRagdoll(a_actor, true)) {
                // This continues our existing pull episode; a failed interruption must not restart itself every recovery tick...
                a_state = State{.mode = Mode::kRequestingRagdoll, .pullEventSent = true, .interruptingGetUp = true};
                SKSE::log::info("Interrupting forced-recovery get-up for {:08X} at distance {:.1f}", formID, distance);
            } else {
                if (UpdateRecovery(a_state, a_actor, a_deltaTime)) {
                    BeginCooldown(a_state);
                    return false;
                }
                return true;
            }
        }

        if (a_state.mode == Mode::kCooldown) {
            a_state.modeElapsed += a_deltaTime;
            if (!a_enabled || actorRestricted || a_state.modeElapsed >= kRagdollRetryCooldown) {
                Release(a_state);
            }
            return false;
        }

        if (!a_enabled || actorRestricted || a_actor.IsInKillMove() || a_actor.AsActorState()->GetLifeState() != RE::ACTOR_LIFE_STATE::kAlive ||
            a_actor.GetActorRuntimeData().boolBits.any(RE::Actor::BOOL_BITS::kParalyzed)) {
            if (!a_state.requestIssued) {
                Release(a_state);
                return false;
            }
            BeginRecovery(a_state);
            return true;
        }

        const auto knockState = a_actor.AsActorState()->GetKnockState();
        if (a_state.mode == Mode::kRequestingRagdoll) {
            const auto waitingForGetUpInterrupt = a_state.interruptingGetUp && knockState == RE::KNOCK_STATE_ENUM::kGetUp;
            a_state.modeElapsed += a_deltaTime;
            a_state.actionRetryDelay = std::max(a_state.actionRetryDelay - a_deltaTime, 0.0F);
            if (a_state.requestIssued) {
                if (knockState == RE::KNOCK_STATE_ENUM::kExplodeLeadIn || knockState == RE::KNOCK_STATE_ENUM::kExplode) {
                    a_state.knockdownObserved = true;
                } else if (a_state.knockdownObserved || (knockState != RE::KNOCK_STATE_ENUM::kNormal && !waitingForGetUpInterrupt) || a_actor.IsInRagdollState()) {
                    BeginRecovery(a_state);
                    return true;
                }
            } else if (!CanRequestRagdoll(a_actor, a_state.interruptingGetUp)) {
                BeginCooldown(a_state);
                return false;
            }

            const auto requestDistance = a_state.interruptingGetUp ? a_maxLength : triggerDistance;
            if (!a_state.knockdownObserved && distance <= requestDistance) {
                if (a_state.requestIssued) {
                    BeginRecovery(a_state);
                    return true;
                }
                Release(a_state);
                return false;
            }
            if (a_state.modeElapsed >= kRagdollRequestTimeout) {
                SKSE::log::warn("Forced recovery could not obtain a dynamic ragdoll for {:08X} (get-up interruption={})", formID, a_state.interruptingGetUp);
                BeginRecovery(a_state);
                return true;
            }

            if (knockState != RE::KNOCK_STATE_ENUM::kExplode) {
                if ((knockState == RE::KNOCK_STATE_ENUM::kNormal || waitingForGetUpInterrupt) && a_state.actionRetryDelay <= 0.0F && CanRequestRagdoll(a_actor, a_state.interruptingGetUp)) {
                    if (!a_state.ragdollHold) {
                        a_state.ragdollHold = RagdollHold::Acquire(a_actor);
                    }
                    if (!a_state.ragdollHold) {
                        BeginCooldown(a_state);
                        return false;
                    }
                    // Publish the hold before the request: the engine may enter and update ragdoll before our next tick.
                    if (RequestRagdoll(a_actor, a_source)) {
                        a_state.requestIssued = true;
                    }
                    a_state.actionRetryDelay = kRagdollRetryInterval;
                }
                return true;
            }
        } else if (knockState != RE::KNOCK_STATE_ENUM::kExplode) {
            BeginRecovery(a_state);
            return true;
        }

        if (!PullRagdoll(a_actor, a_collarAnchor, a_anchor, a_maxLength, a_deltaTime)) {
            if (a_state.mode == Mode::kPulling) {
                SKSE::log::warn("Forced recovery lost its dynamic ragdoll for {:08X}", formID);
                BeginRecovery(a_state);
            }
            return true;
        }

        if (a_state.mode == Mode::kRequestingRagdoll) {
            a_state.mode = Mode::kPulling;
            a_state.modeElapsed = 0.0F;
            a_state.interruptingGetUp = false;
            SendPullEvent(a_actor, distance, a_state.pullEventSent);
        }

        if (distance <= a_maxLength) {
            a_state.insideDistanceTime += a_deltaTime;
            if (a_state.insideDistanceTime >= kInsideDistanceDelay) {
                BeginRecovery(a_state);
            }
        } else {
            a_state.insideDistanceTime = 0.0F;
        }
        return true;
    }

    bool ForcedRecoveryController::Release(State& a_state) {
        const auto wasActive = a_state.mode != Mode::kInactive;
        a_state = {};
        return wasActive;
    }

    void ForcedRecoveryController::BeginRecovery(State& a_state) {
        a_state.ragdollHold.reset();
        if (a_state.mode != Mode::kRecovering) {
            a_state.mode = Mode::kRecovering;
            a_state.modeElapsed = 0.0F;
        }
    }

    void ForcedRecoveryController::BeginCooldown(State& a_state) {
        a_state = {};
        a_state.mode = Mode::kCooldown;
    }

    bool ForcedRecoveryController::UpdateRecovery(State& a_state, RE::Actor& a_actor, float a_deltaTime) {
        a_state.modeElapsed += a_deltaTime;
        const auto isRagdolled = a_actor.IsInRagdollState();
        const auto knockState = a_actor.AsActorState()->GetKnockState();
        if (isRagdolled || knockState != RE::KNOCK_STATE_ENUM::kNormal) {
            a_state.knockdownObserved = true;
        } else {
            if (a_state.requestIssued && !a_state.knockdownObserved && a_state.modeElapsed < kPendingRagdollGrace) {
                return false;
            }
            SKSE::log::info("Completed forced recovery for {:08X}", a_actor.GetFormID());
            return true;
        }

        if (a_state.modeElapsed >= kRecoveryTimeout) {
            SKSE::log::warn("Forced recovery timed out for {:08X}; leaving recovery to Skyrim", a_actor.GetFormID());
            return true;
        }
        return false;
    }
}  // namespace LeashFramework::Recovery
