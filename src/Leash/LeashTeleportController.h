#pragma once

namespace LeashFramework {
    class LeashInstance;

    struct LeashTeleportSettings {
        float gracePeriod{1.5F};
        float playerDistance{2048.0F};
        float npcDistance{2048.0F};
    };

    class LeashTeleportController {
    public:
        enum class UpdateResult { kNone, kPending, kTeleported };

        LeashTeleportController() = default;

        [[nodiscard]] LeashTeleportSettings GetSettings() const noexcept { return _settings; }
        void SetSettings(LeashTeleportSettings a_settings) noexcept;

        [[nodiscard]] bool HandlePlayerPositioned(LeashInstance& a_leash);
        [[nodiscard]] UpdateResult Update(LeashInstance& a_leash, float a_deltaTime);

    private:
        friend class LeashInstance;

        enum class SeparationReason { kNone, kIncompatibleSpace, kExcessiveDistance };

        struct State {
            SeparationReason reason{SeparationReason::kNone};
            float graceElapsed{};
            float cooldownRemaining{};
            bool pendingPlayerPosition{};
        };

        [[nodiscard]] bool TeleportToPlayerHolder(LeashInstance& a_leash);
        [[nodiscard]] bool Teleport(LeashInstance& a_leash, SeparationReason a_reason);

        LeashTeleportSettings _settings;
    };
}  // namespace LeashFramework
