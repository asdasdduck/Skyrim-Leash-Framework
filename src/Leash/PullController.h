#pragma once

#include <cstddef>
#include <vector>

#include "../PCH.h"

namespace LeashFramework {
    class LeashInstance;

    class PullController {
    public:
        PullController() = default;

        [[nodiscard]] bool DiagnosticsEnabled() const noexcept { return _diagnosticsEnabled; }
        void SetDiagnosticsEnabled(bool a_enabled) noexcept { _diagnosticsEnabled = a_enabled; }

    private:
        friend class LeashInstance;

        struct State {
            std::vector<RE::NiPoint3> path;
            std::size_t waypointIndex{};
            std::size_t stableDirectFrames{};
            RE::NiPoint3 lastGoal;
            float replanDelay{};
            float smoothedPlayerEffort{};
            bool active{};
            bool restorePlayerControls{};
        };

        void Update(State& a_state, RE::Actor& a_actor, const RE::NiPoint3& a_collarAnchor, const RE::NiPoint3& a_goal, RE::TESObjectCELL* a_goalCell, float a_minLength, float a_maxLength, float a_deltaTime);
        bool Release(State& a_state, RE::Actor* a_actor);

        bool _diagnosticsEnabled{};
    };
}  // namespace LeashFramework
