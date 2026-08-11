#include "ActorBodyCollision.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#include "../UI/DebugOverlay.h"

namespace LeashFramework::Physics {
    namespace {
        constexpr std::array<std::string_view, 4> kBoneNames{"NPC Spine [Spn0]", "NPC Spine1 [Spn1]", "NPC Spine2 [Spn2]", "NPC Neck [Neck]"};
        constexpr std::string_view kLeftBreastBone = "L Breast01";
        constexpr std::string_view kRightBreastBone = "R Breast01";
        constexpr float kMinimumBoneSpan = 0.01F;
        constexpr float kMinimumQueryRadius = 0.01F;
        constexpr float kMinimumShapeRadius = 0.5F;
        constexpr float kMinimumMovementSquared = 0.0001F;
        constexpr float kDirectionEpsilon = 0.0001F;
        constexpr float kPrimitiveSwitchDistance = 0.25F;
        constexpr float kHalfPi = 1.57079632679F;
        constexpr std::size_t kDebugRingSteps = 16;
        constexpr std::size_t kDebugCapSteps = 8;
        constexpr std::uint32_t kDebugBoundsColor = 0xDC00FFFF;

        [[nodiscard]] bool IsFinite(const RE::NiPoint3& a_point) { return std::isfinite(a_point.x) && std::isfinite(a_point.y) && std::isfinite(a_point.z); }

        [[nodiscard]] RE::NiPoint3 Minimum(const RE::NiPoint3& a_left, const RE::NiPoint3& a_right) { return {(std::min)(a_left.x, a_right.x), (std::min)(a_left.y, a_right.y), (std::min)(a_left.z, a_right.z)}; }

        [[nodiscard]] RE::NiPoint3 Maximum(const RE::NiPoint3& a_left, const RE::NiPoint3& a_right) { return {(std::max)(a_left.x, a_right.x), (std::max)(a_left.y, a_right.y), (std::max)(a_left.z, a_right.z)}; }

        [[nodiscard]] RE::NiPoint3 Interpolate(const RE::NiPoint3& a_previous, const RE::NiPoint3& a_current, float a_fraction) { return a_previous + (a_current - a_previous) * a_fraction; }

        [[nodiscard]] bool IsPreferred(std::span<const ActorBodyCollision::ShapeKey> a_preferredShapes, const ActorBodyCollision::ShapeKey& a_shape) {
            return std::ranges::find(a_preferredShapes, a_shape) != a_preferredShapes.end();
        }

        [[nodiscard]] bool GetDebugAxes(const RE::NiPoint3& a_direction, RE::NiPoint3& a_right, RE::NiPoint3& a_up) {
            auto forward = a_direction;
            if (forward.Unitize() <= kDirectionEpsilon) {
                return false;
            }
            const auto temporaryUp = std::abs(forward.z) < 0.95F ? RE::NiPoint3{0.0F, 0.0F, 1.0F} : RE::NiPoint3{1.0F, 0.0F, 0.0F};
            a_right = temporaryUp.Cross(forward);
            if (a_right.Unitize() <= kDirectionEpsilon) {
                return false;
            }
            a_up = forward.Cross(a_right);
            return a_up.Unitize() > kDirectionEpsilon;
        }

        void AddDebugLine(std::vector<UI::DebugOverlay::WorldLine>& a_lines, const RE::NiPoint3& a_start, const RE::NiPoint3& a_end) { a_lines.push_back({.start = a_start, .end = a_end}); }

        void AddDebugRing(std::vector<UI::DebugOverlay::WorldLine>& a_lines, const RE::NiPoint3& a_center, const RE::NiPoint3& a_right, const RE::NiPoint3& a_up, float a_radius) {
            for (std::size_t step = 0; step < kDebugRingSteps; ++step) {
                const auto angle = static_cast<float>(step) * 4.0F * kHalfPi / static_cast<float>(kDebugRingSteps);
                const auto nextAngle = static_cast<float>(step + 1) * 4.0F * kHalfPi / static_cast<float>(kDebugRingSteps);
                AddDebugLine(a_lines, a_center + a_right * (std::cos(angle) * a_radius) + a_up * (std::sin(angle) * a_radius),
                    a_center + a_right * (std::cos(nextAngle) * a_radius) + a_up * (std::sin(nextAngle) * a_radius));
            }
        }

        void AddDebugSphere(std::vector<UI::DebugOverlay::WorldLine>& a_lines, const RE::NiPoint3& a_center, float a_radius) {
            AddDebugRing(a_lines, a_center, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, a_radius);
            AddDebugRing(a_lines, a_center, {1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, a_radius);
            AddDebugRing(a_lines, a_center, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, a_radius);
        }

        void AddDebugBounds(std::vector<UI::DebugOverlay::WorldLine>& a_lines, const RE::NiPoint3& a_minimum, const RE::NiPoint3& a_maximum) {
            const std::array corners{RE::NiPoint3{a_minimum.x, a_minimum.y, a_minimum.z}, RE::NiPoint3{a_maximum.x, a_minimum.y, a_minimum.z}, RE::NiPoint3{a_maximum.x, a_maximum.y, a_minimum.z},
                RE::NiPoint3{a_minimum.x, a_maximum.y, a_minimum.z}, RE::NiPoint3{a_minimum.x, a_minimum.y, a_maximum.z}, RE::NiPoint3{a_maximum.x, a_minimum.y, a_maximum.z},
                RE::NiPoint3{a_maximum.x, a_maximum.y, a_maximum.z}, RE::NiPoint3{a_minimum.x, a_maximum.y, a_maximum.z}};
            constexpr std::array<std::pair<std::size_t, std::size_t>, 12> edges{{{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}}};
            for (const auto& [start, end] : edges) {
                a_lines.push_back({.start = corners[start], .end = corners[end], .color = kDebugBoundsColor, .thickness = 1.0F});
            }
        }

        void AddDebugCapsule(std::vector<UI::DebugOverlay::WorldLine>& a_lines, const RE::NiPoint3& a_start, const RE::NiPoint3& a_end, float a_radius) {
            auto forward = a_end - a_start;
            RE::NiPoint3 right;
            RE::NiPoint3 up;
            if (!GetDebugAxes(forward, right, up)) {
                AddDebugSphere(a_lines, a_start, a_radius);
                return;
            }
            forward.Unitize();
            AddDebugRing(a_lines, a_start, right, up, a_radius);
            AddDebugRing(a_lines, a_end, right, up, a_radius);

            const std::array offsets{right * a_radius, right * -a_radius, up * a_radius, up * -a_radius};
            for (const auto& offset : offsets) {
                AddDebugLine(a_lines, a_start + offset, a_end + offset);
            }

            const std::array radialDirections{right, -right, up, -up};
            for (const auto& radial : radialDirections) {
                auto previousStart = a_start + radial * a_radius;
                auto previousEnd = a_end + radial * a_radius;
                for (std::size_t step = 1; step <= kDebugCapSteps; ++step) {
                    const auto angle = static_cast<float>(step) * kHalfPi / static_cast<float>(kDebugCapSteps);
                    const auto radialOffset = radial * (std::cos(angle) * a_radius);
                    const auto axialOffset = forward * (std::sin(angle) * a_radius);
                    const auto currentStart = a_start + radialOffset - axialOffset;
                    const auto currentEnd = a_end + radialOffset + axialOffset;
                    AddDebugLine(a_lines, previousStart, currentStart);
                    AddDebugLine(a_lines, previousEnd, currentEnd);
                    previousStart = currentStart;
                    previousEnd = currentEnd;
                }
            }
        }
    }  // namespace

    void ActorBodyCollision::Update(const ActorBodyCollisionSettings& a_settings) {
        LF_PROFILE_SCOPE("Collision/ActorBodies/Update");
        _previousBodies.clear();
        _previousBodies.reserve(_actors.size());
        for (const auto& actor : _actors) {
            if (actor.bodyInitialized) {
                _previousBodies.push_back({.formID = actor.formID, .world = actor.world, .body = _bodies[actor.bodyIndex]});
            }
        }
        _actors.clear();
        _bodies.clear();
        _settings = a_settings;

        UpdateActor(RE::PlayerCharacter::GetSingleton());
        if (auto* processLists = RE::ProcessLists::GetSingleton()) {
            processLists->ForEachHighActor([&](RE::Actor* a_actor) {
                UpdateActor(a_actor);
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }
    }

    void ActorBodyCollision::DrawDebug() const {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* playerWorld = player && player->parentCell ? player->parentCell->GetbhkWorld() : nullptr;
        if (!playerWorld) {
            return;
        }
        std::vector<UI::DebugOverlay::WorldLine> lines;
        for (const auto& actor : _actors) {
            if (actor.world != playerWorld) {
                continue;
            }
            if (actor.hasWorldBound) {
                AddDebugBounds(lines, actor.bounds.minimum, actor.bounds.maximum);
            }
            if (!actor.bodyInitialized) {
                continue;
            }
            const auto& body = _bodies[actor.bodyIndex];
            for (std::size_t index = 0; index < body.capsuleCount; ++index) {
                const auto& capsule = body.capsules[index];
                AddDebugCapsule(lines, capsule.start, capsule.end, capsule.radius);
            }
            for (std::size_t index = 0; index < body.sphereCount; ++index) {
                const auto& sphere = body.spheres[index];
                AddDebugSphere(lines, sphere.center, sphere.radius);
            }
        }
        if (lines.empty()) {
            return;
        }
        UI::DebugOverlay::Queue([lines = std::move(lines)] { UI::DebugOverlay::DrawWorldLines(lines); });
    }

    void ActorBodyCollision::Clear() {
        _actors.clear();
        _bodies.clear();
        _previousBodies.clear();
    }

    std::optional<ActorBodyCollision::Hit> ActorBodyCollision::FindDeepestOverlap(const RE::bhkWorld* a_world, const RE::NiPoint3& a_position, float a_radius, float a_interpolation,
        std::span<const ShapeKey> a_preferredShapes) const {
        LF_PROFILE_SCOPE("Collision/ActorBodies/Overlap");
        const auto interpolation = std::clamp(a_interpolation, 0.0F, 1.0F);
        const auto queryRadius = (std::max)(a_radius, kMinimumQueryRadius);
        const auto queryMinimum = a_position - RE::NiPoint3{queryRadius, queryRadius, queryRadius};
        const auto queryMaximum = a_position + RE::NiPoint3{queryRadius, queryRadius, queryRadius};
        std::optional<Hit> result;
        std::optional<Hit> preferredResult;

        const auto consider = [&](const RE::NiPoint3& a_start, const RE::NiPoint3& a_end, float a_shapeRadius, const ShapeKey& a_shape) {
            const auto segment = a_end - a_start;
            const auto segmentLengthSquared = segment.SqrLength();
            const auto segmentFraction = segmentLengthSquared > kMinimumBoneSpan * kMinimumBoneSpan ? std::clamp((a_position - a_start).Dot(segment) / segmentLengthSquared, 0.0F, 1.0F) : 0.0F;
            const auto closestPoint = a_start + segment * segmentFraction;
            auto normal = a_position - closestPoint;
            const auto distance = normal.Length();
            const auto combinedRadius = queryRadius + a_shapeRadius;
            if (distance >= combinedRadius) {
                return;
            }
            if (distance > kDirectionEpsilon) {
                normal /= distance;
            } else {
                normal = segment.Cross({0.0F, 0.0F, 1.0F});
                if (normal.Unitize() <= kDirectionEpsilon) {
                    normal = segment.Cross({1.0F, 0.0F, 0.0F});
                    if (normal.Unitize() <= kDirectionEpsilon) {
                        normal = {0.0F, 1.0F, 0.0F};
                    }
                }
            }

            const auto penetration = combinedRadius - distance;
            const Hit hit{.shape = a_shape, .normal = normal, .penetration = penetration};
            if (!result || penetration > result->penetration) {
                result = hit;
            }
            if (IsPreferred(a_preferredShapes, a_shape) && (!preferredResult || penetration > preferredResult->penetration)) {
                preferredResult = hit;
            }
        };

        for (const auto& actor : _actors) {
            if (actor.world != a_world || !Intersects(actor.bounds, queryMinimum, queryMaximum)) {
                continue;
            }
            const auto* body = GetBody(actor);
            if (!body) {
                continue;
            }
            const auto* previousBody = actor.hasPreviousBody ? std::addressof(_previousBodies[actor.previousBodyIndex].body) : nullptr;
            auto bodyMinimum = body->bounds.minimum;
            auto bodyMaximum = body->bounds.maximum;
            if (previousBody) {
                bodyMinimum = Minimum(bodyMinimum, previousBody->bounds.minimum);
                bodyMaximum = Maximum(bodyMaximum, previousBody->bounds.maximum);
            }
            if (!Intersects({.minimum = bodyMinimum, .maximum = bodyMaximum}, queryMinimum, queryMaximum)) {
                continue;
            }
            for (std::size_t index = 0; index < body->capsuleCount; ++index) {
                const auto& capsule = body->capsules[index];
                const auto* previous = previousBody && index < previousBody->capsuleCount ? std::addressof(previousBody->capsules[index]) : nullptr;
                const auto start = previous ? Interpolate(previous->start, capsule.start, interpolation) : capsule.start;
                const auto end = previous ? Interpolate(previous->end, capsule.end, interpolation) : capsule.end;
                const auto radius = previous ? std::lerp(previous->radius, capsule.radius, interpolation) : capsule.radius;
                consider(start, end, radius, {.actorFormID = actor.formID, .shapeIndex = static_cast<std::uint8_t>(index)});
            }
            for (std::size_t index = 0; index < body->sphereCount; ++index) {
                const auto& sphere = body->spheres[index];
                const auto* previous = previousBody && index < previousBody->sphereCount ? std::addressof(previousBody->spheres[index]) : nullptr;
                const auto center = previous ? Interpolate(previous->center, sphere.center, interpolation) : sphere.center;
                const auto radius = previous ? std::lerp(previous->radius, sphere.radius, interpolation) : sphere.radius;
                consider(center, center, radius, {.actorFormID = actor.formID, .shapeIndex = static_cast<std::uint8_t>(Body::kMaximumCapsules + index)});
            }
        }
        if (result && preferredResult && result->penetration - preferredResult->penetration <= kPrimitiveSwitchDistance) {
            return preferredResult;
        }
        return result;
    }

    std::optional<ActorBodyCollision::Hit> ActorBodyCollision::SweepSphere(const RE::bhkWorld* a_world, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, float a_radius, float a_interpolation,
        std::span<const ShapeKey> a_preferredShapes) const {
        LF_PROFILE_SCOPE("Collision/ActorBodies/Sweep");
        const auto movement = a_to - a_from;
        const auto movementLengthSquared = movement.SqrLength();
        if (movementLengthSquared < kMinimumMovementSquared) {
            return std::nullopt;
        }

        const auto queryRadius = (std::max)(a_radius, kMinimumQueryRadius);
        const auto radiusVector = RE::NiPoint3{queryRadius, queryRadius, queryRadius};
        const auto queryMinimum = Minimum(a_from, a_to) - radiusVector;
        const auto queryMaximum = Maximum(a_from, a_to) + radiusVector;
        std::optional<Hit> result;
        std::optional<Hit> preferredResult;
        const auto interpolation = std::clamp(a_interpolation, 0.0F, 1.0F);
        const auto movementLength = std::sqrt(movementLengthSquared);

        const auto consider = [&](const RE::NiPoint3& a_start, const RE::NiPoint3& a_end, float a_shapeRadius, const ShapeKey& a_shape) {
            const auto combinedRadius = queryRadius + a_shapeRadius;
            const auto combinedRadiusSquared = combinedRadius * combinedRadius;
            const auto recordHit = [&](const RE::NiPoint3& a_normal, float a_fraction) {
                const Hit hit{.shape = a_shape, .normal = a_normal, .fraction = a_fraction};
                if (!result || a_fraction < result->fraction) {
                    result = hit;
                }
                if (IsPreferred(a_preferredShapes, a_shape) && (!preferredResult || a_fraction < preferredResult->fraction)) {
                    preferredResult = hit;
                }
            };
            auto considerSphere = [&](const RE::NiPoint3& a_center) {
                const auto offset = a_from - a_center;
                const auto projection = offset.Dot(movement);
                const auto discriminant = projection * projection - movementLengthSquared * (offset.SqrLength() - combinedRadiusSquared);
                if (discriminant < 0.0F) {
                    return;
                }
                const auto fraction = (-projection - std::sqrt(discriminant)) / movementLengthSquared;
                if (fraction < 0.0F || fraction > 1.0F) {
                    return;
                }
                auto normal = a_from + movement * fraction - a_center;
                if (normal.Unitize() <= kDirectionEpsilon || normal.Dot(movement) >= 0.0F) {
                    return;
                }
                recordHit(normal, fraction);
            };

            const auto axis = a_end - a_start;
            const auto axisLengthSquared = axis.SqrLength();
            if (axisLengthSquared > kMinimumBoneSpan * kMinimumBoneSpan) {
                const auto origin = a_from - a_start;
                const auto axisMovement = axis.Dot(movement);
                const auto axisOrigin = axis.Dot(origin);
                const auto coefficientA = axisLengthSquared * movementLengthSquared - axisMovement * axisMovement;
                const auto coefficientB = axisLengthSquared * origin.Dot(movement) - axisOrigin * axisMovement;
                const auto coefficientC = axisLengthSquared * origin.SqrLength() - axisOrigin * axisOrigin - combinedRadiusSquared * axisLengthSquared;
                const auto discriminant = coefficientB * coefficientB - coefficientA * coefficientC;
                if (coefficientA > kDirectionEpsilon && discriminant >= 0.0F) {
                    const auto fraction = (-coefficientB - std::sqrt(discriminant)) / coefficientA;
                    const auto axisPosition = axisOrigin + fraction * axisMovement;
                    if (fraction >= 0.0F && fraction <= 1.0F && axisPosition > 0.0F && axisPosition < axisLengthSquared) {
                        auto normal = origin + movement * fraction - axis * (axisPosition / axisLengthSquared);
                        if (normal.Unitize() > kDirectionEpsilon && normal.Dot(movement) < 0.0F) {
                            recordHit(normal, fraction);
                        }
                    }
                }
            }
            considerSphere(a_start);
            if (axisLengthSquared > kMinimumBoneSpan * kMinimumBoneSpan) {
                considerSphere(a_end);
            }
        };

        for (const auto& actor : _actors) {
            if (actor.world != a_world || !Intersects(actor.bounds, queryMinimum, queryMaximum)) {
                continue;
            }
            const auto* body = GetBody(actor);
            if (!body) {
                continue;
            }
            const auto* previousBody = actor.hasPreviousBody ? std::addressof(_previousBodies[actor.previousBodyIndex].body) : nullptr;
            auto bodyMinimum = body->bounds.minimum;
            auto bodyMaximum = body->bounds.maximum;
            if (previousBody) {
                bodyMinimum = Minimum(bodyMinimum, previousBody->bounds.minimum);
                bodyMaximum = Maximum(bodyMaximum, previousBody->bounds.maximum);
            }
            if (!Intersects({.minimum = bodyMinimum, .maximum = bodyMaximum}, queryMinimum, queryMaximum)) {
                continue;
            }
            for (std::size_t index = 0; index < body->capsuleCount; ++index) {
                const auto& capsule = body->capsules[index];
                const auto* previous = previousBody && index < previousBody->capsuleCount ? std::addressof(previousBody->capsules[index]) : nullptr;
                const auto start = previous ? Interpolate(previous->start, capsule.start, interpolation) : capsule.start;
                const auto end = previous ? Interpolate(previous->end, capsule.end, interpolation) : capsule.end;
                const auto radius = previous ? std::lerp(previous->radius, capsule.radius, interpolation) : capsule.radius;
                consider(start, end, radius, {.actorFormID = actor.formID, .shapeIndex = static_cast<std::uint8_t>(index)});
            }
            for (std::size_t index = 0; index < body->sphereCount; ++index) {
                const auto& sphere = body->spheres[index];
                const auto* previous = previousBody && index < previousBody->sphereCount ? std::addressof(previousBody->spheres[index]) : nullptr;
                const auto center = previous ? Interpolate(previous->center, sphere.center, interpolation) : sphere.center;
                const auto radius = previous ? std::lerp(previous->radius, sphere.radius, interpolation) : sphere.radius;
                consider(center, center, radius, {.actorFormID = actor.formID, .shapeIndex = static_cast<std::uint8_t>(Body::kMaximumCapsules + index)});
            }
        }
        if (result && preferredResult && (preferredResult->fraction - result->fraction) * movementLength <= kPrimitiveSwitchDistance) {
            return preferredResult;
        }
        return result;
    }

    void ActorBodyCollision::UpdateActor(RE::Actor* a_actor) {
        if (!a_actor || a_actor->IsDisabled()) {
            return;
        }
        auto* root = a_actor->Get3D(false);
        auto* cell = a_actor->GetParentCell();
        auto* world = cell ? cell->GetbhkWorld() : nullptr;
        if (!root || !world) {
            return;
        }

        const auto formID = a_actor->GetFormID();
        if (std::ranges::find(_actors, formID, &ActorProxy::formID) != _actors.end()) {
            return;
        }

        Bounds bounds;
        const auto& worldBound = root->worldBound;
        const auto hasWorldBound = IsFinite(worldBound.center) && std::isfinite(worldBound.radius) && worldBound.radius >= 0.0F;
        if (hasWorldBound) {
            const auto radius = RE::NiPoint3{worldBound.radius, worldBound.radius, worldBound.radius};
            bounds = {.minimum = worldBound.center - radius, .maximum = worldBound.center + radius};
        } else {
            const auto maximum = (std::numeric_limits<float>::max)();
            bounds = {.minimum = {-maximum, -maximum, -maximum}, .maximum = {maximum, maximum, maximum}};
        }
        const auto previousBody = std::ranges::find_if(_previousBodies, [&](const PreviousBody& a_previous) { return a_previous.formID == formID && a_previous.world == world; });
        const auto hasPreviousBody = previousBody != _previousBodies.end();
        const auto previousBodyIndex = hasPreviousBody ? static_cast<std::size_t>(previousBody - _previousBodies.begin()) : 0;
        if (hasPreviousBody && hasWorldBound) {
            bounds.minimum = Minimum(bounds.minimum, previousBody->body.bounds.minimum);
            bounds.maximum = Maximum(bounds.maximum, previousBody->body.bounds.maximum);
        }
        _actors.push_back({.formID = formID,
            .actor = a_actor->GetHandle(),
            .world = world,
            .bounds = bounds,
            .hasWorldBound = hasWorldBound,
            .previousBodyIndex = previousBodyIndex,
            .hasPreviousBody = hasPreviousBody});
    }

    const ActorBodyCollision::Body* ActorBodyCollision::GetBody(const ActorProxy& a_proxy) const {
        if (a_proxy.bodyInitializationAttempted) {
            return a_proxy.bodyInitialized ? std::addressof(_bodies[a_proxy.bodyIndex]) : nullptr;
        }
        a_proxy.bodyInitializationAttempted = true;

        auto actor = a_proxy.actor.get();
        if (!actor || actor->IsDisabled()) {
            return nullptr;
        }
        auto* root = actor->Get3D(false);
        auto* cell = actor->GetParentCell();
        if (!root || !cell || cell->GetbhkWorld() != a_proxy.world) {
            return nullptr;
        }
        auto body = BuildBody(*actor, *root);
        if (!body) {
            return nullptr;
        }
        a_proxy.bodyIndex = _bodies.size();
        _bodies.push_back(std::move(*body));
        a_proxy.bodyInitialized = true;
        return std::addressof(_bodies.back());
    }

    std::optional<ActorBodyCollision::Body> ActorBodyCollision::BuildBody(RE::Actor& a_actor, RE::NiAVObject& a_root) const {
        std::array<RE::NiAVObject*, static_cast<std::size_t>(Bone::kTotal)> bones{};
        for (std::size_t index = 0; index < kBoneNames.size(); ++index) {
            bones[index] = a_root.GetObjectByName(RE::BSFixedString(kBoneNames[index]));
        }
        if (std::ranges::any_of(bones, [](const RE::NiAVObject* a_bone) { return !a_bone || !IsFinite(a_bone->world.translate); })) {
            return std::nullopt;
        }
        const auto female = a_actor.GetActorBase() && a_actor.GetActorBase()->GetSex() == RE::SEX::kFemale;
        RE::NiAVObject* leftBreast{};
        RE::NiAVObject* rightBreast{};
        if (female) {
            leftBreast = a_root.GetObjectByName(RE::BSFixedString(kLeftBreastBone));
            rightBreast = a_root.GetObjectByName(RE::BSFixedString(kRightBreastBone));
        }

        Body body;
        bool boundsInitialized{};
        const auto expandBounds = [&](const RE::NiPoint3& a_point, float a_radius) {
            const auto radius = RE::NiPoint3{a_radius, a_radius, a_radius};
            const auto minimum = a_point - radius;
            const auto maximum = a_point + radius;
            if (!boundsInitialized) {
                body.bounds = {.minimum = minimum, .maximum = maximum};
                boundsInitialized = true;
            } else {
                body.bounds.minimum = Minimum(body.bounds.minimum, minimum);
                body.bounds.maximum = Maximum(body.bounds.maximum, maximum);
            }
        };
        const auto addCapsule = [&](Bone a_bone, const ActorBodyCapsuleSettings& a_shape) {
            if (!IsFinite(a_shape.offset) || !std::isfinite(a_shape.radius) || !std::isfinite(a_shape.width)) {
                return;
            }
            const auto* bone = bones[static_cast<std::size_t>(a_bone)];
            const auto scale = std::abs(bone->world.scale);
            auto axis = bone->world.rotate * RE::NiPoint3{1.0F, 0.0F, 0.0F};
            if (!std::isfinite(scale) || axis.Unitize() <= kDirectionEpsilon) {
                return;
            }
            const auto center = bone->world * a_shape.offset;
            const auto halfWidth = (std::max)(a_shape.width, 0.0F) * scale * 0.5F;
            const auto radius = (std::max)(a_shape.radius * scale, kMinimumShapeRadius);
            const auto start = center - axis * halfWidth;
            const auto end = center + axis * halfWidth;
            if (!IsFinite(start) || !IsFinite(end)) {
                return;
            }
            body.capsules[body.capsuleCount++] = {.start = start, .end = end, .radius = radius};
            expandBounds(start, radius);
            expandBounds(end, radius);
        };
        const auto addSphere = [&](const RE::NiAVObject* a_bone, const ActorBodyBreastSettings& a_shape) {
            if (!a_bone || !IsFinite(a_bone->world.translate) || !IsFinite(a_shape.offset) || !std::isfinite(a_shape.radius)) {
                return;
            }
            const auto scale = std::abs(a_bone->world.scale);
            const auto center = a_bone->world * a_shape.offset;
            const auto radius = (std::max)(a_shape.radius * scale, kMinimumShapeRadius);
            if (!std::isfinite(scale) || !IsFinite(center)) {
                return;
            }
            body.spheres[body.sphereCount++] = {.center = center, .radius = radius};
            expandBounds(center, radius);
        };

        const auto& sexSettings = female ? _settings.female : _settings.male;
        addCapsule(Bone::kSpine, sexSettings.spine);
        addCapsule(Bone::kSpine1, sexSettings.spine1);
        addCapsule(Bone::kSpine2, sexSettings.spine2);
        addCapsule(Bone::kNeck, sexSettings.neck);
        if (female) {
            addSphere(leftBreast, _settings.femaleBreast);
            addSphere(rightBreast, _settings.femaleBreast);
        }
        return boundsInitialized ? std::optional{body} : std::nullopt;
    }

    bool ActorBodyCollision::Intersects(const Bounds& a_bounds, const RE::NiPoint3& a_minimum, const RE::NiPoint3& a_maximum) {
        return a_maximum.x >= a_bounds.minimum.x && a_minimum.x <= a_bounds.maximum.x && a_maximum.y >= a_bounds.minimum.y && a_minimum.y <= a_bounds.maximum.y && a_maximum.z >= a_bounds.minimum.z &&
               a_minimum.z <= a_bounds.maximum.z;
    }
}  // namespace LeashFramework::Physics
