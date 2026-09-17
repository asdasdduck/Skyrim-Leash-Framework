#pragma once

#include "../PCH.h"
#include "../Animation/PullPoseController.h"
#include "../Hooks/FrameHook.h"
#include "../Leash/LeashTeleportController.h"
#include "../Leash/PullController.h"
#include "../Movement/LeashMovementConstraint.h"
#include "../Physics/SimulationSettings.h"
#include "../Recovery/ForcedRecoveryController.h"

namespace LeashFramework::UI::SettingsPage {
    struct Values {
        Hooks::FrameHookSettings& behavior;
        Physics::SimulationSettings& simulation;
        Animation::PullPoseSettings& pose;
        LocomotionSettings& locomotion;
        Movement::HolderMovementSettings& holderMovement;
        Recovery::ForcedRecoverySettings& recovery;
        LeashTeleportSettings& teleport;
    };

    [[nodiscard]] bool Render(Values a_settings);
}
