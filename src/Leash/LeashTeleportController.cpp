#include "LeashTeleportController.h"

#include <algorithm>
#include <cmath>

#include "../Actor/ActorRestrictions.h"
#include "../PCH.h"
#include "LeashInstance.h"

// Todo: Right now non-actor leash anchors are not handled (holderFormID == 0). Possibly do this in the future? Although
// I'm concerned about issues, and or false intentions.
namespace LeashFramework {
    namespace {
        constexpr float kFollowHandlingDisabledMinLength = 99999.0F;
        constexpr float kTeleportCooldown = 2.0F;
        constexpr float kTeleportSpacing = 128.0F;

        [[nodiscard]] bool AreInCompatibleSpaces(const RE::Actor& a_holder, const RE::Actor& a_leashed) {
            const auto* holderCell = a_holder.GetParentCell();
            const auto* leashedCell = a_leashed.GetParentCell();
            if (!holderCell || !leashedCell) {
                return false;
            }
            if (holderCell == leashedCell) {
                return true;
            }
            if (!holderCell->IsExteriorCell() || !leashedCell->IsExteriorCell()) {
                return false;
            }
            const auto* holderWorldSpace = holderCell->GetRuntimeData().worldSpace;
            return holderWorldSpace && holderWorldSpace == leashedCell->GetRuntimeData().worldSpace;
        }

        [[nodiscard]] bool CanBeginRecovery(RE::Actor& a_actor) {
            const auto* actorState = a_actor.AsActorState();
            const auto* process = a_actor.GetActorRuntimeData().currentProcess;
            const auto* race = a_actor.GetRace();
            return !ActorRestrictions::IsRagdollOrTeleportBlocked(a_actor) && !a_actor.IsInRagdollState() && race && !race->data.flags.any(RE::RACE_DATA::Flag::kImmobile) &&
                   !race->data.flags.any(RE::RACE_DATA::Flag::kNoKnockdowns) && actorState->GetLifeState() != RE::ACTOR_LIFE_STATE::kRestrained && process &&
                   (!process->high || static_cast<std::uint16_t>(process->high->animAction) != static_cast<std::uint16_t>(RE::CombatAnimation::ANIM::kActionActivate));
        }

    }  // namespace

    void LeashTeleportController::SetSettings(LeashTeleportSettings a_settings) noexcept {
        const LeashTeleportSettings defaults;
        a_settings.gracePeriod = std::isfinite(a_settings.gracePeriod) ? std::clamp(a_settings.gracePeriod, 0.0F, 30.0F) : defaults.gracePeriod;
        a_settings.playerDistance = std::isfinite(a_settings.playerDistance) ? a_settings.playerDistance : defaults.playerDistance;
        a_settings.npcDistance = std::isfinite(a_settings.npcDistance) ? a_settings.npcDistance : defaults.npcDistance;
        _settings = a_settings;
    }

    bool LeashTeleportController::HandlePlayerPositioned(LeashInstance& a_leash) {
        const auto& definition = a_leash.GetDefinition();
        auto& state = a_leash._teleportState;
        if (definition.holderFormID == 0) {
            state = {};
            return false;
        }
        if (definition.minLength > kFollowHandlingDisabledMinLength) {
            state = {};
            return false;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || definition.holderFormID != player->GetFormID()) {
            return false;
        }
        auto* leashed = RE::TESForm::LookupByID<RE::Actor>(definition.leashedFormID);
        if (!leashed || leashed->IsDisabled()) {
            return false;
        }
        if (!CanBeginRecovery(*leashed)) {
            state = State{.pendingPlayerPosition = true};
            return false;
        }
        const auto teleported = TeleportToPlayerHolder(a_leash);
        if (teleported) {
            state.pendingPlayerPosition = false;
        }
        return teleported;
    }

    LeashTeleportController::UpdateResult LeashTeleportController::Update(LeashInstance& a_leash, float a_deltaTime) {
        LF_PROFILE_SCOPE("Controller/Teleport");
        const auto& definition = a_leash.GetDefinition();
        auto& state = a_leash._teleportState;
        if (definition.holderFormID == 0) {
            state = {};
            return UpdateResult::kNone;
        }
        if (definition.minLength > kFollowHandlingDisabledMinLength) {
            state = {};
            return UpdateResult::kNone;
        }

        auto* holder = RE::TESForm::LookupByID<RE::Actor>(definition.holderFormID);
        auto* leashed = RE::TESForm::LookupByID<RE::Actor>(definition.leashedFormID);
        if (!holder || !leashed) {
            return UpdateResult::kNone;
        }
        if (holder->IsDisabled() || leashed->IsDisabled()) {
            return UpdateResult::kNone;
        }
        if (holder->IsDead(true) || leashed->IsDead(true)) {
            return UpdateResult::kNone;
        }
        if (holder->IsPlayerRef()) {
            if (!state.pendingPlayerPosition) {
                return UpdateResult::kNone;
            }
            if (!TeleportToPlayerHolder(a_leash)) {
                return UpdateResult::kPending;
            }
            state.pendingPlayerPosition = false;
            return UpdateResult::kTeleported;
        }
        const auto teleportDistance = leashed->IsPlayerRef() ? _settings.playerDistance : _settings.npcDistance;
        if (teleportDistance <= 0.0F) {
            state = {};
            return UpdateResult::kNone;
        }
        state.cooldownRemaining = std::max(state.cooldownRemaining - a_deltaTime, 0.0F);

        SeparationReason reason{SeparationReason::kNone};
        if (!AreInCompatibleSpaces(*holder, *leashed)) {
            reason = SeparationReason::kIncompatibleSpace;
        } else if (holder->GetPosition().GetDistance(leashed->GetPosition()) > definition.maxLength + teleportDistance) {
            reason = SeparationReason::kExcessiveDistance;
        }

        if (reason == SeparationReason::kNone) {
            state.reason = SeparationReason::kNone;
            state.graceElapsed = 0.0F;
            return UpdateResult::kNone;
        }
        if (!CanBeginRecovery(*leashed)) {
            state.reason = reason;
            state.graceElapsed = 0.0F;
            return UpdateResult::kPending;
        }
        if (state.cooldownRemaining > 0.0F) {
            return UpdateResult::kPending;
        }
        if (state.reason != reason) {
            state.reason = reason;
            state.graceElapsed = 0.0F;
        }
        state.graceElapsed += a_deltaTime;
        if (state.graceElapsed < _settings.gracePeriod) {
            return UpdateResult::kPending;
        }
        if (!holder->GetParentCell()) {
            return UpdateResult::kPending;
        }

        if (!Teleport(a_leash, reason)) {
            state.cooldownRemaining = 0.5F;
            return UpdateResult::kPending;
        }
        state.reason = SeparationReason::kNone;
        state.graceElapsed = 0.0F;
        state.cooldownRemaining = kTeleportCooldown;
        return UpdateResult::kTeleported;
    }

    bool LeashTeleportController::TeleportToPlayerHolder(LeashInstance& a_leash) {
        const auto& definition = a_leash.GetDefinition();
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* leashed = RE::TESForm::LookupByID<RE::Actor>(definition.leashedFormID);
        if (!player || !leashed || definition.holderFormID != player->GetFormID() || leashed->IsDisabled() || !CanBeginRecovery(*leashed)) {
            return false;
        }
        leashed->MoveTo(player);
        const auto movedToNavmesh = leashed->MoveToNearestNavmesh();
        a_leash.ResetSimulation();
        SKSE::log::info("Teleported leashed actor {:08X} to player after positioning; navmesh={}", definition.leashedFormID, movedToNavmesh);
        return true;
    }

    bool LeashTeleportController::Teleport(LeashInstance& a_leash, SeparationReason a_reason) {
        const auto& definition = a_leash.GetDefinition();
        auto* holder = RE::TESForm::LookupByID<RE::Actor>(definition.holderFormID);
        auto* leashed = RE::TESForm::LookupByID<RE::Actor>(definition.leashedFormID);
        if (!holder || !leashed || !holder->GetParentCell() || holder->IsDisabled() || leashed->IsDisabled() || holder->IsDead(true) || leashed->IsDead(true) || !CanBeginRecovery(*leashed)) {
            return false;
        }

        auto destination = holder->GetPosition();
        if (const auto holderRoot = holder->Get3D()) {
            auto backward = holderRoot->world.rotate * RE::NiPoint3{0.0F, -kTeleportSpacing, 0.0F};
            backward.z = 0.0F;
            destination += backward;
        }
        leashed->MoveTo(holder);
        bool movedToNavmesh{};
        if (!leashed->IsPlayerRef() && holder->GetParentCell()->IsAttached()) {
            leashed->SetPosition(destination, true);
            movedToNavmesh = leashed->MoveToNearestNavmesh();
        }
        a_leash.ResetSimulation();
        const char* reasonName{};
        switch (a_reason) {
            case SeparationReason::kIncompatibleSpace:
                reasonName = "incompatible space";
                break;
            case SeparationReason::kExcessiveDistance:
                reasonName = "excessive distance";
                break;
            default:
                reasonName = "player positioning";
                break;
        }
        SKSE::log::info("Teleported leashed actor {:08X} to holder {:08X}; reason={}; navmesh={}", definition.leashedFormID, definition.holderFormID, reasonName, movedToNavmesh);
        return true;
    }
}  // namespace LeashFramework
