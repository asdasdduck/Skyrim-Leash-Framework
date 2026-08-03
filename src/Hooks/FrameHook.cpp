#include "FrameHook.h"

#include "../Leash/LeashManager.h"
#include "../PCH.h"

namespace LeashFramework::Hooks {
    namespace {
        float& deltaTimeStub = *reinterpret_cast<float*>(REL::VariantID(523660, 410199, 0x30C3A08).address());
    }

    void FrameHook::Install() {
        static bool installed{};
        if (installed) {
            return;
        }

        auto& trampoline = SKSE::GetTrampoline();
        const auto address = REL::VariantID(35565, 36564, 0x5BAB10).address();
        const auto frameUpdateOffset = REL::VariantOffset(0x748, 0xC26, 0x7EE).offset();
        const auto lateFrameUpdateOffset = REL::VariantOffset(0x56D, 0x9DC, 0x611).offset();
        _originalFrameUpdate = trampoline.write_call<5>(address + frameUpdateOffset, OnFrameUpdate);
        _originalLateFrameUpdate = trampoline.write_call<5>(address + lateFrameUpdateOffset, OnLateFrameUpdate);
        InstallAIControlledCameraFreedomHook();
        installed = true;
        SKSE::log::info("Installed frame hooks");
    }

    FrameHookSettings FrameHook::GetSettings() { return _settings; }

    void FrameHook::SetSettings(FrameHookSettings a_settings) { _settings = a_settings; }

    void FrameHook::InstallAIControlledCameraFreedomHook() {
        REL::Relocation<std::uintptr_t> setFreeRotationMode{RELOCATION_ID(49968, 50904)};
        REL::Relocation<std::uintptr_t> callSite{setFreeRotationMode.address() + 0x43};
        constexpr std::uint8_t expectedOpcode = 0xE8;
        if (!REL::verify_code(callSite.address(), &expectedOpcode, 1)) {
            SKSE::log::critical("Unexpected AI-controlled camera movement-speed call");
            return;
        }

        _originalCameraTargetMovementSpeed = callSite.write_call<5>(OverrideCameraTargetMovementSpeed);
        SKSE::log::info("Installed AI-controlled camera freedom hook");
    }

    void FrameHook::OnFrameUpdate() {
        LF_PROFILE_REPORT();
        {
            LF_PROFILE_SCOPE("Frame/Main");
            LeashManager::GetSingleton().Tick(deltaTimeStub);
        }
        _originalFrameUpdate();
    }

    void FrameHook::OnLateFrameUpdate(void* a_this) {
        _originalLateFrameUpdate(a_this);
        LF_PROFILE_SCOPE("Frame/Late");
        LeashManager::GetSingleton().ApplyDeferredPoses();
    }

    float FrameHook::OverrideCameraTargetMovementSpeed(RE::Actor* a_target) {
        const float speed = _originalCameraTargetMovementSpeed(a_target);
        const auto* player = RE::PlayerCharacter::GetSingleton();
        if (_settings.freeCameraWhileAIControlled && player && a_target == player && player->GetPlayerRuntimeData().playerFlags.aiControlledPackage) {
            return 0.0F;
        }
        return speed;
    }
}  // namespace LeashFramework::Hooks
