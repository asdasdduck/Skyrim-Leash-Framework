#include "Actor/ActorRestrictions.h"
#include "Hooks/FrameHook.h"
#include "Leash/LeashManager.h"
#include "PCH.h"
#include "Papyrus/LeashPapyrus.h"
#include "Serialization/LeashSerialization.h"
#include "UI/ModMenu.h"

namespace {
    void OnSKSEMessage(SKSE::MessagingInterface::Message* a_msg) {
        switch (a_msg->type) {
            case SKSE::MessagingInterface::kPreLoadGame:
                LeashFramework::LeashManager::GetSingleton().HandlePreLoadGame();
                break;
            case SKSE::MessagingInterface::kPostLoadGame: {
                const auto succeeded = a_msg->data != nullptr;
                LeashFramework::LeashManager::GetSingleton().HandlePostLoadGame(succeeded);
                break;
            }
            case SKSE::MessagingInterface::kSaveGame: {
                auto& manager = LeashFramework::LeashManager::GetSingleton();
                // We need to prevent shit getting baked into the save otherwise it might lead to corruption.
                const auto released = manager.PrepareForSave();
                if (manager.PullDiagnosticsEnabled()) {
                    SKSE::log::info("[PullDiag] SKSE save-game releasedPulls={}", released);
                }
                break;
            }
            default:
                break;
        }

        if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
            LeashFramework::ActorRestrictions::ResolveFactions();
            LeashFramework::Hooks::FrameHook::Install();
            LeashFramework::UI::ModMenu::Register();
        }
    }
}  // namespace

SKSEPluginLoad(const SKSE::LoadInterface* a_skse) {
    SKSE::Init(a_skse);
    spdlog::set_level(spdlog::level::info);

    SKSE::log::info("{} loading", SKSE::PluginDeclaration::GetSingleton()->GetName());

    SKSE::AllocTrampoline(1 << 12);

    const auto* papyrus = SKSE::GetPapyrusInterface();
    if (!papyrus || !papyrus->Register(LeashFramework::Papyrus::Register)) {
        SKSE::log::error("Failed to register Papyrus API");
        return false;
    }
    if (!LeashFramework::Serialization::Install()) {
        return false;
    }
    if (const auto* messaging = SKSE::GetMessagingInterface(); !messaging->RegisterListener(OnSKSEMessage)) {
        SKSE::log::error("Failed to register SKSE messaging listener somehow");
        return false;
    }
    SKSE::log::info("{} loaded", SKSE::PluginDeclaration::GetSingleton()->GetName());
    return true;
}
