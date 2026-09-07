#pragma once

#include "../PCH.h"

namespace LeashFramework::Movement {
    void InstallLeashMovementConstraint();
    [[nodiscard]] bool CanConstrainNativeMovement(const RE::Actor& a_actor);
    void UpdateLeashMovementConstraint(RE::MovementControllerNPC*& a_binding, RE::Actor& a_actor, const RE::NiPoint3& a_collar, const RE::NiPoint3& a_anchor,
        const RE::NiPoint3& a_goal, float a_ropeLength, float a_maxLength);
    void ClearLeashMovementConstraint(RE::MovementControllerNPC*& a_binding);
}  // namespace LeashFramework::Movement
