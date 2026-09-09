#pragma once

#include "../PCH.h"

namespace LeashFramework::Hooks {
    struct FrameHookSettings {
        bool freeCameraWhileAIControlled{true};
        bool suppressGreetingsWhileLeashed{true};
    };

    class FrameHook {
    public:
        static void Install();
        [[nodiscard]] static FrameHookSettings GetSettings();
        static void SetSettings(FrameHookSettings a_settings);

    private:
        static void InstallAIControlledCameraFreedomHook();
        static void InstallGreetingSuppressionHook();
        static void OnFrameUpdate();
        static void OnLateFrameUpdate(void* a_this);
        static float OverrideCameraTargetMovementSpeed(RE::Actor* a_target);
        static float OverrideGreetingDistance(RE::TESObjectREFR* a_source, RE::Actor* a_target, bool a_ignoreDisabled, bool a_ignoreCell);
        inline static REL::Relocation<decltype(OnFrameUpdate)> _originalFrameUpdate;
        inline static REL::Relocation<decltype(OnLateFrameUpdate)> _originalLateFrameUpdate;
        inline static REL::Relocation<float (*)(RE::Actor*)> _originalCameraTargetMovementSpeed;
        inline static REL::Relocation<decltype(OverrideGreetingDistance)> _originalGreetingDistance;
        inline static FrameHookSettings _settings;
    };
}  // namespace LeashFramework::Hooks
