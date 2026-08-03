#include "PullPoseController.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <string_view>
#include <utility>

#include "../PCH.h"

namespace LeashFramework::Animation {
    namespace {
        constexpr std::array<std::string_view, 4> kBoneNames{"NPC Spine [Spn0]", "NPC Spine1 [Spn1]", "NPC Spine2 [Spn2]", "NPC Neck [Neck]"};
        constexpr std::array<float, 3> kLowerAttachmentWeights{0.95F, -0.35F, -0.55F};  // This is like waist ropes
        constexpr std::array<float, 3> kUpperAttachmentWeights{0.20F, 0.35F, 0.45F}; // Neck ropes!
        constexpr float kEngageThreshold = 0.10F;
        constexpr float kReleaseThreshold = 0.05F;
        constexpr float kNeckCounterRotation = 0.20F;
        constexpr float kDirectionEpsilon = 0.0001F;
        constexpr float kMinimumVisibleStrength = 0.0001F;

        [[nodiscard]] bool IsDescendantOf(const RE::NiAVObject* a_object, const RE::NiAVObject* a_root) {
            for (auto* object = a_object; object; object = object->parent) {
                if (object == a_root) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] float SmoothStep(float a_minimum, float a_maximum, float a_value) {
            const auto normalized = std::clamp((a_value - a_minimum) / (a_maximum - a_minimum), 0.0F, 1.0F);
            return normalized * normalized * (3.0F - 2.0F * normalized);
        }

        [[nodiscard]] float AttachmentHeight(const std::array<RE::NiPointer<RE::NiAVObject>, 4>& a_bones, const RE::NiPoint3& a_attachment) {
            std::array<float, 3> lengths{};
            float totalLength{};
            for (std::size_t index = 0; index < lengths.size(); ++index) {
                lengths[index] = a_bones[index]->world.translate.GetDistance(a_bones[index + 1]->world.translate);
                totalLength += lengths[index];
            }
            if (totalLength <= kDirectionEpsilon) {
                return 1.0F;
            }

            float bestDistanceSquared = std::numeric_limits<float>::max();
            float bestHeight{};
            float precedingLength{};
            for (std::size_t index = 0; index < lengths.size(); ++index) {
                const auto start = a_bones[index]->world.translate;
                const auto segment = a_bones[index + 1]->world.translate - start;
                const auto segmentLengthSquared = segment.SqrLength();
                const auto fraction = segmentLengthSquared > kDirectionEpsilon ? std::clamp((a_attachment - start).Dot(segment) / segmentLengthSquared, 0.0F, 1.0F) : 0.0F;
                const auto distanceSquared = (a_attachment - (start + segment * fraction)).SqrLength();
                if (distanceSquared < bestDistanceSquared) {
                    bestDistanceSquared = distanceSquared;
                    bestHeight = (precedingLength + lengths[index] * fraction) / totalLength;
                }
                precedingLength += lengths[index];
            }
            return std::clamp(bestHeight, 0.0F, 1.0F);
        }

        void ApplyRotation(RE::NiAVObject& a_object, const RE::NiPoint3& a_pivot, const RE::NiMatrix3& a_rotation) {
            // the parent's rigid world delta must be carried through every descendant manually
            a_object.world.translate = a_pivot + a_rotation * (a_object.world.translate - a_pivot);
            a_object.world.rotate = a_rotation * a_object.world.rotate;
            if (auto* node = a_object.AsNode()) {
                for (const auto& child : node->GetChildren()) {
                    if (child) {
                        ApplyRotation(*child, a_pivot, a_rotation);
                    }
                }
            }
        }
    }  // namespace

    void PullPoseController::SetSettings(PullPoseSettings a_settings) noexcept {
        const PullPoseSettings defaults;
        a_settings.minimumStrength = std::isfinite(a_settings.minimumStrength) ? std::clamp(a_settings.minimumStrength, 0.0F, 1.0F) : defaults.minimumStrength;
        const auto maximumStrength = std::isfinite(a_settings.maximumStrength) ? a_settings.maximumStrength : defaults.maximumStrength;
        a_settings.maximumStrength = std::clamp(maximumStrength, a_settings.minimumStrength, 1.0F);
        a_settings.maximumAngleDegrees = std::isfinite(a_settings.maximumAngleDegrees) ? std::clamp(a_settings.maximumAngleDegrees, 0.0F, 50.0F) : defaults.maximumAngleDegrees;
        a_settings.responseRate = std::isfinite(a_settings.responseRate) ? std::clamp(a_settings.responseRate, 0.1F, 30.0F) : defaults.responseRate;
        _settings = a_settings;
    }

    void PullPoseController::Prepare(State& a_state, RE::Actor& a_actor, RE::NiAVObject& a_attachment, float a_deltaTime, bool a_allowed) {
        if (!_settings.enabled || !a_allowed || a_actor.IsDead(false) || a_actor.IsInRagdollState() || !std::isfinite(a_deltaTime) || a_deltaTime <= 0.0F) {
            Reset(a_state);
            return;
        }
        a_state.frozen = false;

        const auto response = 1.0F - std::exp(-_settings.responseRate * a_deltaTime);
        a_state.smoothedStrength = std::lerp(a_state.smoothedStrength, a_state.pending.strength, response);
        if (a_state.pending.strength > 0.0F) {
            if (!a_state.hasSmoothedDirection) {
                a_state.smoothedDirection = a_state.pending.direction;
                a_state.hasSmoothedDirection = true;
            } else {
                auto direction = a_state.smoothedDirection * (1.0F - response) + a_state.pending.direction * response;
                if (direction.Unitize() > kDirectionEpsilon) {
                    a_state.smoothedDirection = direction;
                } else {
                    a_state.smoothedDirection = a_state.pending.direction;
                }
            }
        }

        a_state.prepared = false;
        if (a_state.smoothedStrength <= kMinimumVisibleStrength || !a_state.hasSmoothedDirection || !Bind(a_state, a_actor)) {
            a_state.deferredTransforms.clear();
            return;
        }
        BuildPose(a_state, a_attachment);
        if (!a_state.prepared) {
            a_state.deferredTransforms.clear();
        }
    }

    void PullPoseController::Transform(const State& a_state, const RE::NiAVObject& a_object, RE::NiPoint3& a_position, RE::NiMatrix3& a_rotation) const {
        if (!a_state.prepared) {
            return;
        }

        std::array<RE::NiPoint3, 4> pivots;
        for (std::size_t index = 0; index < pivots.size(); ++index) {
            pivots[index] = a_state.bones[index]->world.translate;
        }
        for (std::size_t index = 0; index < pivots.size(); ++index) {
            RE::NiMatrix3 delta;
            delta.MakeRotation(a_state.angles[index], a_state.axis);
            const auto pivot = pivots[index];
            for (std::size_t descendant = index + 1; descendant < pivots.size(); ++descendant) {
                pivots[descendant] = pivot + delta * (pivots[descendant] - pivot);
            }
            if (IsDescendantOf(&a_object, a_state.bones[index].get())) {
                a_position = pivot + delta * (a_position - pivot);
                a_rotation = delta * a_rotation;
            }
        }
    }

    void PullPoseController::Capture(State& a_state, std::span<const RE::NiPoint3> a_ropePositions, float a_distance, float a_minLength, float a_maxLength) {
        a_state.pending = {};
        if (!_settings.enabled || a_ropePositions.size() < 2 || !std::isfinite(a_distance)) {
            a_state.tensionEngaged = false;
            return;
        }

        const auto tensionRange = std::max(a_maxLength - a_minLength, 1.0F);
        const auto normalizedTension = std::clamp((a_distance - a_minLength) / tensionRange, 0.0F, 1.0F);
        if (a_state.tensionEngaged) {
            a_state.tensionEngaged = normalizedTension > kReleaseThreshold;
        } else {
            a_state.tensionEngaged = normalizedTension > kEngageThreshold;
        }
        if (!a_state.tensionEngaged) {
            return;
        }

        auto direction = a_ropePositions[1] - a_ropePositions[0];
        if (direction.Unitize() <= kDirectionEpsilon) {
            return;
        }
        const auto blend = SmoothStep(kEngageThreshold, 1.0F, normalizedTension);
        a_state.pending = {.direction = direction, .strength = blend * std::lerp(_settings.minimumStrength, _settings.maximumStrength, blend)};
    }

    void PullPoseController::Apply(State& a_state, RE::Actor& a_actor) {
        if (!_settings.enabled || !a_state.prepared || a_actor.IsDead(false) || a_actor.IsInRagdollState() || !Bind(a_state, a_actor)) {
            return;
        }
        if (a_state.frozen) {
            ApplyFrozenPose(a_state);
            return;
        }

        for (std::size_t index = 0; index < a_state.bones.size(); ++index) {
            if (std::abs(a_state.angles[index]) <= kDirectionEpsilon) {
                continue;
            }
            RE::NiMatrix3 rotation;
            rotation.MakeRotation(a_state.angles[index], a_state.axis);
            ApplyRotation(*a_state.bones[index], a_state.bones[index]->world.translate, rotation);
        }
        CaptureDeferredPose(a_state);
    }

    void PullPoseController::Freeze(State& a_state) { a_state.frozen = true; }

    void PullPoseController::Reset(State& a_state) { a_state = {}; }

    bool PullPoseController::Bind(State& a_state, RE::Actor& a_actor) {
        auto* root = a_actor.Get3D(false);
        if (!root) {
            a_state.root.reset();
            a_state.bones = {};
            return false;
        }

        const auto validBinding = a_state.root.get() == root && std::ranges::all_of(a_state.bones, [&](const auto& a_bone) { return a_bone && IsDescendantOf(a_bone.get(), root); });
        if (validBinding) {
            return true;
        }

        a_state.root.reset(root);
        for (std::size_t index = 0; index < a_state.bones.size(); ++index) {
            a_state.bones[index].reset(root->GetObjectByName(RE::BSFixedString(kBoneNames[index])));
        }
        if (std::ranges::contains(a_state.bones, nullptr)) {
            a_state.bones = {};
            return false;
        }
        for (std::size_t index = 1; index < a_state.bones.size(); ++index) {
            if (!IsDescendantOf(a_state.bones[index].get(), a_state.bones[index - 1].get())) {
                a_state.bones = {};
                return false;
            }
        }
        return true;
    }

    void PullPoseController::BuildPose(State& a_state, RE::NiAVObject& a_attachment) {
        auto spineDirection = a_state.bones[std::to_underlying(Bone::kNeck)]->world.translate - a_state.bones[std::to_underlying(Bone::kSpine)]->world.translate;
        if (spineDirection.Unitize() <= kDirectionEpsilon) {
            return;
        }

        auto axis = a_state.smoothedDirection.Cross(spineDirection);
        const auto bendFactor = axis.Unitize();
        if (bendFactor <= kDirectionEpsilon) {
            return;
        }

        // Height will be like ~0.25 for the current waist rope, 1 for neck rope
        const auto height = AttachmentHeight(a_state.bones, a_attachment.world.translate);
        const auto totalAngle = _settings.maximumAngleDegrees * std::numbers::pi_v<float> / 180.0F * a_state.smoothedStrength * bendFactor;

        a_state.axis = axis;
        for (std::size_t index = 0; index < 3; ++index) {
            a_state.angles[index] = totalAngle * std::lerp(kLowerAttachmentWeights[index], kUpperAttachmentWeights[index], height);
        }
        a_state.angles[std::to_underlying(Bone::kNeck)] = -totalAngle * kNeckCounterRotation;
        a_state.prepared = true;
    }

    void PullPoseController::CaptureDeferredPose(State& a_state) {
        std::size_t index{};
        CaptureDeferredPose(a_state, *a_state.bones[std::to_underlying(Bone::kSpine)], index);
        a_state.deferredTransforms.resize(index);
    }

    void PullPoseController::CaptureDeferredPose(State& a_state, RE::NiAVObject& a_object, std::size_t& a_index) {
        if (a_index == a_state.deferredTransforms.size()) {
            a_state.deferredTransforms.emplace_back();
        }
        auto& transform = a_state.deferredTransforms[a_index++];
        transform.object.reset(&a_object);
        transform.translation = a_object.world.translate;
        transform.rotation = a_object.world.rotate;
        if (auto* node = a_object.AsNode()) {
            for (const auto& child : node->GetChildren()) {
                if (child) {
                    CaptureDeferredPose(a_state, *child, a_index);
                }
            }
        }
    }

    void PullPoseController::ApplyFrozenPose(State& a_state) {
        if (a_state.deferredTransforms.empty() ||
            std::ranges::any_of(a_state.deferredTransforms, [&](const auto& a_transform) { return !a_transform.object || !IsDescendantOf(a_transform.object.get(), a_state.root.get()); })) {
            a_state.deferredTransforms.clear();
            return;
        }
        for (const auto& transform : a_state.deferredTransforms) {
            transform.object->world.translate = transform.translation;
            transform.object->world.rotate = transform.rotation;
        }
    }
}  // namespace LeashFramework::Animation
