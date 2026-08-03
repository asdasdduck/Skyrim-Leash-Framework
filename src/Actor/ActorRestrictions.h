#pragma once

#include "../PCH.h"

namespace LeashFramework::ActorRestrictions {
    void ResolveFactions();
    [[nodiscard]] bool IsRagdollOrTeleportBlocked(const RE::Actor& a_actor);
}  // namespace LeashFramework::ActorRestrictions
