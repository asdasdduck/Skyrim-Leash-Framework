#pragma once

#include "../PCH.h"

namespace LeashFramework::Hooks {
    struct FrameHookSettings {
        bool freeCameraWhileAIControlled{true};
    };

    class FrameHook {
    public:
        static void Install();
        [[nodiscard]] static FrameHookSettings GetSettings();
        static void SetSettings(FrameHookSettings a_settings);

    private:
        static void InstallAIControlledCameraFreedomHook();
        static void OnFrameUpdate();
        static void OnLateFrameUpdate(void* a_this);
        static float OverrideCameraTargetMovementSpeed(RE::Actor* a_target);
        inline static REL::Relocation<decltype(OnFrameUpdate)> _originalFrameUpdate;
        inline static REL::Relocation<decltype(OnLateFrameUpdate)> _originalLateFrameUpdate;
        inline static REL::Relocation<float (*)(RE::Actor*)> _originalCameraTargetMovementSpeed;
        inline static FrameHookSettings _settings;
    };
}  // namespace LeashFramework::Hooks
