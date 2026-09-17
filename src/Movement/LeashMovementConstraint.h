#pragma once

#include "../PCH.h"

namespace LeashFramework::Movement {
    struct HolderMovementSettings {
        bool preventOverstretch{true};
        float stretchAllowance{};
    };

    [[nodiscard]] HolderMovementSettings GetHolderMovementSettings();
    void SetHolderMovementSettings(HolderMovementSettings a_settings);
    void InstallLeashMovementConstraint();
    [[nodiscard]] bool CanConstrainNativeMovement(const RE::Actor& a_actor);
    void UpdateLeashMovementConstraint(RE::MovementControllerNPC*& a_binding, RE::Actor& a_actor, const RE::NiPoint3& a_collar, const RE::NiPoint3& a_anchor,
        const RE::NiPoint3& a_goal, float a_ropeLength, float a_maxLength);
    void UpdateHolderMovementConstraint(RE::MovementControllerNPC*& a_binding, RE::Actor& a_holder, const RE::NiPoint3& a_attachment,
        const RE::NiPoint3& a_leanLimitAttachment, float a_ropeLength);
    void ClearLeashMovementConstraint(RE::MovementControllerNPC*& a_binding);
}  // namespace LeashFramework::Movement
