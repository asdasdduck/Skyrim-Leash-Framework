#pragma once

#include <memory>

#include "../PCH.h"

namespace LeashFramework::Recovery {
    class RagdollHold {
    public:
        static void InstallHooks();
        [[nodiscard]] static bool IsAvailable();
        [[nodiscard]] static std::unique_ptr<RagdollHold> Acquire(RE::Actor& a_actor);

        ~RagdollHold();
        RagdollHold(const RagdollHold&) = delete;
        RagdollHold& operator=(const RagdollHold&) = delete;

    private:
        explicit RagdollHold(RE::Actor& a_actor);

        RE::NiPointer<RE::Actor> _actor;
    };
}  // namespace LeashFramework::Recovery
