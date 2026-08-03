#pragma once

#include <cstdint>
#include <string_view>

#include "../PCH.h"

namespace LeashFramework::Movement {
    struct DirectLocomotionState {
        std::uintptr_t process{};
        std::uintptr_t controller{};
        std::uintptr_t controllerActor{};
        std::uint32_t movementType{};
        bool highProcess{};
        bool rawControlsDriven{};
        bool controlsDriven{};
        bool aiDriven{};
        bool plannerDirect{};
        bool exactDirect{};
        bool unk1C4{};
        bool unk1C6{};
        bool unk1C8{};
        bool unk1C9{};
        bool unk1CA{};
        bool unk1CB{};

        bool operator==(const DirectLocomotionState&) const = default;
    };

    [[nodiscard]] DirectLocomotionState CaptureDirectLocomotionState(RE::Actor& a_actor);
    void LogDirectLocomotionState(RE::Actor& a_actor, std::string_view a_context);
    [[nodiscard]] bool IsDirectLocomotionActive(const RE::Actor& a_actor);
    [[nodiscard]] bool IsDirectLocomotionDriving(const RE::Actor& a_actor);
    [[nodiscard]] bool StartDirectLocomotion(RE::Actor& a_actor, bool a_diagnosticsEnabled);
    [[nodiscard]] bool DriveDirectLocomotion(RE::Actor& a_actor, const RE::NiPoint3& a_target, float a_speed);
    void StopDirectLocomotion(RE::Actor& a_actor);
}  // namespace LeashFramework::Movement
