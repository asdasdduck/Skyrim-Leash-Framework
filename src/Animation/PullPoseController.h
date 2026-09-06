#pragma once

#include <array>
#include <vector>

#include "../PCH.h"

namespace LeashFramework {
    class LeashInstance;
}

namespace LeashFramework::Animation {
    struct PullPoseSettings {
        bool enabled{true};
        float minimumStrength{0.3F};
        float maximumStrength{1.0F};
        float maximumAngleDegrees{50.0F};
        float responseRate{12.0F};
        float anticipationTime{0.0F};
        float slackReserveRatio{0.0F};
    };

    class PullPoseController {
    public:
        [[nodiscard]] PullPoseSettings GetSettings() const noexcept { return _settings; }
        void SetSettings(PullPoseSettings a_settings) noexcept;

    private:
        friend class LeashFramework::LeashInstance;

        enum class Bone : std::size_t { kSpine, kSpine1, kSpine2, kNeck, kTotal };

        struct DeferredTransform {
            RE::NiPointer<RE::NiAVObject> object;
            RE::NiPoint3 translation;
            RE::NiMatrix3 rotation;
        };

        struct State {
            RE::NiPointer<RE::NiAVObject> root;
            std::array<RE::NiPointer<RE::NiAVObject>, static_cast<std::size_t>(Bone::kTotal)> bones;
            RE::NiPoint3 ropeDirection;
            RE::NiPoint3 smoothedDirection;
            RE::NiPoint3 axis;
            std::array<float, static_cast<std::size_t>(Bone::kTotal)> angles{};
            std::vector<DeferredTransform> deferredTransforms;
            float smoothedStrength{};
            float previousDistance{};
            float separationSpeed{};
            bool hasDistanceSample{};
            bool hasSmoothedDirection{};
            bool tensionEngaged{};
            bool prepared{};
            bool frozen{};
        };

        void Prepare(State& a_state, RE::Actor& a_actor, const RE::NiAVObject* a_attachmentNode, const RE::NiPoint3& a_attachment, const RE::NiPoint3& a_anchor, float a_ropeLength, float a_deltaTime, bool a_allowed);
        void Transform(const State& a_state, const RE::NiAVObject& a_object, RE::NiPoint3& a_position, RE::NiMatrix3& a_rotation) const;
        void Capture(State& a_state, const RE::NiPoint3& a_collar, const RE::NiPoint3& a_nextRopePoint);
        void Apply(State& a_state, RE::Actor& a_actor);
        void Freeze(State& a_state);
        void Reset(State& a_state);
        [[nodiscard]] bool Bind(State& a_state, RE::Actor& a_actor);
        void BuildPose(State& a_state, const RE::NiPoint3& a_attachment, float a_strength);
        void CaptureDeferredPose(State& a_state);
        void CaptureDeferredPose(State& a_state, RE::NiAVObject& a_object, std::size_t& a_index);
        void ApplyFrozenPose(State& a_state);

        PullPoseSettings _settings;
    };
}  // namespace LeashFramework::Animation
