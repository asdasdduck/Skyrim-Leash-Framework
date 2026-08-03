#pragma once

#include <span>
#include <vector>

#include "../Animation/PullPoseController.h"
#include "../PCH.h"
#include "../Physics/RopeSolver.h"
#include "../Physics/SimulationSettings.h"
#include "../Recovery/ForcedRecoveryController.h"
#include "LeashAnchor.h"
#include "LeashDefinition.h"
#include "LeashTeleportController.h"
#include "PullController.h"

namespace LeashFramework::Physics {
    class ActorBodyCollision;
}

namespace LeashFramework {
    class LeashInstance {
    public:
        LeashInstance(LeashDefinition a_definition, PullController& a_pullController, Recovery::ForcedRecoveryController& a_recoveryController, Animation::PullPoseController& a_pullPoseController);

        [[nodiscard]] const LeashDefinition& GetDefinition() const;
        void SetMinLength(float a_length) noexcept;
        void SetMaxLength(float a_length) noexcept;
        bool ReleasePull();
        bool ReleaseControl();
        void Tick(float a_deltaTime, const Physics::SimulationSettings& a_settings, const Physics::ActorBodyCollision* a_actorCollision, bool a_allowForcedRecovery);
        void FreezeSimulation();
        void ResetSimulation();
        void ApplyDeferredPose();

    private:
        friend class LeashTeleportController;

        [[nodiscard]] bool Bind(RE::Actor& a_leashed);
        void ResetBinding();
        void ReadNeutralPose();
        void ApplyPose(std::span<const RE::NiPoint3> a_neutralPositions, std::span<const RE::NiMatrix3> a_neutralRotations);

        LeashDefinition _definition;
        LeashAnchor _anchor;
        PullController& _pullController;
        Recovery::ForcedRecoveryController& _recoveryController;
        Animation::PullPoseController& _pullPoseController;
        // Controller state lives with the leash so it can't get out of sync with separate actor ID maps.
        PullController::State _pullState;
        Recovery::ForcedRecoveryController::State _recoveryState;
        Animation::PullPoseController::State _pullPoseState;
        LeashTeleportController::State _teleportState;
        RE::ActorHandle _leashed;
        RE::NiPointer<RE::NiAVObject> _boundLeashedRoot;
        std::vector<RE::NiPointer<RE::NiAVObject>> _bones;
        std::vector<RE::NiPoint3> _neutralPositions;
        std::vector<RE::NiMatrix3> _neutralRotations;
        std::vector<float> _segmentLengths;
        std::vector<RE::NiPoint3> _deferredTranslations;
        std::vector<RE::NiMatrix3> _deferredRotations;
        Physics::RopeSolver _solver;
        bool _bindingWarningLogged{};
        bool _exceeded{};
    };
}  // namespace LeashFramework
