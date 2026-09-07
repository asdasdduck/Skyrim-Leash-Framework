#pragma once

#include <cstddef>
#include <vector>

#include "../PCH.h"

namespace LeashFramework {
    struct LocomotionSettings {
        float forwardAssistance{1.0F};
        float backwardResistance{1.5F};
        float minimumForcedPullRatio{0.5F};
        float maximumCatchUpSpeed{3.0F};
        float movingFollowGap{0.4F};
        float distanceResponseRate{2.5F};
    };

    class LeashInstance;

    class PullController {
    public:
        PullController() = default;

        [[nodiscard]] LocomotionSettings GetSettings() const noexcept { return _settings; }
        void SetSettings(LocomotionSettings a_settings) noexcept;

        [[nodiscard]] bool DiagnosticsEnabled() const noexcept { return _diagnosticsEnabled; }
        void SetDiagnosticsEnabled(bool a_enabled) noexcept { _diagnosticsEnabled = a_enabled; }

    private:
        friend class LeashInstance;

        struct MotionState {
            RE::NiPoint3 previousGoal;
            RE::NiPoint3 velocity;
            float stationaryTime{};
            float movingBlend{};
            bool hasSample{};
            bool moving{};
        };

        struct State {
            MotionState motion;
            RE::MovementControllerNPC* nativeMovementBinding{};
            std::vector<RE::NiPoint3> path;
            std::size_t waypointIndex{};
            std::size_t stableDirectFrames{};
            RE::NiPoint3 lastGoal;
            float replanDelay{};
            float smoothedPlayerEffort{};
            float commandedSpeed{};
            float idleTime{};
            float retryDelay{};
            bool active{};
            bool restorePlayerControls{};
        };

        void Update(State& a_state, RE::Actor& a_actor, const RE::NiPoint3& a_collarAnchor, const RE::NiPoint3& a_anchor, float a_ropeLength, const RE::NiPoint3& a_goal, RE::TESObjectCELL* a_goalCell, bool a_hasHolder, float a_minLength, float a_maxLength, float a_deltaTime);
        void ResetMotion(State& a_state);
        bool Release(State& a_state, RE::Actor* a_actor, bool a_keepMotion = false);

        LocomotionSettings _settings;
        bool _diagnosticsEnabled{};
    };
}  // namespace LeashFramework
