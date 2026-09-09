#include "RagdollHold.h"

#include <array>
#include <mutex>
#include <unordered_map>

namespace LeashFramework::Recovery {
    namespace {
        using KnockdownUpdate = void (*)(RE::AIProcess*, RE::Actor*);

        REL::Relocation<KnockdownUpdate> originalNPCUpdate;
        REL::Relocation<KnockdownUpdate> originalPlayerUpdate;
        std::mutex holdMutex;
        std::unordered_map<const RE::Actor*, const RagdollHold*> heldActors;
        bool installed{};

        [[nodiscard]] bool ShouldHold(RE::Actor* a_actor) {
            if (!a_actor || a_actor->AsActorState()->GetKnockState() != RE::KNOCK_STATE_ENUM::kExplode || a_actor->AsActorState()->GetLifeState() != RE::ACTOR_LIFE_STATE::kAlive ||
                a_actor->IsDead(true) || a_actor->IsDisabled() || a_actor->IsInKillMove() || !a_actor->Is3DLoaded() || a_actor->GetActorRuntimeData().boolBits.any(RE::Actor::BOOL_BITS::kParalyzed)) {
                return false;
            }

            // Animation updates can run off-thread; this registry never reads the leash manager or its states.
            const std::lock_guard lock{holdMutex};
            return heldActors.contains(a_actor);
        }

        void UpdateNPC(RE::AIProcess* a_process, RE::Actor* a_actor) {
            if (!ShouldHold(a_actor)) {
                originalNPCUpdate(a_process, a_actor);
            }
        }

        void UpdatePlayer(RE::AIProcess* a_process, RE::Actor* a_actor) {
            if (!ShouldHold(a_actor)) {
                originalPlayerUpdate(a_process, a_actor);
            }
        }
    }  // namespace

    void RagdollHold::InstallHooks() {
        if (installed) {
            return;
        }

        // Todo: Find newer AE offsets
        REL::Relocation<std::uintptr_t> npcCall{REL::VariantID(36370, 37361, 0x5E1F10), REL::VariantOffset(0x59, 0x6F, 0x59)};
        REL::Relocation<std::uintptr_t> playerCall{REL::VariantID(39375, 40447, 0x6BEC10), REL::VariantOffset(0x7B8, 0xD31, 0x808)};
        constexpr std::array<std::uint8_t, 5> expectedNPCSE{0xE8, 0x12, 0x40, 0x0A, 0x00};
        constexpr std::array<std::uint8_t, 5> expectedPlayerSE{0xE8, 0xC3, 0xEB, 0xFD, 0xFF};
        constexpr std::array<std::uint8_t, 5> expectedNPCAE{0xE8, 0xDC, 0x64, 0x0A, 0x00};
        constexpr std::array<std::uint8_t, 5> expectedPlayerAE{0xE8, 0x8A, 0xDC, 0xFD, 0xFF};
        constexpr std::array<std::uint8_t, 5> expectedNPCVR{0xE8, 0x12, 0x4E, 0x0A, 0x00};
        constexpr std::array<std::uint8_t, 5> expectedPlayerVR{0xE8, 0x67, 0x79, 0xFC, 0xFF};
        const auto& expectedNPCCode = REL::Module::IsVR() ? expectedNPCVR : REL::Module::IsAE() ? expectedNPCAE : expectedNPCSE;
        const auto& expectedPlayerCode = REL::Module::IsVR() ? expectedPlayerVR : REL::Module::IsAE() ? expectedPlayerAE : expectedPlayerSE;
        if (!REL::verify_code(npcCall.address(), expectedNPCCode) || !REL::verify_code(playerCall.address(), expectedPlayerCode)) {
            SKSE::log::critical("Unexpected knockdown-update calls; forced ragdoll recovery is disabled");
            return;
        }

        originalNPCUpdate = npcCall.write_call<5>(UpdateNPC);
        originalPlayerUpdate = playerCall.write_call<5>(UpdatePlayer);
        installed = true;
        SKSE::log::info("Installed NPC and player ragdoll hold hooks");
    }

    bool RagdollHold::IsAvailable() { return installed; }

    std::unique_ptr<RagdollHold> RagdollHold::Acquire(RE::Actor& a_actor) {
        if (!installed) {
            return nullptr;
        }

        auto hold = std::unique_ptr<RagdollHold>{new RagdollHold(a_actor)};
        {
            const std::lock_guard lock{holdMutex};
            if (!heldActors.try_emplace(std::addressof(a_actor), hold.get()).second) {
                return nullptr;
            }
        }
        return hold;
    }

    RagdollHold::RagdollHold(RE::Actor& a_actor) : _actor(std::addressof(a_actor)) {}

    RagdollHold::~RagdollHold() {
        const std::lock_guard lock{holdMutex};
        const auto entry = heldActors.find(_actor.get());
        if (entry != heldActors.end() && entry->second == this) {
            heldActors.erase(entry);
        }
    }
}  // namespace LeashFramework::Recovery
