#include "NavMeshPathfinder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <queue>
#include <unordered_map>
#include <unordered_set>

#include "../PCH.h"

namespace LeashFramework::Pathing {
    namespace {
        struct TriangleLocation {
            RE::BSNavmesh* navMesh{};
            std::uint16_t triangle{std::numeric_limits<std::uint16_t>::max()};

            bool operator==(const TriangleLocation&) const = default;
        };

        struct TriangleLocationHash {
            std::size_t operator()(const TriangleLocation& a_location) const noexcept {
                const auto pointer = reinterpret_cast<std::uintptr_t>(a_location.navMesh);
                return std::hash<std::uintptr_t>{}(pointer) ^ (static_cast<std::size_t>(a_location.triangle) << 1);
            }
        };

        struct LocatedPoint {
            TriangleLocation location;
            RE::NiPoint3 point;
            float distanceSquared{std::numeric_limits<float>::max()};
        };

        struct SearchRecord {
            float cost{std::numeric_limits<float>::max()};
            TriangleLocation parent;
            std::uint8_t parentEdge{};
            bool hasParent{};
        };

        struct OpenEntry {
            TriangleLocation location;
            float cost{};
            float estimatedCost{};
        };

        struct OpenEntryCompare {
            bool operator()(const OpenEntry& a_left, const OpenEntry& a_right) const noexcept { return a_left.estimatedCost > a_right.estimatedCost; }
        };

        struct Portal {
            RE::NiPoint3 left;
            RE::NiPoint3 right;
        };

        constexpr std::uint16_t kNoTriangle = std::numeric_limits<std::uint16_t>::max();
        constexpr std::size_t kMaximumNavMeshes = 64;
        constexpr std::size_t kMaximumExpandedTriangles = 4096;
        constexpr float kPointEqualityDistance = 0.001F;

        [[nodiscard]] float DistanceSquared(const RE::NiPoint3& a_left, const RE::NiPoint3& a_right) {
            const auto delta = a_left - a_right;
            return delta.Dot(delta);
        }

        [[nodiscard]] float HorizontalDistanceSquared(const RE::NiPoint3& a_left, const RE::NiPoint3& a_right) {
            const auto deltaX = a_left.x - a_right.x;
            const auto deltaY = a_left.y - a_right.y;
            return deltaX * deltaX + deltaY * deltaY;
        }

        [[nodiscard]] float SignedHorizontalArea(const RE::NiPoint3& a_first, const RE::NiPoint3& a_second, const RE::NiPoint3& a_third) {
            return (a_second.x - a_first.x) * (a_third.y - a_first.y) - (a_second.y - a_first.y) * (a_third.x - a_first.x);
        }

        [[nodiscard]] bool IsSameHorizontalPoint(const RE::NiPoint3& a_left, const RE::NiPoint3& a_right) { return HorizontalDistanceSquared(a_left, a_right) <= kPointEqualityDistance * kPointEqualityDistance; }

        [[nodiscard]] RE::NiPoint3 ClosestPointOnTriangle(const RE::NiPoint3& a_point, const RE::NiPoint3& a_first, const RE::NiPoint3& a_second, const RE::NiPoint3& a_third) {
            const auto firstSecond = a_second - a_first;
            const auto firstThird = a_third - a_first;
            const auto firstPoint = a_point - a_first;
            const auto firstSecondDot = firstSecond.Dot(firstPoint);
            const auto firstThirdDot = firstThird.Dot(firstPoint);
            if (firstSecondDot <= 0.0F && firstThirdDot <= 0.0F) {
                return a_first;
            }

            const auto secondPoint = a_point - a_second;
            const auto secondFirstDot = firstSecond.Dot(secondPoint);
            const auto secondThirdDot = firstThird.Dot(secondPoint);
            if (secondFirstDot >= 0.0F && secondThirdDot <= secondFirstDot) {
                return a_second;
            }

            const auto firstSecondRegion = firstSecondDot * secondThirdDot - secondFirstDot * firstThirdDot;
            if (firstSecondRegion <= 0.0F && firstSecondDot >= 0.0F && secondFirstDot <= 0.0F) {
                return a_first + firstSecond * (firstSecondDot / (firstSecondDot - secondFirstDot));
            }

            const auto thirdPoint = a_point - a_third;
            const auto thirdFirstDot = firstSecond.Dot(thirdPoint);
            const auto thirdSecondDot = firstThird.Dot(thirdPoint);
            if (thirdSecondDot >= 0.0F && thirdFirstDot <= thirdSecondDot) {
                return a_third;
            }

            const auto firstThirdRegion = thirdFirstDot * firstThirdDot - firstSecondDot * thirdSecondDot;
            if (firstThirdRegion <= 0.0F && firstThirdDot >= 0.0F && thirdSecondDot <= 0.0F) {
                return a_first + firstThird * (firstThirdDot / (firstThirdDot - thirdSecondDot));
            }

            const auto secondThirdRegion = secondFirstDot * thirdSecondDot - thirdFirstDot * secondThirdDot;
            if (secondThirdRegion <= 0.0F && secondThirdDot - secondFirstDot >= 0.0F && thirdFirstDot - thirdSecondDot >= 0.0F) {
                return a_second + (a_third - a_second) * ((secondThirdDot - secondFirstDot) / ((secondThirdDot - secondFirstDot) + (thirdFirstDot - thirdSecondDot)));
            }

            const auto denominator = 1.0F / (secondThirdRegion + firstThirdRegion + firstSecondRegion);
            const auto secondWeight = firstThirdRegion * denominator;
            const auto thirdWeight = firstSecondRegion * denominator;
            return a_first + firstSecond * secondWeight + firstThird * thirdWeight;
        }

        [[nodiscard]] bool GetTriangleVertices(const TriangleLocation& a_location, std::array<RE::NiPoint3, 3>& a_vertices) {
            if (!a_location.navMesh || a_location.triangle >= a_location.navMesh->triangles.size()) {
                return false;
            }
            const auto& triangle = a_location.navMesh->triangles[a_location.triangle];
            for (std::size_t index = 0; index < a_vertices.size(); ++index) {
                if (triangle.vertices[index] >= a_location.navMesh->vertices.size()) {
                    return false;
                }
                a_vertices[index] = a_location.navMesh->vertices[triangle.vertices[index]].location;
            }
            return true;
        }

        [[nodiscard]] RE::NiPoint3 GetTriangleCenter(const TriangleLocation& a_location) {
            std::array<RE::NiPoint3, 3> vertices;
            if (!GetTriangleVertices(a_location, vertices)) {
                return {};
            }
            return (vertices[0] + vertices[1] + vertices[2]) * (1.0F / 3.0F);
        }

        [[nodiscard]] bool IsTriangleTraversable(RE::BSNavmesh& a_navMesh, std::uint16_t a_triangleIndex) {
            if (a_triangleIndex >= a_navMesh.triangles.size()) {
                return false;
            }
            const auto& triangle = a_navMesh.triangles[a_triangleIndex];
            if (triangle.triangleFlags.all(RE::BSNavmeshTriangle::TriangleFlag::kDeleted)) {
                return false;
            }
            return std::ranges::none_of(a_navMesh.closedDoors, [&](const auto& a_closedDoor) { return a_closedDoor.triangleIndex == a_triangleIndex; });
        }

        [[nodiscard]] RE::BSNavmesh* ResolveNavMesh(RE::FormID a_formID) {
            auto* tes = RE::TES::GetSingleton();
            auto* runtimeData = tes ? std::addressof(tes->GetRuntimeData2()) : nullptr;
            auto* info = runtimeData && runtimeData->unk2A8 ? runtimeData->unk2A8->GetNavmeshInfo(a_formID) : nullptr;
            return info ? info->navMesh : nullptr;
        }

        [[nodiscard]] bool IsLoadedNavMesh(const RE::BSNavmesh* a_navMesh) {
            return a_navMesh && a_navMesh->parentCell && a_navMesh->parentCell->QAttached() && !a_navMesh->vertices.empty() && !a_navMesh->triangles.empty();
        }

        void AddCellNavMeshes(RE::TESObjectCELL* a_cell, std::vector<RE::BSNavmesh*>& a_navMeshes, std::unordered_set<RE::BSNavmesh*>& a_seen) {
            if (!a_cell || !a_cell->IsAttached()) {
                return;
            }
            auto* navMeshArray = a_cell->GetRuntimeData().navMeshes;
            if (!navMeshArray) {
                return;
            }
            for (const auto& navMesh : navMeshArray->navMeshes) {
                auto* runtimeNavMesh = static_cast<RE::BSNavmesh*>(navMesh.get());
                if (IsLoadedNavMesh(runtimeNavMesh) && a_seen.insert(runtimeNavMesh).second) {
                    a_navMeshes.push_back(runtimeNavMesh);
                }
            }
        }

        [[nodiscard]] RE::BSNavmesh* GetLinkedNavMesh(RE::BSNavmesh& a_navMesh, std::uint16_t a_triangleIndex, std::uint8_t a_edge) {
            const auto& triangle = a_navMesh.triangles[a_triangleIndex];
            const auto linkFlag = static_cast<RE::BSNavmeshTriangle::TriangleFlag>(1U << a_edge);
            if (!triangle.triangleFlags.all(linkFlag)) {
                return nullptr;
            }
            const auto extraInfoIndex = triangle.triangles[a_edge];
            if (extraInfoIndex >= a_navMesh.extraEdgeInfo.size()) {
                return nullptr;
            }
            const auto& extraInfo = a_navMesh.extraEdgeInfo[extraInfoIndex];
            if (extraInfo.type != RE::EDGE_EXTRA_INFO_TYPE::kPortal && extraInfo.type != RE::EDGE_EXTRA_INFO_TYPE::kEnableDisablePortal) {
                return nullptr;
            }
            return extraInfo.portal.otherMeshID ? ResolveNavMesh(extraInfo.portal.otherMeshID) : nullptr;
        }

        [[nodiscard]] std::vector<RE::BSNavmesh*> CollectNavMeshes(RE::TESObjectCELL* a_startCell, RE::TESObjectCELL* a_goalCell) {
            std::vector<RE::BSNavmesh*> navMeshes;
            std::unordered_set<RE::BSNavmesh*> seen;
            AddCellNavMeshes(a_startCell, navMeshes, seen);
            AddCellNavMeshes(a_goalCell, navMeshes, seen);

            for (std::size_t meshIndex = 0; meshIndex < navMeshes.size() && navMeshes.size() < kMaximumNavMeshes; ++meshIndex) {
                auto& navMesh = *navMeshes[meshIndex];
                for (std::uint16_t triangleIndex = 0; triangleIndex < navMesh.triangles.size(); ++triangleIndex) {
                    const auto& triangle = navMesh.triangles[triangleIndex];
                    for (std::uint8_t edge = 0; edge < 3; ++edge) {
                        if (triangle.triangles[edge] == kNoTriangle) {
                            continue;
                        }
                        auto* linkedNavMesh = GetLinkedNavMesh(navMesh, triangleIndex, edge);
                        if (IsLoadedNavMesh(linkedNavMesh) && seen.insert(linkedNavMesh).second) {
                            navMeshes.push_back(linkedNavMesh);
                            if (navMeshes.size() == kMaximumNavMeshes) {
                                return navMeshes;
                            }
                        }
                    }
                }
            }
            return navMeshes;
        }

        [[nodiscard]] LocatedPoint FindClosestPoint(const RE::NiPoint3& a_point, const std::vector<RE::BSNavmesh*>& a_navMeshes) {
            LocatedPoint closest;
            for (auto* navMesh : a_navMeshes) {
                for (std::uint16_t triangleIndex = 0; triangleIndex < navMesh->triangles.size(); ++triangleIndex) {
                    if (!IsTriangleTraversable(*navMesh, triangleIndex)) {
                        continue;
                    }
                    const TriangleLocation location{navMesh, triangleIndex};
                    std::array<RE::NiPoint3, 3> vertices;
                    if (!GetTriangleVertices(location, vertices)) {
                        continue;
                    }
                    const auto point = ClosestPointOnTriangle(a_point, vertices[0], vertices[1], vertices[2]);
                    const auto distanceSquared = DistanceSquared(a_point, point);
                    if (distanceSquared < closest.distanceSquared) {
                        closest = LocatedPoint{location, point, distanceSquared};
                    }
                }
            }
            return closest;
        }

        [[nodiscard]] bool GetNeighbor(const TriangleLocation& a_location, std::uint8_t a_edge, TriangleLocation& a_neighbor) {
            const auto& triangle = a_location.navMesh->triangles[a_location.triangle];
            const auto neighborIndex = triangle.triangles[a_edge];
            if (neighborIndex == kNoTriangle) {
                return false;
            }

            const auto linkFlag = static_cast<RE::BSNavmeshTriangle::TriangleFlag>(1U << a_edge);
            if (!triangle.triangleFlags.all(linkFlag)) {
                a_neighbor = TriangleLocation{a_location.navMesh, neighborIndex};
                return IsTriangleTraversable(*a_neighbor.navMesh, a_neighbor.triangle);
            }

            if (neighborIndex >= a_location.navMesh->extraEdgeInfo.size()) {
                return false;
            }
            const auto& extraInfo = a_location.navMesh->extraEdgeInfo[neighborIndex];
            if (extraInfo.type != RE::EDGE_EXTRA_INFO_TYPE::kPortal && extraInfo.type != RE::EDGE_EXTRA_INFO_TYPE::kEnableDisablePortal) {
                return false;
            }
            const auto& portal = extraInfo.portal;
            auto* linkedNavMesh = portal.otherMeshID ? ResolveNavMesh(portal.otherMeshID) : nullptr;
            if (!IsLoadedNavMesh(linkedNavMesh)) {
                return false;
            }
            a_neighbor = TriangleLocation{linkedNavMesh, portal.triangle};
            return IsTriangleTraversable(*linkedNavMesh, portal.triangle);
        }

        [[nodiscard]] bool GetPortal(const TriangleLocation& a_location, const TriangleLocation& a_neighbor, std::uint8_t a_edge, float a_actorRadius, Portal& a_portal) {
            std::array<RE::NiPoint3, 3> vertices;
            if (!GetTriangleVertices(a_location, vertices)) {
                return false;
            }

            auto first = vertices[a_edge];
            auto second = vertices[(a_edge + 1) % 3];
            const auto width = std::sqrt(HorizontalDistanceSquared(first, second));
            if (width <= a_actorRadius * 2.0F) {
                return false;
            }

            const auto shrink = a_actorRadius / width;
            const auto edge = second - first;
            first += edge * shrink;
            second -= edge * shrink;

            const auto currentCenter = GetTriangleCenter(a_location);
            const auto neighborCenter = GetTriangleCenter(a_neighbor);
            const auto firstSide = SignedHorizontalArea(currentCenter, neighborCenter, first);
            const auto secondSide = SignedHorizontalArea(currentCenter, neighborCenter, second);
            if (firstSide >= secondSide) {
                a_portal = Portal{first, second};
            } else {
                a_portal = Portal{second, first};
            }
            return true;
        }

        void AppendWaypoint(std::vector<RE::NiPoint3>& a_path, const RE::NiPoint3& a_waypoint) {
            if (a_path.empty() || !IsSameHorizontalPoint(a_path.back(), a_waypoint)) {
                a_path.push_back(a_waypoint);
            }
        }

        [[nodiscard]] std::vector<RE::NiPoint3> PullString(const LocatedPoint& a_start, const LocatedPoint& a_goal, const std::vector<Portal>& a_portals) {
            std::vector<RE::NiPoint3> path;
            auto apex = a_start.point;
            auto left = apex;
            auto right = apex;
            std::size_t apexIndex{};
            std::size_t leftIndex{};
            std::size_t rightIndex{};

            for (std::size_t portalIndex = 0; portalIndex < a_portals.size(); ++portalIndex) {
                const auto& portal = a_portals[portalIndex];
                if (SignedHorizontalArea(apex, right, portal.right) >= 0.0F) {
                    if (IsSameHorizontalPoint(apex, right) || SignedHorizontalArea(apex, left, portal.right) < 0.0F) {
                        right = portal.right;
                        rightIndex = portalIndex;
                    } else {
                        AppendWaypoint(path, left);
                        apex = left;
                        apexIndex = leftIndex;
                        left = apex;
                        right = apex;
                        leftIndex = apexIndex;
                        rightIndex = apexIndex;
                        portalIndex = apexIndex;
                        continue;
                    }
                }

                if (SignedHorizontalArea(apex, left, portal.left) <= 0.0F) {
                    if (IsSameHorizontalPoint(apex, left) || SignedHorizontalArea(apex, right, portal.left) > 0.0F) {
                        left = portal.left;
                        leftIndex = portalIndex;
                    } else {
                        AppendWaypoint(path, right);
                        apex = right;
                        apexIndex = rightIndex;
                        left = apex;
                        right = apex;
                        leftIndex = apexIndex;
                        rightIndex = apexIndex;
                        portalIndex = apexIndex;
                    }
                }
            }

            AppendWaypoint(path, a_goal.point);
            return path;
        }

        [[nodiscard]] std::vector<RE::NiPoint3> BuildPath(const LocatedPoint& a_start, const LocatedPoint& a_goal, float a_actorRadius) {
            if (a_start.location == a_goal.location) {
                return {a_goal.point};
            }

            std::priority_queue<OpenEntry, std::vector<OpenEntry>, OpenEntryCompare> open;
            std::unordered_map<TriangleLocation, SearchRecord, TriangleLocationHash> records;
            records[a_start.location].cost = 0.0F;
            open.push(OpenEntry{a_start.location, 0.0F, std::sqrt(HorizontalDistanceSquared(a_start.point, a_goal.point))});

            std::size_t expanded{};
            while (!open.empty() && expanded < kMaximumExpandedTriangles) {
                const auto currentEntry = open.top();
                open.pop();
                const auto current = currentEntry.location;
                if (currentEntry.cost > records[current].cost) {
                    continue;
                }
                if (current == a_goal.location) {
                    break;
                }
                ++expanded;

                const auto currentCenter = GetTriangleCenter(current);
                const auto currentCost = records[current].cost;
                for (std::uint8_t edge = 0; edge < 3; ++edge) {
                    TriangleLocation neighbor;
                    if (!GetNeighbor(current, edge, neighbor)) {
                        continue;
                    }
                    Portal portal;
                    if (!GetPortal(current, neighbor, edge, a_actorRadius, portal)) {
                        continue;
                    }
                    const auto neighborCenter = GetTriangleCenter(neighbor);
                    auto stepCost = std::sqrt(DistanceSquared(currentCenter, neighborCenter));
                    if (neighbor.navMesh->triangles[neighbor.triangle].triangleFlags.all(RE::BSNavmeshTriangle::TriangleFlag::kPreferred)) {
                        stepCost *= 0.85F;
                    }
                    const auto newCost = currentCost + stepCost;
                    auto& record = records[neighbor];
                    if (newCost >= record.cost) {
                        continue;
                    }
                    record.cost = newCost;
                    record.parent = current;
                    record.parentEdge = edge;
                    record.hasParent = true;
                    const auto heuristic = std::sqrt(HorizontalDistanceSquared(neighborCenter, a_goal.point));
                    open.push(OpenEntry{neighbor, newCost, newCost + heuristic});
                }
            }

            auto goalRecord = records.find(a_goal.location);
            if (goalRecord == records.end() || !goalRecord->second.hasParent) {
                return {};
            }

            struct PathStep {
                TriangleLocation location;
                TriangleLocation neighbor;
                std::uint8_t exitEdge{};
            };
            std::vector<PathStep> reverseSteps;
            auto current = a_goal.location;
            while (!(current == a_start.location)) {
                const auto record = records.find(current);
                if (record == records.end() || !record->second.hasParent) {
                    return {};
                }
                reverseSteps.push_back(PathStep{record->second.parent, current, record->second.parentEdge});
                current = record->second.parent;
            }
            std::ranges::reverse(reverseSteps);

            std::vector<Portal> portals;
            portals.reserve(reverseSteps.size() + 1);
            for (const auto& step : reverseSteps) {
                Portal portal;
                if (!GetPortal(step.location, step.neighbor, step.exitEdge, a_actorRadius, portal)) {
                    return {};
                }
                portals.push_back(portal);
            }
            portals.push_back(Portal{a_goal.point, a_goal.point});
            return PullString(a_start, a_goal, portals);
        }
    }  // namespace

    std::vector<RE::NiPoint3> NavMeshPathfinder::FindPath(const RE::NiPoint3& a_start, RE::TESObjectCELL* a_startCell, const RE::NiPoint3& a_goal, RE::TESObjectCELL* a_goalCell, float a_actorRadius) const {
        LF_PROFILE_SCOPE("Pathing/FindPath");
        const auto actorRadius = std::isfinite(a_actorRadius) ? std::max(a_actorRadius, 0.0F) : 0.0F;
        const auto navMeshes = CollectNavMeshes(a_startCell, a_goalCell);
        if (navMeshes.empty()) {
            return {};
        }
        const auto start = FindClosestPoint(a_start, navMeshes);
        const auto goal = FindClosestPoint(a_goal, navMeshes);
        if (!start.location.navMesh || !goal.location.navMesh) {
            return {};
        }
        return BuildPath(start, goal, actorRadius);
    }
}  // namespace LeashFramework::Pathing
