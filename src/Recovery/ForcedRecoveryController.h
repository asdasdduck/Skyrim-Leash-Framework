#pragma once

#include "../PCH.h"

namespace LeashFramework {
    class LeashInstance;
}

namespace LeashFramework::Recovery {
    struct ForcedRecoverySettings {
        bool enableNPCs{true};
        bool enablePlayer{};
        float distanceMultiplier{2.0F};
    };

    class ForcedRecoveryController {
    public:
        ForcedRecoveryController() = default;

        [[nodiscard]] ForcedRecoverySettings GetSettings() const noexcept { return _settings; }
        void SetSettings(ForcedRecoverySettings a_settings) noexcept;

    private:
        friend class LeashFramework::LeashInstance;

        enum class Mode { kRequestingRagdoll, kPulling, kRecovering, kCooldown };

        struct State {
            Mode mode{Mode::kRequestingRagdoll};
            float insideDistanceTime{};
            float actionRetryDelay{};
            float modeElapsed{};
            float recoveryElapsed{};
            bool active{};
            bool ownsRagdoll{};
            bool requestIssued{};
            bool knockdownObserved{};
            bool getUpEndQueued{};
        };

        [[nodiscard]] bool Update(State& a_state, RE::Actor& a_actor, const RE::NiPoint3& a_collarAnchor, const RE::NiPoint3& a_anchor, const RE::NiPoint3& a_source, float a_maxLength, float a_deltaTime);
        bool Release(State& a_state);
        void BeginRecovery(State& a_state);
        void BeginCooldown(State& a_state);
        [[nodiscard]] bool UpdateRecovery(State& a_state, RE::Actor& a_actor, float a_deltaTime);

        ForcedRecoverySettings _settings;
    };
}  // namespace LeashFramework::Recovery
