#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>

#include "../PCH.h"

namespace LeashFramework::Physics::ActorShapeSweep {
    struct Shape {
        RE::NiPoint3 start;
        RE::NiPoint3 end;
        float radius{};
    };

    struct Hit {
        RE::NiPoint3 normal;
        float fraction{};
        float axisFraction{};
    };

    [[nodiscard]] inline double Dot(const RE::NiPoint3& a_left, const RE::NiPoint3& a_right) {
        return static_cast<double>(a_left.x) * a_right.x + static_cast<double>(a_left.y) * a_right.y + static_cast<double>(a_left.z) * a_right.z;
    }

    [[nodiscard]] inline std::optional<Hit> Cast(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, float a_radius, const Shape& a_previous, const Shape& a_current) {
        constexpr double kContactTolerance = 0.0001;
        constexpr std::size_t kMaximumIterations = 64;
        const auto start = a_previous.start - a_from;
        const auto end = a_previous.end - a_from;
        const auto movement = a_to - a_from;
        const auto startMovement = a_current.start - a_previous.start - movement;
        const auto endMovement = a_current.end - a_previous.end - movement;
        const auto radius = static_cast<double>(a_radius) + a_previous.radius;
        const auto radiusMovement = static_cast<double>(a_current.radius) - a_previous.radius;
        double fraction{};

        for (std::size_t iteration = 0; iteration < kMaximumIterations; ++iteration) {
            // Work relative to the particle, avoiding interpolation of large world coordinates.
            const auto currentStart = start + startMovement * static_cast<float>(fraction);
            const auto currentEnd = end + endMovement * static_cast<float>(fraction);
            const auto axis = currentEnd - currentStart;
            const auto axisSquared = Dot(axis, axis);
            const auto axisFraction = axisSquared > 0.0001 ? std::clamp(-Dot(currentStart, axis) / axisSquared, 0.0, 1.0) : 0.0;
            auto normal = -(currentStart + axis * static_cast<float>(axisFraction));
            const auto distance = std::sqrt(Dot(normal, normal));
            if (distance <= kContactTolerance) {
                normal = -(startMovement + (endMovement - startMovement) * static_cast<float>(axisFraction));
                if (normal.Unitize() <= 0.0F) {
                    normal = {0.0F, 0.0F, 1.0F};
                }
                return Hit{.normal = normal, .fraction = static_cast<float>(fraction), .axisFraction = static_cast<float>(axisFraction)};
            }
            normal *= static_cast<float>(1.0 / distance);
            const auto currentRadius = radius + radiusMovement * fraction;
            const auto separation = distance - currentRadius;
            const auto closingStart = Dot(startMovement, normal) + radiusMovement;
            const auto closingEnd = Dot(endMovement, normal) + radiusMovement;
            const auto closingSpeed = std::lerp(closingStart, closingEnd, axisFraction);
            if (separation <= kContactTolerance && (separation < 0.0 || closingSpeed > 0.0)) {
                return Hit{.normal = normal, .fraction = static_cast<float>(fraction), .axisFraction = static_cast<float>(axisFraction)};
            }

            // Neither endpoint may cross this separating plane before the next closest-feature query.
            auto advance = 2.0;
            if (closingStart > 0.0) {
                advance = (std::min)(advance, (std::max)(0.0, -Dot(currentStart, normal) - currentRadius) / closingStart);
            }
            if (closingEnd > 0.0) {
                advance = (std::min)(advance, (std::max)(0.0, -Dot(currentEnd, normal) - currentRadius) / closingEnd);
            }
            if (fraction + advance > 1.0) {
                return std::nullopt;
            }
            if (advance <= 1.0e-8 || iteration + 1 == kMaximumIterations) {
                // Preserve the last non-penetrating time if a grazing contact exhausts numerical progress.
                return Hit{.normal = normal, .fraction = static_cast<float>(fraction), .axisFraction = static_cast<float>(axisFraction)};
            }
            fraction += advance;
        }
        return std::nullopt;
    }
}  // namespace LeashFramework::Physics::ActorShapeSweep
