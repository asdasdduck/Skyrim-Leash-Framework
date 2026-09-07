#include "FrameHook.h"

#include "../Leash/LeashManager.h"
#include "../Movement/LeashMovementConstraint.h"
#include "../PCH.h"

namespace LeashFramework::Hooks {
    namespace {
        float& deltaTimeStub = *reinterpret_cast<float*>(REL::VariantID(523660, 410199, 0x30C3A08).address());

        [[nodiscard]] bool ShouldSuppressGreeting(RE::Actor* a_actor) {
            auto& manager = LeashManager::GetSingleton();
            return manager.IsLeashed(a_actor) || manager.IsLeashed(RE::PlayerCharacter::GetSingleton());
        }
    }  // namespace

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
        InstallGreetingSuppressionHook();
        Movement::InstallLeashMovementConstraint();
        installed = true;
        SKSE::log::info("Installed frame hooks");
    }

    FrameHookSettings FrameHook::GetSettings() { return _settings; }

    void FrameHook::SetSettings(FrameHookSettings a_settings) { _settings = a_settings; }

    void FrameHook::InstallAIControlledCameraFreedomHook() {
        REL::Relocation<std::uintptr_t> setFreeRotationMode{REL::VariantID(49968, 50904, 0x879dd0)};
        REL::Relocation<std::uintptr_t> callSite{setFreeRotationMode.address() + 0x43};
        constexpr std::uint8_t expectedOpcode = 0xE8;
        if (!REL::verify_code(callSite.address(), &expectedOpcode, 1)) {
            SKSE::log::critical("Unexpected AI-controlled camera movement-speed call");
            return;
        }

        _originalCameraTargetMovementSpeed = callSite.write_call<5>(OverrideCameraTargetMovementSpeed);
        SKSE::log::info("Installed AI-controlled camera freedom hook");
    }

    // Todo: AE 1799 or whatever version
    void FrameHook::InstallGreetingSuppressionHook() {
        if (!REL::Module::IsSE() && !REL::Module::IsAE()) {
            SKSE::log::warn("Greeting suppression hook is not implemented for VR");
            return;
        }

        REL::Relocation<std::uintptr_t> callSite{REL::RelocationID(38601, 39632), 0x1C2};

        constexpr std::array<std::uint8_t, 5> expectedSE{0xE8, 0xD9, 0xB2, 0xC3, 0xFF};

        constexpr std::array<std::uint8_t, 5> expectedAE{0xE8, 0xB9, 0xC3, 0xBF, 0xFF};

        const auto& expected = REL::Module::IsAE() ? expectedAE : expectedSE;

        if (!REL::verify_code(callSite.address(), expected.data(), expected.size())) {
            SKSE::log::critical("Unexpected greeting-distance call");
            return;
        }

        _originalGreetingDistance = callSite.write_call<5>(OverrideGreetingDistance);

        SKSE::log::info("Installed greeting suppression hook");
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

    float FrameHook::OverrideGreetingDistance(RE::TESObjectREFR* a_source, RE::Actor* a_target, bool a_ignoreDisabled, bool a_ignoreCell) {
        const float distanceSquared = _originalGreetingDistance(a_source, a_target, a_ignoreDisabled, a_ignoreCell);
        return ShouldSuppressGreeting(a_target) ? (std::numeric_limits<float>::max)() : distanceSquared;
    }
}  // namespace LeashFramework::Hooks
