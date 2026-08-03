#include "DirectLocomotion.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <utility>

#include "../PCH.h"

/// <summary>
/// Much of the code in here is for debugging purposes, don't freak out
/// </summary>
namespace LeashFramework::Movement {
    namespace {
        constexpr std::uint32_t kPlannerDirectControl = 8;

        template <class Function, class Interface, class... Arguments>
        decltype(auto) CallVirtual(Interface* a_interface, std::size_t a_index, Arguments&&... a_arguments) {
            const auto vtable = *reinterpret_cast<std::uintptr_t**>(a_interface);
            return reinterpret_cast<Function>(vtable[a_index])(a_interface, std::forward<Arguments>(a_arguments)...);
        }

        [[nodiscard]] RE::MovementControllerNPC* GetMovementController(RE::Actor& a_actor) { return a_actor.GetActorRuntimeData().movementController.get(); }

        [[nodiscard]] const RE::MovementControllerNPC* GetMovementController(const RE::Actor& a_actor) { return a_actor.GetActorRuntimeData().movementController.get(); }

        [[nodiscard]] bool IsDriving(const RE::MovementControllerNPC& a_controller) { return a_controller.unk1C0 == kPlannerDirectControl && a_controller.unk1C7; }
    }  // namespace

    DirectLocomotionState CaptureDirectLocomotionState(RE::Actor& a_actor) {
        DirectLocomotionState state;
        auto* process = a_actor.GetActorRuntimeData().currentProcess;
        state.process = reinterpret_cast<std::uintptr_t>(process);
        state.highProcess = process && process->InHighProcess();

        auto* controller = GetMovementController(a_actor);
        if (!controller) {
            return state;
        }
        state.controller = reinterpret_cast<std::uintptr_t>(controller);
        state.controllerActor = reinterpret_cast<std::uintptr_t>(controller->actor);
        state.movementType = controller->unk1C0;
        state.rawControlsDriven = controller->controlsDriven;
        state.controlsDriven = controller->GetControlsDriven();
        state.aiDriven = controller->GetAIDriven();
        state.plannerDirect = controller->unk1C7;
        state.exactDirect = IsDriving(*controller);
        state.unk1C4 = controller->unk1C4;
        state.unk1C6 = controller->unk1C6;
        state.unk1C8 = controller->unk1C8;
        state.unk1C9 = controller->unk1C9;
        state.unk1CA = controller->unk1CA;
        state.unk1CB = controller->unk1CB;
        return state;
    }

    void LogDirectLocomotionState(RE::Actor& a_actor, std::string_view a_context) {
        const auto state = CaptureDirectLocomotionState(a_actor);
        const auto* actorState = a_actor.AsActorState();
        SKSE::log::info(
            "[PullDiag] {} actor={:08X} actorPtr={:X} loaded={} process={:X} high={} controller={:X} controllerActor={:X} movementType={} controlsDriven={} aiDriven={} plannerDirect={} exactDirect={} "
            "rawFlags=[c4={},c5={},c6={},c7={},c8={},c9={},ca={},cb={}] playerControls={} animationDriven={} allowRotation={} ragdoll={} knock={} sit={}",
            a_context, a_actor.GetFormID(), reinterpret_cast<std::uintptr_t>(std::addressof(a_actor)), a_actor.Is3DLoaded(), state.process, state.highProcess, state.controller, state.controllerActor, state.movementType,
            state.controlsDriven, state.aiDriven, state.plannerDirect, state.exactDirect, state.unk1C4, state.rawControlsDriven, state.unk1C6, state.plannerDirect, state.unk1C8, state.unk1C9, state.unk1CA, state.unk1CB,
            a_actor.GetPlayerControls(), a_actor.IsAnimationDriven(), a_actor.IsAllowRotation(), a_actor.IsInRagdollState(), actorState ? std::to_underlying(actorState->GetKnockState()) : 0,
            actorState ? std::to_underlying(actorState->GetSitSleepState()) : 0);

        const auto* process = a_actor.GetActorRuntimeData().currentProcess;
        const auto* package = a_actor.GetCurrentPackage();
        const auto* characterController = a_actor.GetCharController();
        const auto position = a_actor.GetPosition();
        SKSE::log::info(
            "[PullDiag] {} actorMotion={:08X} position=[{:.2f},{:.2f},{:.2f}] cell={:08X} package={:08X} packageType={} procedure={} procedureIndex={} processFlags=[follower={},escortPlayer={},skipPathing={}] "
            "charController={:X} charFlags=[noSim={},farAway={},stuckQuad={},noCharCollisions={}] outVelocity=[{:.3f},{:.3f},{:.3f}] speedPct={:.3f} contacts={}",
            a_context, a_actor.GetFormID(), position.x, position.y, position.z, a_actor.GetParentCell() ? a_actor.GetParentCell()->GetFormID() : 0, package ? package->GetFormID() : 0,
            package ? std::to_underlying(package->packData.packType.get()) : 0, package ? std::to_underlying(package->procedureType.get()) : 0, process ? process->currentPackage.currentProcedureIndex : -1,
            process && process->lowProcessFlags.all(RE::AIProcess::LowProcessFlags::kFollower), process && process->escortingPlayer, process && process->skippedTimeStampForPathing,
            reinterpret_cast<std::uintptr_t>(characterController), characterController && characterController->flags.all(RE::CHARACTER_FLAGS::kNoSim),
            characterController && characterController->flags.all(RE::CHARACTER_FLAGS::kFarAway), characterController && characterController->flags.all(RE::CHARACTER_FLAGS::kStuckQuad),
            characterController && characterController->flags.all(RE::CHARACTER_FLAGS::kNoCharacterCollisions), characterController ? characterController->outVelocity.quad.m128_f32[0] : 0.0F,
            characterController ? characterController->outVelocity.quad.m128_f32[1] : 0.0F, characterController ? characterController->outVelocity.quad.m128_f32[2] : 0.0F,
            characterController ? characterController->speedPct : 0.0F, characterController ? characterController->contactPointsCount : 0);
    }

    bool IsDirectLocomotionActive(const RE::Actor& a_actor) {
        const auto* controller = GetMovementController(a_actor);
        return controller && controller->unk1C7;
    }

    bool IsDirectLocomotionDriving(const RE::Actor& a_actor) {
        const auto* controller = GetMovementController(a_actor);
        return controller && IsDriving(*controller);
    }

    bool StartDirectLocomotion(RE::Actor& a_actor, bool a_diagnosticsEnabled) {
        auto* controller = GetMovementController(a_actor);
        if (!controller || IsDirectLocomotionActive(a_actor)) {
            return false;
        }

        bool traceAttempt{};
        if (a_diagnosticsEnabled) {
            static std::unordered_map<RE::FormID, DirectLocomotionState> lastTracedStarts;
            const auto state = CaptureDirectLocomotionState(a_actor);
            const auto lastState = lastTracedStarts.find(a_actor.GetFormID());
            traceAttempt = lastState == lastTracedStarts.end() || lastState->second != state;
            lastTracedStarts.insert_or_assign(a_actor.GetFormID(), state);
            if (traceAttempt) {
                LogDirectLocomotionState(a_actor, "StartDirect before motion[1]");
            }
        }

        auto* motionControl = static_cast<RE::IMovementMotionDrivenControl*>(controller);
        auto* plannerControl = static_cast<RE::IMovementPlannerDirectControl*>(controller);
        using MotionCommand = void (*)(RE::IMovementMotionDrivenControl*);
        using PlannerCommand = void (*)(RE::IMovementPlannerDirectControl*);
        CallVirtual<MotionCommand>(motionControl, 1);
        if (traceAttempt) {
            LogDirectLocomotionState(a_actor, "StartDirect after motion[1]");
        }
        CallVirtual<PlannerCommand>(plannerControl, 1);
        if (traceAttempt) {
            LogDirectLocomotionState(a_actor, "StartDirect after planner[1]");
        }
        CallVirtual<MotionCommand>(motionControl, 4);
        if (traceAttempt) {
            LogDirectLocomotionState(a_actor, "StartDirect after motion[4]");
        }
        if (IsDriving(*controller)) {
            return true;
        }
        CallVirtual<PlannerCommand>(plannerControl, 5);
        if (traceAttempt) {
            LogDirectLocomotionState(a_actor, "StartDirect after rollback planner[5]");
        }
        return false;
    }

    bool DriveDirectLocomotion(RE::Actor& a_actor, const RE::NiPoint3& a_target, float a_speed) {
        auto* controller = GetMovementController(a_actor);
        if (!controller || !IsDriving(*controller)) {
            return false;
        }

        auto direction = a_target - a_actor.GetPosition();
        direction.z = 0.0F;
        const auto length = direction.Unitize();
        const auto speed = length > 0.001F ? std::max(a_speed, 0.0F) : 0.0F;
        const RE::NiPoint3 targetAngle{0.0F, 0.0F, length > 0.001F ? std::atan2(direction.x, direction.y) : a_actor.GetAngleZ()};

        auto* plannerControl = static_cast<RE::IMovementPlannerDirectControl*>(controller);
        using SetPoint = void (*)(RE::IMovementPlannerDirectControl*, const RE::NiPoint3&);
        using SetSpeed = void (*)(RE::IMovementPlannerDirectControl*, float);
        CallVirtual<SetSpeed>(plannerControl, 3, speed);
        if (speed > 0.0F) {
            CallVirtual<SetPoint>(plannerControl, 2, targetAngle);
        }
        CallVirtual<SetPoint>(plannerControl, 4, targetAngle);
        return true;
    }

    void StopDirectLocomotion(RE::Actor& a_actor) {
        auto* controller = GetMovementController(a_actor);
        if (!controller || !controller->unk1C7) {
            return;
        }
        auto* plannerControl = static_cast<RE::IMovementPlannerDirectControl*>(controller);
        using PlannerCommand = void (*)(RE::IMovementPlannerDirectControl*);
        CallVirtual<PlannerCommand>(plannerControl, 5);
    }
}  // namespace LeashFramework::Movement
