#include "RopeSolver.h"

#include <algorithm>
#include <cmath>

#include "../PCH.h"
#include "WorldCollision.h"

namespace LeashFramework::Physics {
    namespace {
        constexpr float kMaximumDeltaTime = 0.05F;
        constexpr float kMaximumSubstepTime = 1.0F / 60.0F;
        constexpr float kReferenceDampingTime = 1.0F / 60.0F;
        constexpr float kMinimumSegmentLength = 0.001F;
        constexpr float kSnagTriggerTime = 0.15F;
        constexpr float kMinimumCollisionReleaseTime = 0.2F;
        constexpr float kContactReleaseDistance = 0.15F;
        constexpr float kMatchingContactNormalDot = 0.98F;
        constexpr std::uint8_t kMaximumMissedContactQueries = 1;
        constexpr std::size_t kContactProjectionIterations = 2;
    }  // namespace

    void RopeSolver::Reset() {
        _positions.clear();
        _previousPositions.clear();
        _substepStart.clear();
        _constraintMultipliers.clear();
        _contactConstraints.clear();
        _contactConstraintCounts.clear();
        _contactBlockedDistances.clear();
        _blockedContactTimes.clear();
        _collisionReleaseTimes.clear();
        _collisionReleased.clear();
    }

    void RopeSolver::Freeze() {
        _previousPositions = _positions;
        _substepStart = _positions;
    }

    const std::vector<RE::NiPoint3>& RopeSolver::GetPositions() const { return _positions; }

    const std::vector<RE::NiPoint3>& RopeSolver::Solve(std::span<const RE::NiPoint3> a_neutralPositions, std::span<const float> a_segmentLengths, const RE::NiPoint3& a_endAnchor, float a_deltaTime, RE::bhkWorld* a_world,
        const ActorBodyCollision* a_actorCollision, const SimulationSettings& a_settings) {
        LF_PROFILE_SCOPE("Rope/Solve");
        if (a_neutralPositions.size() < 2 || a_segmentLengths.size() + 1 != a_neutralPositions.size()) {
            Reset();
            return _positions;
        }

        if (_positions.size() != a_neutralPositions.size()) {
            _positions.assign(a_neutralPositions.begin(), a_neutralPositions.end());
            _previousPositions = _positions;
            _substepStart = _positions;
            _constraintMultipliers.assign(a_segmentLengths.size(), 0.0F);
            _contactConstraints.resize(_positions.size());
            _contactConstraintCounts.assign(_positions.size(), 0);
            _contactBlockedDistances.assign(_positions.size(), 0.0F);
            _blockedContactTimes.assign(_positions.size(), 0.0F);
            _collisionReleaseTimes.assign(_positions.size(), 0.0F);
            _collisionReleased.assign(_positions.size(), false);
        }

        const auto frameTime = std::clamp(a_deltaTime, 0.0F, kMaximumDeltaTime);
        if (frameTime <= 0.0F) {
            return _positions;
        }
        const auto substepCount = (std::max)(1U, static_cast<std::uint32_t>(std::ceil(frameTime / kMaximumSubstepTime)));
        const auto substepTime = substepCount > 0 ? frameTime / static_cast<float>(substepCount) : 0.0F;
        const auto startAnchor = a_neutralPositions.front();
        const auto substepDamping = std::pow(a_settings.damping, substepTime / kReferenceDampingTime);
        const auto complianceScale = a_settings.stretchCompliance / (substepTime * substepTime);

        for (std::uint32_t step = 0; step < substepCount; ++step) {
            _positions.front() = startAnchor;
            _positions.back() = a_endAnchor;
            _previousPositions.front() = startAnchor;
            _previousPositions.back() = a_endAnchor;
            _substepStart = _positions;
            std::fill(_contactBlockedDistances.begin(), _contactBlockedDistances.end(), 0.0F);

            for (std::size_t index = 1; index + 1 < _positions.size(); ++index) {
                if (_collisionReleased[index]) {
                    _collisionReleaseTimes[index] += substepTime;
                }
                const auto current = _positions[index];
                const auto velocity = (current - _previousPositions[index]) * substepDamping;
                _previousPositions[index] = current;
                _positions[index] = current + velocity + a_settings.gravity * (substepTime * substepTime);
            }

            ResolveCollisions(a_world, a_actorCollision, a_settings.collisionPadding, a_segmentLengths, 0.0F, a_settings.snagReleaseStrain, a_settings.snagBlockedDistance);
            std::fill(_constraintMultipliers.begin(), _constraintMultipliers.end(), 0.0F);
            for (std::uint32_t iteration = 0; iteration < a_settings.constraintIterations; ++iteration) {
                ApplyConstraints(a_segmentLengths, startAnchor, a_endAnchor, complianceScale, iteration % 2 != 0);
                if (iteration + 1 < a_settings.constraintIterations) {
                    for (std::size_t index = 1; index + 1 < _positions.size(); ++index) {
                        if (!_collisionReleased[index]) {
                            ApplyContactConstraints(index);
                        }
                    }
                }
            }
            ResolveCollisions(a_world, a_actorCollision, a_settings.collisionPadding, a_segmentLengths, substepTime, a_settings.snagReleaseStrain, a_settings.snagBlockedDistance);
        }

        return _positions;
    }

    void RopeSolver::ApplyConstraints(std::span<const float> a_segmentLengths, const RE::NiPoint3& a_startAnchor, const RE::NiPoint3& a_endAnchor, float a_complianceScale, bool a_reverse) {
        _positions.front() = a_startAnchor;
        _positions.back() = a_endAnchor;

        for (std::size_t offset = 0; offset < a_segmentLengths.size(); ++offset) {
            const auto index = a_reverse ? a_segmentLengths.size() - 1 - offset : offset;
            const auto delta = _positions[index + 1] - _positions[index];
            const auto distance = delta.Length();
            if (distance <= kMinimumSegmentLength) {
                continue;
            }

            const auto firstWeight = index == 0 ? 0.0F : 1.0F;
            const auto secondWeight = index + 1 == _positions.size() - 1 ? 0.0F : 1.0F;
            const auto denominator = firstWeight + secondWeight + a_complianceScale;
            if (denominator <= 0.0F) {
                continue;
            }

            const auto constraint = distance - (std::max)(a_segmentLengths[index], kMinimumSegmentLength);
            const auto multiplierDelta = (-constraint - a_complianceScale * _constraintMultipliers[index]) / denominator;
            _constraintMultipliers[index] += multiplierDelta;
            const auto correction = delta * (multiplierDelta / distance);
            _positions[index] -= correction * firstWeight;
            _positions[index + 1] += correction * secondWeight;
        }

        _positions.front() = a_startAnchor;
        _positions.back() = a_endAnchor;
    }

    void RopeSolver::ApplyContactConstraints(std::size_t a_index) {
        for (std::size_t iteration = 0; iteration < kContactProjectionIterations; ++iteration) {
            for (std::size_t contactIndex = 0; contactIndex < _contactConstraintCounts[a_index]; ++contactIndex) {
                const auto& contact = _contactConstraints[a_index][contactIndex];
                const auto separation = (_positions[a_index] - contact.planePoint).Dot(contact.normal);
                if (separation >= 0.0F) {
                    continue;
                }

                const auto correction = contact.normal * -separation;
                _positions[a_index] += correction;
                // Move the old position too so the correction doesn't become fake velocity
                _previousPositions[a_index] += correction;
                _contactBlockedDistances[a_index] = (std::max)(_contactBlockedDistances[a_index], -separation);
            }
        }

        auto velocity = _positions[a_index] - _previousPositions[a_index];
        for (std::size_t iteration = 0; iteration < kContactProjectionIterations; ++iteration) {
            for (std::size_t contactIndex = 0; contactIndex < _contactConstraintCounts[a_index]; ++contactIndex) {
                const auto inwardVelocity = velocity.Dot(_contactConstraints[a_index][contactIndex].normal);
                if (inwardVelocity < 0.0F) {
                    velocity -= _contactConstraints[a_index][contactIndex].normal * inwardVelocity;
                }
            }
        }
        _previousPositions[a_index] = _positions[a_index] - velocity;
    }

    void RopeSolver::ResolveCollisions(RE::bhkWorld* a_world, const ActorBodyCollision* a_actorCollision, float a_radius, std::span<const float> a_segmentLengths, float a_snagDeltaTime, float a_snagReleaseStrain,
        float a_snagBlockedDistance) {
        LF_PROFILE_SCOPE("Rope/ResolveCollisions");
        if (!a_world) {
            std::fill(_contactConstraintCounts.begin(), _contactConstraintCounts.end(), 0);
            std::fill(_contactBlockedDistances.begin(), _contactBlockedDistances.end(), 0.0F);
            std::fill(_blockedContactTimes.begin(), _blockedContactTimes.end(), 0.0F);
            std::fill(_collisionReleaseTimes.begin(), _collisionReleaseTimes.end(), 0.0F);
            std::fill(_collisionReleased.begin(), _collisionReleased.end(), false);
            return;
        }

        for (std::size_t index = 1; index + 1 < _positions.size(); ++index) {
            if (_collisionReleased[index]) {
                _contactConstraintCounts[index] = 0;
                _substepStart[index] = _positions[index];
                if (a_snagDeltaTime > 0.0F && _collisionReleaseTimes[index] >= kMinimumCollisionReleaseTime) {
                    const auto overlap = WorldCollision::ResolveMovement(a_world, a_actorCollision, _positions[index], _positions[index], a_radius);
                    if (!overlap.collided) {
                        _blockedContactTimes[index] = 0.0F;
                        _collisionReleaseTimes[index] = 0.0F;
                        _collisionReleased[index] = false;
                        _previousPositions[index] = _positions[index];
                    }
                }
                continue;
            }

            const auto targetPosition = _positions[index];
            const auto result = WorldCollision::ResolveMovement(a_world, a_actorCollision, _substepStart[index], targetPosition, a_radius);
            _previousPositions[index] += result.position - targetPosition;
            _positions[index] = result.position;

            std::array<bool, kMaximumContactConstraints> matchedContacts{};
            std::array<ContactConstraint, kMaximumContactConstraints> updatedContacts{};
            std::size_t updatedContactCount{};
            for (std::size_t resultIndex = 0; resultIndex < result.contactCount && updatedContactCount < updatedContacts.size(); ++resultIndex) {
                const auto& resultContact = result.contacts[resultIndex];
                std::size_t matchingIndex = _contactConstraintCounts[index];
                float matchingDot = kMatchingContactNormalDot;
                for (std::size_t contactIndex = 0; contactIndex < _contactConstraintCounts[index]; ++contactIndex) {
                    const auto normalDot = _contactConstraints[index][contactIndex].normal.Dot(resultContact.normal);
                    if (!matchedContacts[contactIndex] && _contactConstraints[index][contactIndex].movingSurface == resultContact.movingSurface && normalDot >= matchingDot) {
                        matchingIndex = contactIndex;
                        matchingDot = normalDot;
                    }
                }
                if (matchingIndex < _contactConstraintCounts[index]) {
                    matchedContacts[matchingIndex] = true;
                }
                if (matchingIndex < _contactConstraintCounts[index] && !resultContact.movingSurface) {
                    // Keep the old static plane when Havok only gives us a small change
                    auto contact = _contactConstraints[index][matchingIndex];
                    const auto planeShift = (resultContact.planePoint - contact.planePoint).Dot(contact.normal);
                    if (std::abs(planeShift) <= kContactReleaseDistance) {
                        contact.missedQueries = 0;
                        updatedContacts[updatedContactCount++] = contact;
                    } else {
                        updatedContacts[updatedContactCount++] = {.planePoint = resultContact.planePoint, .normal = resultContact.normal, .movingSurface = resultContact.movingSurface};
                    }
                } else {
                    updatedContacts[updatedContactCount++] = {.planePoint = resultContact.planePoint, .normal = resultContact.normal, .movingSurface = resultContact.movingSurface};
                }
                _contactBlockedDistances[index] = (std::max)(_contactBlockedDistances[index], (result.position - targetPosition).Dot(resultContact.normal));
            }

            for (std::size_t contactIndex = 0; contactIndex < _contactConstraintCounts[index] && updatedContactCount < updatedContacts.size(); ++contactIndex) {
                auto contact = _contactConstraints[index][contactIndex];
                const auto separation = (result.position - contact.planePoint).Dot(contact.normal);
                if (!matchedContacts[contactIndex] && contact.missedQueries < kMaximumMissedContactQueries && separation <= kContactReleaseDistance) {
                    ++contact.missedQueries;
                    updatedContacts[updatedContactCount++] = contact;
                }
            }
            _contactConstraints[index] = updatedContacts;
            _contactConstraintCounts[index] = updatedContactCount;
            ApplyContactConstraints(index);
            _substepStart[index] = _positions[index];

            if (a_snagDeltaTime > 0.0F) {
                const auto previousLength = _positions[index - 1].GetDistance(_positions[index]);
                const auto nextLength = _positions[index].GetDistance(_positions[index + 1]);
                const auto previousStretched = previousLength > (std::max)(a_segmentLengths[index - 1], kMinimumSegmentLength) * (1.0F + a_snagReleaseStrain);
                const auto nextStretched = nextLength > (std::max)(a_segmentLengths[index], kMinimumSegmentLength) * (1.0F + a_snagReleaseStrain);
                if ((previousStretched || nextStretched) && _contactBlockedDistances[index] > a_snagBlockedDistance) {
                    _blockedContactTimes[index] += a_snagDeltaTime;
                    if (_blockedContactTimes[index] >= kSnagTriggerTime) {
                        _blockedContactTimes[index] = 0.0F;
                        _collisionReleaseTimes[index] = 0.0F;
                        _collisionReleased[index] = true;
                        _contactConstraintCounts[index] = 0;
                        _previousPositions[index] = _positions[index];
                    }
                } else {
                    _blockedContactTimes[index] = 0.0F;
                }
            }
        }
    }
}  // namespace LeashFramework::Physics
