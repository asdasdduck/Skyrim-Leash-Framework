#pragma once

#include <vector>

#include "../PCH.h"

namespace LeashFramework::Pathing {
    class NavMeshPathfinder {
    public:
        [[nodiscard]] std::vector<RE::NiPoint3> FindPath(const RE::NiPoint3& a_start, RE::TESObjectCELL* a_startCell, const RE::NiPoint3& a_goal, RE::TESObjectCELL* a_goalCell, float a_actorRadius) const;
    };
}  // namespace LeashFramework::Pathing
