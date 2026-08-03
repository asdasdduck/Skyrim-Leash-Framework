#include "ActorRestrictions.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

#include "../PCH.h"

namespace LeashFramework::ActorRestrictions {
    namespace {
        struct FactionDefinition {
            RE::FormID formID;
            std::string_view pluginName;
        };

        constexpr std::array kBlockedFactionDefinitions{
            FactionDefinition{0xE50F, "SexLab.esm"},  // "Animation" faction ;)
        };

        std::vector<RE::TESFaction*> blockedFactions;
    }  // namespace

    void ResolveFactions() {
        blockedFactions.clear();
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) {
            return;
        }
        for (const auto& definition : kBlockedFactionDefinitions) {
            if (auto* faction = dataHandler->LookupForm<RE::TESFaction>(definition.formID, definition.pluginName)) {
                blockedFactions.push_back(faction);
            }
        }
    }

    bool IsRagdollOrTeleportBlocked(const RE::Actor& a_actor) {
        return std::ranges::any_of(blockedFactions, [&](const auto* a_faction) { return a_actor.IsInFaction(a_faction); });
    }
}  // namespace LeashFramework::ActorRestrictions
