#include "LeashInstance.h"

#include <algorithm>
#include <cmath>
#include <string_view>

#include "../Movement/DirectLocomotion.h"
#include "../PCH.h"
#include "../Recovery/ForcedRecoveryController.h"
#include "PullController.h"

namespace LeashFramework {
    namespace {
        constexpr float kDirectionEpsilon = 0.0001F;

        [[nodiscard]] bool CanSimulateTogether(const RE::TESObjectCELL* a_anchorCell, const RE::Actor& a_leashed) {
            const auto* leashedCell = a_leashed.GetParentCell();
            if (!a_anchorCell || !leashedCell || !a_anchorCell->IsAttached() || !leashedCell->IsAttached()) {
                return false;
            }
            if (a_anchorCell == leashedCell) {
                return true;
            }
            if (!a_anchorCell->IsExteriorCell() || !leashedCell->IsExteriorCell()) {
                return false;
            }
            const auto* worldSpace = a_anchorCell->GetRuntimeData().worldSpace;
            return worldSpace && worldSpace == leashedCell->GetRuntimeData().worldSpace;
        }

        void CollectLeashBones(RE::NiAVObject* a_object, std::string_view a_match, std::vector<RE::NiPointer<RE::NiAVObject>>& a_bones) {
            if (!a_object) {
                return;
            }

            const std::string_view name = a_object->name;
            if (name.contains(a_match)) {
                a_bones.emplace_back(a_object);
            }

            auto* node = a_object->AsNode();
            if (!node) {
                return;
            }

            for (const auto& child : node->GetChildren()) {
                if (child) {
                    CollectLeashBones(child.get(), a_match, a_bones);
                }
            }
        }

        // Detached nodes remain alive through NiPointer but no longer lead to the actor root through parent links
        [[nodiscard]] bool IsDescendantOf(const RE::NiAVObject* a_object, const RE::NiAVObject* a_root) {
            for (auto* object = a_object; object; object = object->parent) {
                if (object == a_root) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] RE::NiMatrix3 AlignRotation(const RE::NiMatrix3& a_neutralRotation, RE::NiPoint3 a_from, RE::NiPoint3 a_to) {
            if (a_from.Unitize() <= kDirectionEpsilon || a_to.Unitize() <= kDirectionEpsilon) {
                return a_neutralRotation;
            }

            const auto dot = std::clamp(a_from.Dot(a_to), -1.0F, 1.0F);
            if (dot > 1.0F - kDirectionEpsilon) {
                return a_neutralRotation;
            }

            auto axis = a_to.Cross(a_from);
            if (axis.Unitize() <= kDirectionEpsilon) {
                axis = a_from.Cross({1.0F, 0.0F, 0.0F});
                if (axis.Unitize() <= kDirectionEpsilon) {
                    axis = a_from.Cross({0.0F, 1.0F, 0.0F});
                    axis.Unitize();
                }
            }

            RE::NiMatrix3 alignment;
            alignment.MakeRotation(std::acos(dot), axis);
            return alignment * a_neutralRotation;
        }
    }  // namespace

    LeashInstance::LeashInstance(LeashDefinition a_definition, PullController& a_pullController, Recovery::ForcedRecoveryController& a_recoveryController, Animation::PullPoseController& a_pullPoseController)
        : _definition(std::move(a_definition)), _anchor(_definition), _pullController(a_pullController), _recoveryController(a_recoveryController), _pullPoseController(a_pullPoseController) {
        if (auto* holder = RE::TESForm::LookupByID<RE::Actor>(_definition.holderFormID)) {
            _holder = holder->GetHandle();
        }
        if (auto* leashed = RE::TESForm::LookupByID<RE::Actor>(_definition.leashedFormID)) {
            _leashed = leashed->GetHandle();
        }
    }

    const LeashDefinition& LeashInstance::GetDefinition() const { return _definition; }

    void LeashInstance::SetMinLength(float a_length) noexcept { _definition.minLength = a_length; }

    void LeashInstance::SetMaxLength(float a_length) noexcept { _definition.maxLength = a_length; }

    bool LeashInstance::ReleasePull() {
        auto leashed = _leashed.get();
        return _pullController.Release(_pullState, leashed.get());
    }

    bool LeashInstance::ReleaseControl() {
        const auto releasedPull = ReleasePull();
        const auto releasedRecovery = _recoveryController.Release(_recoveryState);
        _pullPoseController.Reset(_pullPoseState);
        return releasedPull || releasedRecovery;
    }

    // Todo: Clean up this constant statement swap jank. Such logic should be abstracted 
    void LeashInstance::Tick(float a_deltaTime, const Physics::SimulationSettings& a_settings, const Physics::ActorBodyCollision* a_actorCollision, bool a_allowForcedRecovery) {
        LF_PROFILE_SCOPE("Leash/Tick");
        if (!std::isfinite(a_deltaTime) || a_deltaTime <= 0.0F) {
            return;
        }

        auto leashed = _leashed.get();
        auto holder = _holder.get();
        auto* attachmentActor = _definition.meshOwner == LeashMeshOwner::kHolder ? leashed.get() : holder.get();
        const auto invalidate = [&] {
            ReleaseControl();
            ResetSimulation();
            _exceeded = false;
        };
        const auto holderOwnsMesh = _definition.meshOwner == LeashMeshOwner::kHolder;
        auto* meshOwner = holderOwnsMesh ? holder.get() : leashed.get();
        if (!leashed || !meshOwner || !Bind(*meshOwner)) {
            invalidate();
            return;
        }

        const auto anchorBindResult = _anchor.Bind(attachmentActor, holder.get());
        if (anchorBindResult == LeashAnchor::BindResult::kFailed) {
            invalidate();
            return;
        }
        // The anchor bound successfully, but its runtime scene binding is new or different from the cached binding (Like eequipment changed, etc)
        if (anchorBindResult == LeashAnchor::BindResult::kChanged) {
            ResetSimulation();
        }
        _anchor.ApplyPose();
        const auto anchor = _anchor.GetSample(attachmentActor);
        if (!anchor) {
            invalidate();
            return;
        }
        const auto pullGoal = holder ? holder->GetPosition() : anchor->position;
        auto* pullGoalCell = holder ? holder->GetParentCell() : anchor->cell;
        if (!CanSimulateTogether(pullGoalCell, *leashed)) {
            invalidate();
            return;
        }

        const auto newlyBound = _neutralPositions.empty();
        if (newlyBound && _pullController.DiagnosticsEnabled()) {
            Movement::LogDirectLocomotionState(*leashed, "leash tick after bind");
        }
        ReadNeutralPose();
        const auto& collarAnchor = holderOwnsMesh ? anchor->position : _neutralPositions.front();
        const auto& leasherAnchor = holderOwnsMesh ? _neutralPositions.front() : anchor->position;
        const auto anchorDistance = collarAnchor.GetDistance(leasherAnchor);
        if (anchorDistance > _definition.maxLength) {
            if (!_exceeded) {
                SKSE::log::info("Exceeded by {}", anchorDistance - _definition.maxLength);
                _exceeded = true;
            }
        } else {
            _exceeded = false;
        }
        if (!a_allowForcedRecovery) {
            _recoveryController.Release(_recoveryState);
        }
        const auto forcedRecoveryActive = a_allowForcedRecovery && _recoveryController.Update(_recoveryState, *leashed, collarAnchor, leasherAnchor, pullGoal, _definition.maxLength, a_deltaTime);
        if (forcedRecoveryActive) {
            _pullController.Release(_pullState, leashed.get());
        } else {
            _pullController.Update(_pullState, *leashed, collarAnchor, pullGoal, pullGoalCell, _definition.minLength, _definition.maxLength, a_deltaTime);
        }

        _pullPoseController.Prepare(_pullPoseState, *leashed, collarAnchor, a_deltaTime, !forcedRecoveryActive);
        auto posedNeutralPositions = _neutralPositions;
        auto posedNeutralRotations = _neutralRotations;
        for (std::size_t index = 0; index < _bones.size(); ++index) {
            _pullPoseController.Transform(_pullPoseState, *_bones[index], posedNeutralPositions[index], posedNeutralRotations[index]);
        }
        auto posedEndAnchor = anchor->position;
        if (holderOwnsMesh && anchor->poseReference) {
            auto posedEndRotation = anchor->poseReference->world.rotate;
            _pullPoseController.Transform(_pullPoseState, *anchor->poseReference, posedEndAnchor, posedEndRotation);
        }
        const auto& posedCollarAnchor = holderOwnsMesh ? posedEndAnchor : posedNeutralPositions.front();
        const auto& posedLeasherAnchor = holderOwnsMesh ? posedNeutralPositions.front() : posedEndAnchor;

        RE::bhkWorld* world{};
        if (auto* cell = leashed->GetParentCell()) {
            world = cell->GetbhkWorld();
        }

        const auto& positions = _solver.Solve(posedNeutralPositions, _segmentLengths, posedEndAnchor, a_deltaTime, world, a_actorCollision, a_settings);
        if (!forcedRecoveryActive && positions.size() >= 2) {
            const auto& collar = holderOwnsMesh ? positions.back() : positions.front();
            const auto& nextRopePoint = holderOwnsMesh ? positions[positions.size() - 2] : positions[1];
            _pullPoseController.Capture(_pullPoseState, collar, nextRopePoint, posedCollarAnchor.GetDistance(posedLeasherAnchor), _definition.minLength, _definition.maxLength);
        }
        if (positions.size() == _bones.size()) {
            ApplyPose(posedNeutralPositions, posedNeutralRotations);
        }
        if (newlyBound && _pullController.DiagnosticsEnabled()) {
            Movement::LogDirectLocomotionState(*leashed, "leash tick complete");
        }
    }

    void LeashInstance::FreezeSimulation() {
        _solver.Freeze();
        _pullPoseController.Freeze(_pullPoseState);
    }

    void LeashInstance::ResetSimulation() {
        _solver.Reset();
        _pullPoseController.Reset(_pullPoseState);
        _deferredTranslations.clear();
        _deferredRotations.clear();
    }

    void LeashInstance::ApplyDeferredPose() {
        LF_PROFILE_SCOPE("Leash/ApplyDeferredPose");
        auto leashed = _leashed.get();
        auto holder = _holder.get();
        auto* attachmentActor = _definition.meshOwner == LeashMeshOwner::kHolder ? leashed.get() : holder.get();
        auto* meshOwner = _definition.meshOwner == LeashMeshOwner::kHolder ? holder.get() : leashed.get();
        if (!leashed || !meshOwner || !Bind(*meshOwner)) {
            return;
        }

        const auto anchorBindResult = _anchor.Bind(attachmentActor, holder.get());
        if (anchorBindResult == LeashAnchor::BindResult::kFailed) {
            return;
        }
        _anchor.ApplyPose();
        const auto anchor = _anchor.GetSample(attachmentActor);
        if (!anchor) {
            return;
        }
        auto* pullGoalCell = holder ? holder->GetParentCell() : anchor->cell;
        if (!CanSimulateTogether(pullGoalCell, *leashed)) {
            return;
        }
        if (anchorBindResult == LeashAnchor::BindResult::kChanged) {
            ResetSimulation();
            return;
        }

        _pullPoseController.Apply(_pullPoseState, *leashed);
        if (_deferredTranslations.empty() || _deferredTranslations.size() != _bones.size() || _deferredRotations.size() != _bones.size()) {
            return;
        }

        for (std::size_t index = 0; index < _bones.size(); ++index) {
            _bones[index]->world.translate = _deferredTranslations[index];
            _bones[index]->world.rotate = _deferredRotations[index];
        }
    }

    bool LeashInstance::Bind(RE::Actor& a_meshOwner) {
        auto* meshRoot = a_meshOwner.Get3D(false);
        if (!meshRoot) {
            ResetBinding();
            if (!_bindingWarningLogged) {
                SKSE::log::warn("Unable to bind leash for {:08X}: mesh owner {:08X} has no third-person 3D", _definition.leashedFormID, a_meshOwner.GetFormID());
                _bindingWarningLogged = true;
            }
            return false;
        }

        // Equipment changes can detach cached nodes without replacing the actor root.
        const auto leashBonesAttached = _bones.size() >= 2 && std::ranges::all_of(_bones, [&](const auto& a_bone) { return IsDescendantOf(a_bone.get(), meshRoot); });
        if (_boundMeshRoot.get() == meshRoot && leashBonesAttached) {
            return true;
        }

        ResetBinding();
        _boundMeshRoot.reset(meshRoot);

        auto* parent = meshRoot->GetObjectByName(RE::BSFixedString(_definition.parentBone));
        if (!parent) {
            ResetBinding();
            if (!_bindingWarningLogged) {
                SKSE::log::warn("Unable to bind leash {:08X}->{:08X}: parent bone '{}' was not found", _definition.holderFormID, _definition.leashedFormID, _definition.parentBone);
                _bindingWarningLogged = true;
            }
            return false;
        }

        auto* parentNode = parent->AsNode();
        if (!parentNode) {
            ResetBinding();
            if (!_bindingWarningLogged) {
                SKSE::log::warn("Unable to bind leash {:08X}->{:08X}: parent bone '{}' is not a node", _definition.holderFormID, _definition.leashedFormID, _definition.parentBone);
                _bindingWarningLogged = true;
            }
            return false;
        }

        for (const auto& child : parentNode->GetChildren()) {
            if (child) {
                CollectLeashBones(child.get(), _definition.leashBoneMatch, _bones);
            }
        }

        if (_bones.size() < 2) {
            const auto matchedBones = _bones.size();
            ResetBinding();
            if (!_bindingWarningLogged) {
                SKSE::log::warn("Unable to bind leash {:08X}->{:08X}: found {} child bone(s) containing '{}' under '{}'", _definition.holderFormID, _definition.leashedFormID, matchedBones, _definition.leashBoneMatch,
                    _definition.parentBone);
                _bindingWarningLogged = true;
            }
            return false;
        }

        _bindingWarningLogged = false;
        SKSE::log::info("Bound {} leash bones containing '{}' under '{}' for {:08X}->{:08X}", _bones.size(), _definition.leashBoneMatch, _definition.parentBone, _definition.holderFormID, _definition.leashedFormID);
        return true;
    }

    void LeashInstance::ResetBinding() {
        _bones.clear();
        _boundMeshRoot.reset();
        _neutralPositions.clear();
        _neutralRotations.clear();
        _segmentLengths.clear();
        _deferredTranslations.clear();
        _deferredRotations.clear();
        _solver.Reset();
        _pullPoseController.Reset(_pullPoseState);
    }

    void LeashInstance::ReadNeutralPose() {
        _neutralPositions.resize(_bones.size());
        _neutralRotations.resize(_bones.size());
        _segmentLengths.resize(_bones.size() - 1);

        for (std::size_t index = 0; index < _bones.size(); ++index) {
            _neutralPositions[index] = _bones[index]->world.translate;
            _neutralRotations[index] = _bones[index]->world.rotate;
            if (index > 0) {
                _segmentLengths[index - 1] = _neutralPositions[index - 1].GetDistance(_neutralPositions[index]);
            }
        }
    }

    void LeashInstance::ApplyPose(std::span<const RE::NiPoint3> a_neutralPositions, std::span<const RE::NiMatrix3> a_neutralRotations) {
        const auto& positions = _solver.GetPositions();
        _deferredTranslations.resize(_bones.size());
        _deferredRotations.resize(_bones.size());
        for (std::size_t index = 0; index < _bones.size(); ++index) {
            RE::NiPoint3 neutralDirection;
            RE::NiPoint3 solvedDirection;
            if (index + 1 < _bones.size()) {
                neutralDirection = a_neutralPositions[index + 1] - a_neutralPositions[index];
                solvedDirection = positions[index + 1] - positions[index];
            } else {
                neutralDirection = a_neutralPositions[index] - a_neutralPositions[index - 1];
                solvedDirection = positions[index] - positions[index - 1];
            }

            _bones[index]->world.translate = positions[index];
            _bones[index]->world.rotate = AlignRotation(a_neutralRotations[index], neutralDirection, solvedDirection);
            _deferredTranslations[index] = _bones[index]->world.translate;
            _deferredRotations[index] = _bones[index]->world.rotate;
        }
    }
}  // namespace LeashFramework
