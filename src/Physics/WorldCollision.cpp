#include "WorldCollision.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>

#include "../PCH.h"
#include "ActorBodyCollision.h"

namespace RE {
    hkpCdPointCollector::~hkpCdPointCollector() = default;

    void hkpCdPointCollector::Reset() { earlyOutDistance = std::bit_cast<float>(0x7F7FFFEEU); }
}  // namespace RE

namespace LeashFramework::Physics {
    namespace {
        constexpr std::size_t kMaximumSweepIterations = 4;
        constexpr std::size_t kContactProjectionIterations = 2;
        constexpr float kMinimumMovementSquared = 0.0001F;
        constexpr float kMinimumSphereRadius = 0.01F;
        constexpr float kContactSkin = 0.05F;
        // This is scaled into Havok units later and needs to stay smaller than the skin
        constexpr float kCastToleranceFraction = 0.1F;
        constexpr float kMatchingContactNormalDot = 0.995F;

        struct SphereShapeStorage {
            std::uintptr_t vtable;
            std::uint16_t memSizeAndFlags{0xFFFF};
            std::int16_t referenceCount{1};
            std::uint32_t pad0C{};
            void* userData{};
            RE::hkpShapeType type{RE::hkpShapeType::kSphere};
            std::uint32_t pad1C{};
            float radius;
            std::uint32_t pad24{};
        };
        static_assert(sizeof(SphereShapeStorage) == sizeof(RE::hkpSphereShape));

        // These seem to be the most stable...
        [[nodiscard]] bool IsCollidableLayer(RE::COL_LAYER a_layer) {
            return a_layer == RE::COL_LAYER::kStatic || a_layer == RE::COL_LAYER::kAnimStatic || a_layer == RE::COL_LAYER::kTerrain || a_layer == RE::COL_LAYER::kGround;
        }

        [[nodiscard]] const RE::hkpCollidable* GetRootCollidable(const RE::hkpCdBody* a_body) {
            if (!a_body) {
                return nullptr;
            }
            while (a_body->parent) {
                a_body = a_body->parent;
            }
            return reinterpret_cast<const RE::hkpCollidable*>(a_body);
        }

        class SphereCastCollector final : public RE::hkpCdPointCollector {
        public:
            SphereCastCollector(const RE::hkpCollidable* a_queryCollidable, const RE::NiPoint3& a_direction, bool a_collectOverlap)
                : _queryCollidable(a_queryCollidable), _direction(a_direction), _collectOverlap(a_collectOverlap) {
                Reset();
            }

            ~SphereCastCollector() override = default;

            void AddCdPoint(const RE::hkpCdPoint& a_point) override {
                const auto* rootA = GetRootCollidable(a_point.cdBodyA);
                const auto* rootB = GetRootCollidable(a_point.cdBodyB);
                const auto queryIsA = rootA == _queryCollidable;
                const auto* other = queryIsA ? rootB : rootA;
                if ((!queryIsA && rootB != _queryCollidable) || !other || !IsCollidableLayer(other->GetCollisionLayer())) {
                    return;
                }
                const auto movingSurface = other->GetCollisionLayer() == RE::COL_LAYER::kAnimStatic;

                alignas(16) std::array<float, 4> values{};
                _mm_store_ps(values.data(), a_point.contact.separatingNormal.quad);
                auto normal = RE::NiPoint3{values[0], values[1], values[2]};
                if (!queryIsA) {
                    normal = -normal;
                }
                if (normal.Unitize() <= 0.0F || (!_collectOverlap && normal.Dot(_direction) >= 0.0F)) {
                    return;
                }

                const auto distance = values[3];
                if (_collectOverlap) {
                    if (!_hasHit || distance < _distance) {
                        _normal = normal;
                        _distance = distance;
                        _movingSurface = movingSurface;
                        _hasHit = true;
                    }
                } else if (distance >= 0.0F && distance <= 1.0F && (!_hasHit || distance < _distance)) {
                    _normal = normal;
                    _distance = distance;
                    _movingSurface = movingSurface;
                    _hasHit = true;
                    earlyOutDistance = distance;
                }
            }

            void Reset() override {
                earlyOutDistance = (std::numeric_limits<float>::max)();
                pad0C = 0;
                _normal = {};
                _distance = (std::numeric_limits<float>::max)();
                _movingSurface = false;
                _hasHit = false;
            }

            [[nodiscard]] bool HasHit() const { return _hasHit; }

            [[nodiscard]] const RE::NiPoint3& GetNormal() const { return _normal; }

            [[nodiscard]] float GetDistance() const { return _distance; }

            [[nodiscard]] bool IsMovingSurface() const { return _movingSurface; }

        private:
            const RE::hkpCollidable* _queryCollidable;
            RE::NiPoint3 _direction;
            RE::NiPoint3 _normal{};
            float _distance{(std::numeric_limits<float>::max)()};
            bool _collectOverlap;
            bool _movingSurface{};
            bool _hasHit{};
        };

        void AddContact(WorldCollision::Result& a_result, const RE::NiPoint3& a_planePoint, const RE::NiPoint3& a_normal, const ActorBodyCollision::ShapeKey& a_actorShape, bool a_movingSurface) {
            a_result.collided = true;
            for (std::size_t index = 0; index < a_result.contactCount; ++index) {
                auto& contact = a_result.contacts[index];
                if (a_actorShape.actorFormID != 0) {
                    if (contact.actorShape != a_actorShape) {
                        continue;
                    }
                    contact.planePoint = a_planePoint;
                    contact.normal = a_normal;
                    contact.movingSurface = a_movingSurface;
                    return;
                }
                if (contact.actorShape.actorFormID != 0) {
                    continue;
                }
                if (contact.movingSurface != a_movingSurface || contact.normal.Dot(a_normal) < kMatchingContactNormalDot) {
                    continue;
                }
                if ((a_planePoint - contact.planePoint).Dot(contact.normal) > 0.0F) {
                    contact.planePoint = a_planePoint;
                }
                return;
            }

            if (a_result.contactCount < a_result.contacts.size()) {
                a_result.contacts[a_result.contactCount++] = {.planePoint = a_planePoint, .normal = a_normal, .actorShape = a_actorShape, .movingSurface = a_movingSurface};
            }
        }
    }  // namespace

    WorldCollision::Result WorldCollision::ResolveMovement(RE::bhkWorld* a_world, const ActorBodyCollision* a_actorCollision, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, float a_radius,
        float a_actorInterpolation, std::span<const ActorBodyCollision::ShapeKey> a_preferredActorShapes) {
        LF_PROFILE_SCOPE("Collision/ResolveMovement");
        if (!a_world) {
            return {.position = a_to};
        }

        auto* havokWorld = a_world->GetWorld1();
        if (!havokWorld) {
            return {.position = a_to};
        }

        static REL::Relocation<std::uintptr_t> sphereVtable{RE::hkpSphereShape::VTABLE[0]};

        const auto worldScale = RE::bhkWorld::GetWorldScale();
        const auto worldScaleInverse = RE::bhkWorld::GetWorldScaleInverse();
        const auto castTolerance = kContactSkin * kCastToleranceFraction * worldScale;
        alignas(16) SphereShapeStorage sphereShape{.vtable = sphereVtable.address(), .radius = (std::max)(a_radius, kMinimumSphereRadius) * worldScale};
        RE::hkMotionState motionState{};
        motionState.transform.rotation.col0.quad = _mm_setr_ps(1.0F, 0.0F, 0.0F, 0.0F);
        motionState.transform.rotation.col1.quad = _mm_setr_ps(0.0F, 1.0F, 0.0F, 0.0F);
        motionState.transform.rotation.col2.quad = _mm_setr_ps(0.0F, 0.0F, 1.0F, 0.0F);

        RE::hkpCollidable queryCollidable{};
        queryCollidable.shape = reinterpret_cast<const RE::hkpShape*>(&sphereShape);
        queryCollidable.shapeKey = RE::HK_INVALID_SHAPE_KEY;
        queryCollidable.motion = &motionState;
        queryCollidable.forceCollideOntoPpu = static_cast<std::uint8_t>(RE::hkpCollidable::ForceCollideOntoPpuReasons::kShapeUnchecked);
        queryCollidable.broadPhaseHandle.type = static_cast<std::int8_t>(RE::hkpWorldObject::BroadPhaseType::kPhantom);
        queryCollidable.broadPhaseHandle.ownerOffset = -0x24;
        queryCollidable.broadPhaseHandle.objectQualityType = -1;
        queryCollidable.broadPhaseHandle.collisionFilterInfo.SetCollisionLayer(RE::COL_LAYER::kDroppingPick);
        queryCollidable.boundingVolumeData.min[0] = 1;
        queryCollidable.allowedPenetrationDepth = -1.0F;

        WorldCollision::Result result{.position = a_from};
        auto position = a_from;
        auto target = a_to;
        RE::BSReadLockGuard lock{a_world->worldLock};
        const auto finish = [&](const RE::NiPoint3& a_result) {
            result.position = a_result;
            // Two cheap passes handle corners without doing more Havok casts
            for (std::size_t iteration = 0; iteration < kContactProjectionIterations; ++iteration) {
                for (std::size_t index = 0; index < result.contactCount; ++index) {
                    const auto& contact = result.contacts[index];
                    const auto separation = (result.position - contact.planePoint).Dot(contact.normal);
                    if (separation < 0.0F) {
                        result.position -= contact.normal * separation;
                    }
                }
            }
            return result;
        };

        for (std::size_t iteration = 0; iteration < kMaximumSweepIterations; ++iteration) {
            auto direction = target - position;
            const auto distance = direction.Unitize();
            motionState.transform.translation = RE::hkVector4(position * worldScale);

            RE::hkpLinearCastInput input{};
            input.to = RE::hkVector4(target * worldScale);
            input.maxExtraPenetration = castTolerance;
            input.startPointTolerance = castTolerance;

            SphereCastCollector castCollector{&queryCollidable, direction, false};
            SphereCastCollector overlapCollector{&queryCollidable, direction, true};
            havokWorld->LinearCast(&queryCollidable, input, castCollector, &overlapCollector);

            const auto actorOverlap = a_actorCollision ? a_actorCollision->FindDeepestOverlap(a_world, position, a_radius, a_actorInterpolation, a_preferredActorShapes) : std::nullopt;
            const auto worldOverlap = overlapCollector.HasHit() && overlapCollector.GetDistance() < 0.0F;
            if (worldOverlap || actorOverlap) {
                const auto worldPenetration = worldOverlap ? -overlapCollector.GetDistance() * worldScaleInverse : 0.0F;
                const auto useActor = actorOverlap && (!worldOverlap || actorOverlap->penetration > worldPenetration);
                const auto& normal = useActor ? actorOverlap->normal : overlapCollector.GetNormal();
                const auto penetration = useActor ? actorOverlap->penetration : worldPenetration;
                const auto correction = normal * (penetration + kContactSkin);
                position += correction;
                target += correction;
                AddContact(result, position, normal, useActor ? actorOverlap->shape : ActorBodyCollision::ShapeKey{}, useActor || overlapCollector.IsMovingSurface());
                continue;
            }

            if (distance <= 0.0F) {
                return finish(target);
            }

            const auto actorHit = a_actorCollision ? a_actorCollision->SweepSphere(a_world, position, target, a_radius, a_actorInterpolation, a_preferredActorShapes) : std::nullopt;
            if (!castCollector.HasHit() && !actorHit) {
                return finish(target);
            }

            const auto useActor = actorHit && (!castCollector.HasHit() || actorHit->fraction < castCollector.GetDistance());
            const auto& normal = useActor ? actorHit->normal : castCollector.GetNormal();
            const auto hitFraction = useActor ? actorHit->fraction : castCollector.GetDistance();
            const auto hitPoint = position + (target - position) * std::clamp(hitFraction, 0.0F, 1.0F);

            auto remaining = target - hitPoint;
            const auto inwardDistance = remaining.Dot(normal);
            if (inwardDistance < 0.0F) {
                remaining -= normal * inwardDistance;
            }

            const auto skinPosition = hitPoint + normal * kContactSkin;
            // A near zero cast can start inside the tolerance so don't add the skin again
            if ((skinPosition - position).Dot(normal) <= 0.0F) {
                position = skinPosition;
            }
            AddContact(result, position, normal, useActor ? actorHit->shape : ActorBodyCollision::ShapeKey{}, useActor || castCollector.IsMovingSurface());
            if (remaining.SqrLength() < kMinimumMovementSquared) {
                return finish(position);
            }
            target = position + remaining;
        }

        return finish(position);
    }
}  // namespace LeashFramework::Physics
