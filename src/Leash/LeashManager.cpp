#include "LeashManager.h"

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <utility>

#include "../PCH.h"
#include "../UI/ModMenu.h"
#include "LeashInstance.h"

namespace LeashFramework {
    namespace {
        constexpr std::string_view kPluginName = "Leash.esm";
        constexpr RE::FormID kLeashedFactionFormID = 0xD6A;
        constexpr RE::FormID kLeasherFactionFormID = 0xD6B;
        constexpr RE::FormID kPlayerFormID = 0x14;

        void SendLeashEvent(const char* a_eventName, const char* a_reason, RE::FormID a_leashedFormID) {
            auto* leashed = RE::TESForm::LookupByID<RE::Actor>(a_leashedFormID);
            auto* eventSource = SKSE::GetModCallbackEventSource();
            if (!leashed || !eventSource) {
                return;
            }

            const SKSE::ModCallbackEvent event{.eventName = RE::BSFixedString{a_eventName}, .strArg = RE::BSFixedString{a_reason}, .numArg = 0.0F, .sender = leashed};
            eventSource->SendEvent(std::addressof(event));
            SKSE::log::info("Sent {} for {:08X} with reason '{}'", a_eventName, a_leashedFormID, a_reason);
        }

        void AddActorFormID(std::vector<RE::FormID>& a_formIDs, RE::FormID a_formID) {
            if (a_formID != 0) {
                a_formIDs.push_back(a_formID);
            }
        }

        [[nodiscard]] bool AnchorFormsExist(const LeashDefinition& a_definition) {
            if (a_definition.holderFormID != 0 && !RE::TESForm::LookupByID<RE::Actor>(a_definition.holderFormID)) {
                return false;
            }
            if (const auto* anchor = std::get_if<WorldPositionAnchor>(&a_definition.anchor)) {
                return RE::TESForm::LookupByID<RE::TESObjectCELL>(anchor->cellFormID) != nullptr;
            }
            return true;
        }
    }  // namespace

    LeashManager::LeashManager() {
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            player->AsPositionPlayerEventSource()->AddEventSink(this);
        }
        if (auto* eventSource = RE::ScriptEventSourceHolder::GetSingleton()) {
            eventSource->AddEventSink<RE::TESObjectLoadedEvent>(this);
        }
    }
    LeashManager::~LeashManager() = default;

    LeashManager& LeashManager::GetSingleton() {
        static LeashManager singleton;
        return singleton;
    }

    Physics::SimulationSettings LeashManager::GetSimulationSettings() const { return _settings; }

    void LeashManager::SetSimulationSettings(Physics::SimulationSettings a_settings) {
        const Physics::SimulationSettings defaults;
        const auto finiteOr = [](float a_value, float a_default) { return std::isfinite(a_value) ? a_value : a_default; };
        a_settings.gravity.x = finiteOr(a_settings.gravity.x, defaults.gravity.x);
        a_settings.gravity.y = finiteOr(a_settings.gravity.y, defaults.gravity.y);
        a_settings.gravity.z = finiteOr(a_settings.gravity.z, defaults.gravity.z);
        a_settings.damping = std::clamp(finiteOr(a_settings.damping, defaults.damping), 0.0F, 1.0F);
        a_settings.collisionPadding = (std::max)(finiteOr(a_settings.collisionPadding, defaults.collisionPadding), 0.0F);
        a_settings.stretchCompliance = (std::max)(finiteOr(a_settings.stretchCompliance, defaults.stretchCompliance), 0.0F);
        a_settings.snagReleaseStrain = (std::max)(finiteOr(a_settings.snagReleaseStrain, defaults.snagReleaseStrain), 0.0F);
        a_settings.snagBlockedDistance = (std::max)(finiteOr(a_settings.snagBlockedDistance, defaults.snagBlockedDistance), 0.0F);
        a_settings.constraintIterations = std::clamp(a_settings.constraintIterations, Physics::SimulationSettings::kMinimumConstraintIterations, Physics::SimulationSettings::kMaximumConstraintIterations);
        _settings = a_settings;
    }

    Recovery::ForcedRecoverySettings LeashManager::GetRecoverySettings() const { return _recoveryController.GetSettings(); }

    void LeashManager::SetRecoverySettings(Recovery::ForcedRecoverySettings a_settings) { _recoveryController.SetSettings(a_settings); }

    LeashTeleportSettings LeashManager::GetTeleportSettings() const { return _teleportController.GetSettings(); }

    void LeashManager::SetTeleportSettings(LeashTeleportSettings a_settings) { _teleportController.SetSettings(a_settings); }

    Animation::PullPoseSettings LeashManager::GetPullPoseSettings() const { return _pullPoseController.GetSettings(); }

    void LeashManager::SetPullPoseSettings(Animation::PullPoseSettings a_settings) { _pullPoseController.SetSettings(a_settings); }

    bool LeashManager::PullDiagnosticsEnabled() const { return _pullController.DiagnosticsEnabled(); }

    void LeashManager::SetPullDiagnosticsEnabled(bool a_enabled) { _pullController.SetDiagnosticsEnabled(a_enabled); }

    void LeashManager::HandlePreLoadGame() {
        for (auto& leash : _leashes) {
            if (leash->GetDefinition().leashedFormID == kPlayerFormID) {
                leash->ReleasePull();
                return;
            }
        }
    }

    std::size_t LeashManager::PrepareForSave() {
        std::size_t releasedPulls{};
        for (auto& leash : _leashes) {
            if (leash->ReleasePull()) {
                ++releasedPulls;
            }
        }
        return releasedPulls;
    }

    RE::BSEventNotifyControl LeashManager::ProcessEvent(const RE::PositionPlayerEvent* a_event, RE::BSTEventSource<RE::PositionPlayerEvent>*) {
        if (!a_event) {
            return RE::BSEventNotifyControl::kContinue;
        }

        const auto eventType = a_event->type.get();

        switch (eventType) {
            case RE::PositionPlayerEvent::EVENT_TYPE::kPre: {
                _positioningPlayer = true;
                _simulationSuspended = true;
                std::size_t releasedControls{};
                for (auto& leash : _leashes) {
                    if (leash->ReleaseControl()) {
                        ++releasedControls;
                    }
                    leash->ResetSimulation();
                }
                if (releasedControls > 0) {
                    SKSE::log::info("Released {} active control state(s) before player positioning", releasedControls);
                }
                break;
            }
            case RE::PositionPlayerEvent::EVENT_TYPE::kFinish: {
                std::size_t teleportedFollowers{};
                for (auto& leash : _leashes) {
                    if (_teleportController.HandlePlayerPositioned(*leash)) {
                        leash->ReleaseControl();
                        ++teleportedFollowers;
                    }
                }
                _positioningPlayer = false;
                _simulationSuspended = true;
                if (teleportedFollowers > 0) {
                    SKSE::log::info("Teleported {} leashed follower(s) after player positioning", teleportedFollowers);
                }
                break;
            }
            default:
                break;
        }
        return RE::BSEventNotifyControl::kContinue;
    }

    // Workaround for factions potentially becoming stale
    RE::BSEventNotifyControl LeashManager::ProcessEvent(const RE::TESObjectLoadedEvent* a_event, RE::BSTEventSource<RE::TESObjectLoadedEvent>*) {
        // Unload events can come from a worker thread. Bethesda finishes loaded events on the main thread, so don't touch any state before this check.
        if (!a_event || !a_event->loaded || !RE::TESForm::LookupByID<RE::Actor>(a_event->formID)) {
            return RE::BSEventNotifyControl::kContinue;
        }

        RefreshActorFactions({a_event->formID});
        return RE::BSEventNotifyControl::kContinue;
    }

    bool LeashManager::Apply(RE::Actor* a_holder, RE::Actor* a_leashed, std::string_view a_parentBone, std::string_view a_leashBoneMatch, float a_minLength, float a_maxLength, bool a_persistent) {
        return ApplyToHand(a_holder, a_leashed, a_parentBone, a_leashBoneMatch, a_minLength, a_maxLength, a_persistent, true);
    }

    bool LeashManager::ApplyToHand(RE::Actor* a_holder, RE::Actor* a_leashed, std::string_view a_parentBone, std::string_view a_leashBoneMatch, float a_minLength, float a_maxLength, bool a_persistent, bool a_rightHand) {
        if (!a_holder || !a_leashed) {
            SKSE::log::warn("ApplyLeashToHand rejected null actor");
            return false;
        }

        LeashDefinition definition{.holderFormID = a_holder->GetFormID(),
            .leashedFormID = a_leashed->GetFormID(),
            .anchor = HandAnchor{.rightHand = a_rightHand},
            .parentBone = std::string(a_parentBone),
            .leashBoneMatch = std::string(a_leashBoneMatch),
            .minLength = a_minLength,
            .maxLength = a_maxLength,
            .persistent = a_persistent};
        return ApplyDefinition(std::move(definition));
    }

    bool LeashManager::ApplyToBone(RE::Actor* a_holder, RE::Actor* a_leashed, std::string_view a_holderBone, std::string_view a_parentBone, std::string_view a_leashBoneMatch, float a_minLength, float a_maxLength,
        bool a_persistent) {
        if (!a_holder || !a_leashed) {
            SKSE::log::warn("ApplyLeashToBone rejected null actor");
            return false;
        }

        LeashDefinition definition{.holderFormID = a_holder->GetFormID(),
            .leashedFormID = a_leashed->GetFormID(),
            .anchor = ActorBoneAnchor{.boneName = std::string(a_holderBone)},
            .parentBone = std::string(a_parentBone),
            .leashBoneMatch = std::string(a_leashBoneMatch),
            .minLength = a_minLength,
            .maxLength = a_maxLength,
            .persistent = a_persistent};
        return ApplyDefinition(std::move(definition));
    }

    bool LeashManager::ApplyAtPosition(RE::Actor* a_leashed, RE::TESObjectCELL* a_anchorCell, float a_x, float a_y, float a_z, std::string_view a_parentBone, std::string_view a_leashBoneMatch, float a_minLength,
        float a_maxLength, bool a_persistent) {
        if (!a_leashed || !a_anchorCell) {
            SKSE::log::warn("ApplyLeashAtPosition rejected null actor or cell");
            return false;
        }

        LeashDefinition definition{.leashedFormID = a_leashed->GetFormID(),
            .anchor = WorldPositionAnchor{.cellFormID = a_anchorCell->GetFormID(), .x = a_x, .y = a_y, .z = a_z},
            .parentBone = std::string(a_parentBone),
            .leashBoneMatch = std::string(a_leashBoneMatch),
            .minLength = a_minLength,
            .maxLength = a_maxLength,
            .persistent = a_persistent};
        return ApplyDefinition(std::move(definition));
    }

    bool LeashManager::ApplyDefinition(LeashDefinition a_definition) {
        auto& definition = a_definition;
        if (!IsValid(definition)) {
            SKSE::log::warn("ApplyLeash rejected invalid arguments for {:08X}->{:08X}", definition.holderFormID, definition.leashedFormID);
            return false;
        }

        std::vector<RE::FormID> affectedActorFormIDs{definition.leashedFormID};
        AddActorFormID(affectedActorFormIDs, definition.holderFormID);
        bool replaced{};
        std::erase_if(_leashes, [&](const auto& a_leash) {
            if (a_leash->GetDefinition().leashedFormID != definition.leashedFormID) {
                return false;
            }
            AddActorFormID(affectedActorFormIDs, a_leash->GetDefinition().holderFormID);
            a_leash->ReleaseControl();
            replaced = true;
            return true;
        });
        const auto leashedFormID = definition.leashedFormID;
        if (replaced) {
            SendLeashEvent("LeashFramework_OnUnleash", "replaced", leashedFormID);
        }
        _leashes.push_back(std::make_unique<LeashInstance>(std::move(definition), _pullController, _recoveryController, _pullPoseController));
        RefreshActorFactions(affectedActorFormIDs);
        SendLeashEvent("LeashFramework_OnLeash", replaced ? "replaced" : "applied", leashedFormID);
        SKSE::log::info("Applied leash to {:08X}", leashedFormID);
        return true;
    }

    bool LeashManager::Disconnect(RE::Actor* a_holder, RE::Actor* a_leashed) {
        if (!a_leashed) {
            return false;
        }

        const auto holderFormID = a_holder ? a_holder->GetFormID() : 0;
        const auto leashedFormID = a_leashed->GetFormID();
        const auto removed = std::erase_if(_leashes, [&](const auto& a_leash) {
            const auto& definition = a_leash->GetDefinition();
            if (definition.holderFormID != holderFormID || definition.leashedFormID != leashedFormID) {
                return false;
            }
            a_leash->ReleaseControl();
            return true;
        });
        if (removed > 0) {
            RefreshActorFactions({holderFormID, leashedFormID});
            SendLeashEvent("LeashFramework_OnUnleash", "disconnected", leashedFormID);
            SKSE::log::info("Disconnected leash {:08X}->{:08X}", holderFormID, leashedFormID);
        }
        return removed > 0;
    }

    bool LeashManager::UnleashAll(RE::Actor* a_actor) {
        if (!a_actor) {
            return false;
        }

        const auto formID = a_actor->GetFormID();
        std::vector<RE::FormID> affectedActorFormIDs{formID};
        std::vector<RE::FormID> unleashedActorFormIDs;
        const auto removed = std::erase_if(_leashes, [&](const auto& a_leash) {
            const auto& definition = a_leash->GetDefinition();
            if (definition.holderFormID != formID && definition.leashedFormID != formID) {
                return false;
            }
            AddActorFormID(affectedActorFormIDs, definition.holderFormID);
            affectedActorFormIDs.push_back(definition.leashedFormID);
            unleashedActorFormIDs.push_back(definition.leashedFormID);
            a_leash->ReleaseControl();
            return true;
        });
        if (removed > 0) {
            RefreshActorFactions(affectedActorFormIDs);
            for (const auto leashedFormID : unleashedActorFormIDs) {
                SendLeashEvent("LeashFramework_OnUnleash", "unleashAll", leashedFormID);
            }
            SKSE::log::info("Unleashed {} leash(es) connected to {:08X}", removed, formID);
        }
        return removed > 0;
    }

    bool LeashManager::IsLeashed(RE::Actor* a_actor) const {
        if (!a_actor) {
            return false;
        }

        const auto formID = a_actor->GetFormID();
        return std::ranges::any_of(_leashes, [&](const auto& a_leash) { return a_leash->GetDefinition().leashedFormID == formID; });
    }

    bool LeashManager::IsLeashHolder(RE::Actor* a_actor) const {
        if (!a_actor) {
            return false;
        }

        const auto formID = a_actor->GetFormID();
        return std::ranges::any_of(_leashes, [&](const auto& a_leash) { return a_leash->GetDefinition().holderFormID == formID; });
    }

    RE::Actor* LeashManager::GetLeashHolder(RE::Actor* a_leashed) const {
        if (!a_leashed) {
            return nullptr;
        }

        const auto formID = a_leashed->GetFormID();
        const auto leash = std::ranges::find_if(_leashes, [&](const auto& a_leash) { return a_leash->GetDefinition().leashedFormID == formID; });
        return leash != _leashes.end() ? RE::TESForm::LookupByID<RE::Actor>((*leash)->GetDefinition().holderFormID) : nullptr;
    }

    std::vector<RE::Actor*> LeashManager::GetLeashedActors(RE::Actor* a_holder) const {
        std::vector<RE::Actor*> actors;
        if (!a_holder) {
            return actors;
        }

        const auto formID = a_holder->GetFormID();
        for (const auto& leash : _leashes) {
            const auto& definition = leash->GetDefinition();
            if (definition.holderFormID == formID) {
                if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(definition.leashedFormID)) {
                    actors.push_back(actor);
                }
            }
        }
        return actors;
    }

    float LeashManager::GetMinLength(RE::Actor* a_leashed) const {
        if (!a_leashed) {
            return -1.0F;
        }

        const auto formID = a_leashed->GetFormID();
        const auto leash = std::ranges::find_if(_leashes, [&](const auto& a_leash) { return a_leash->GetDefinition().leashedFormID == formID; });
        return leash != _leashes.end() ? (*leash)->GetDefinition().minLength : -1.0F;
    }

    float LeashManager::GetMaxLength(RE::Actor* a_leashed) const {
        if (!a_leashed) {
            return -1.0F;
        }

        const auto formID = a_leashed->GetFormID();
        const auto leash = std::ranges::find_if(_leashes, [&](const auto& a_leash) { return a_leash->GetDefinition().leashedFormID == formID; });
        return leash != _leashes.end() ? (*leash)->GetDefinition().maxLength : -1.0F;
    }

    bool LeashManager::SetMinLength(RE::Actor* a_leashed, float a_length) {
        if (!a_leashed || !std::isfinite(a_length) || a_length < 0.0F) {
            return false;
        }

        const auto formID = a_leashed->GetFormID();
        const auto leash = std::ranges::find_if(_leashes, [&](const auto& a_leash) { return a_leash->GetDefinition().leashedFormID == formID; });
        if (leash == _leashes.end() || a_length > (*leash)->GetDefinition().maxLength) {
            return false;
        }
        (*leash)->SetMinLength(a_length);
        return true;
    }

    bool LeashManager::SetMaxLength(RE::Actor* a_leashed, float a_length) {
        if (!a_leashed || !std::isfinite(a_length) || a_length <= 0.0F) {
            return false;
        }

        const auto formID = a_leashed->GetFormID();
        const auto leash = std::ranges::find_if(_leashes, [&](const auto& a_leash) { return a_leash->GetDefinition().leashedFormID == formID; });
        if (leash == _leashes.end() || a_length < (*leash)->GetDefinition().minLength) {
            return false;
        }
        (*leash)->SetMaxLength(a_length);
        return true;
    }

    void LeashManager::HandlePostLoadGame(bool a_succeeded) {
        if (a_succeeded) {
            RefreshLoadedActorFactions();
        }
    }

    void LeashManager::Tick(float a_deltaTime) {
        LF_PROFILE_SCOPE("LeashManager/Tick");
        const auto suspendSimulation = _positioningPlayer || !std::isfinite(a_deltaTime) || a_deltaTime <= 0.0F || (RE::UI::GetSingleton()->GameIsPaused() || RE::Main::GetSingleton()->GetRuntimeData().freezeTime);
        if (suspendSimulation) {
            if (!_simulationSuspended) {
                for (auto& leash : _leashes) {
                    leash->FreezeSimulation();
                }
            }
            _simulationSuspended = true;
            return;
        }

        if (_simulationSuspended) {
            _simulationSuspended = false;
            return;
        }

        const Physics::ActorBodyCollision* actorCollision{};
        if (_settings.collideWithActors && !_leashes.empty()) {
            _actorBodyCollision.Update(_settings.actorBodyCollision);
            actorCollision = &_actorBodyCollision;
        }
        for (auto& leash : _leashes) {
            const auto teleportResult = _teleportController.Update(*leash, a_deltaTime);
            if (teleportResult == LeashTeleportController::UpdateResult::kTeleported) {
                leash->ReleaseControl();
            } else {
                leash->Tick(a_deltaTime, _settings, actorCollision, teleportResult != LeashTeleportController::UpdateResult::kPending);
            }
        }
        if (actorCollision && UI::ModMenu::IsActorCollisionDebugEnabled()) {
            _actorBodyCollision.DrawDebug();
        }
    }

    void LeashManager::ApplyDeferredPoses() {
        LF_PROFILE_SCOPE("LeashManager/ApplyDeferredPoses");
        for (auto& leash : _leashes) {
            leash->ApplyDeferredPose();
        }
    }

    void LeashManager::Clear() {
        std::vector<RE::FormID> affectedActorFormIDs;
        affectedActorFormIDs.reserve(_leashes.size() * 2);
        for (const auto& leash : _leashes) {
            const auto& definition = leash->GetDefinition();
            AddActorFormID(affectedActorFormIDs, definition.holderFormID);
            affectedActorFormIDs.push_back(definition.leashedFormID);
            leash->ReleaseControl();
        }
        _leashes.clear();
        RefreshActorFactions(affectedActorFormIDs);
        _actorBodyCollision.Clear();
    }

    std::vector<LeashDefinition> LeashManager::GetDefinitions() const {
        std::vector<LeashDefinition> definitions;
        definitions.reserve(_leashes.size());
        for (const auto& leash : _leashes) {
            definitions.push_back(leash->GetDefinition());
        }
        return definitions;
    }

    std::vector<LeashDefinition> LeashManager::GetPersistentDefinitions() const {
        std::vector<LeashDefinition> definitions;
        definitions.reserve(_leashes.size());
        for (const auto& leash : _leashes) {
            if (leash->GetDefinition().persistent) {
                definitions.push_back(leash->GetDefinition());
            }
        }
        return definitions;
    }

    void LeashManager::LoadPersistentDefinitions(std::vector<LeashDefinition> a_definitions) {
        if (_pullController.DiagnosticsEnabled()) {
            SKSE::log::info("[PullDiag] LoadPersistentDefinitions begin definitions={}", a_definitions.size());
        }
        std::vector<RE::FormID> affectedActorFormIDs;
        std::vector<RE::FormID> loadedActorFormIDs;
        loadedActorFormIDs.reserve(a_definitions.size());
        std::erase_if(_leashes, [&](const auto& a_leash) {
            const auto& definition = a_leash->GetDefinition();
            if (!definition.persistent) {
                return false;
            }
            AddActorFormID(affectedActorFormIDs, definition.holderFormID);
            affectedActorFormIDs.push_back(definition.leashedFormID);
            a_leash->ReleaseControl();
            return true;
        });

        std::size_t loaded{};
        for (auto& definition : a_definitions) {
            if (!IsValid(definition) || !definition.persistent || !AnchorFormsExist(definition) || !RE::TESForm::LookupByID<RE::Actor>(definition.leashedFormID)) {
                continue;
            }
            std::erase_if(_leashes, [&](const auto& a_leash) {
                if (a_leash->GetDefinition().leashedFormID != definition.leashedFormID) {
                    return false;
                }
                AddActorFormID(affectedActorFormIDs, a_leash->GetDefinition().holderFormID);
                a_leash->ReleaseControl();
                return true;
            });
            AddActorFormID(affectedActorFormIDs, definition.holderFormID);
            affectedActorFormIDs.push_back(definition.leashedFormID);
            const auto leashedFormID = definition.leashedFormID;
            _leashes.push_back(std::make_unique<LeashInstance>(std::move(definition), _pullController, _recoveryController, _pullPoseController));
            loadedActorFormIDs.push_back(leashedFormID);
            ++loaded;
        }
        RefreshActorFactions(affectedActorFormIDs);
        for (const auto leashedFormID : loadedActorFormIDs) {
            SendLeashEvent("LeashFramework_OnLeash", "loaded", leashedFormID);
        }
        SKSE::log::info("Loaded {} persistent leash(es)", loaded);
    }

    bool LeashManager::IsValid(const LeashDefinition& a_definition) {
        const auto validAnchor = std::visit(
            [&](const auto& a_anchor) {
                using Anchor = std::decay_t<decltype(a_anchor)>;
                if constexpr (std::is_same_v<Anchor, HandAnchor>) {
                    return a_definition.holderFormID != 0;
                } else if constexpr (std::is_same_v<Anchor, ActorBoneAnchor>) {
                    return a_definition.holderFormID != 0 && !a_anchor.boneName.empty();
                } else {
                    return a_definition.holderFormID == 0 && a_anchor.cellFormID != 0 && std::isfinite(a_anchor.x) && std::isfinite(a_anchor.y) && std::isfinite(a_anchor.z);
                }
            },
            a_definition.anchor);
        return validAnchor && a_definition.leashedFormID != 0 && a_definition.holderFormID != a_definition.leashedFormID && !a_definition.parentBone.empty() && !a_definition.leashBoneMatch.empty() &&
               std::isfinite(a_definition.minLength) && std::isfinite(a_definition.maxLength) && a_definition.minLength >= 0.0F && a_definition.maxLength >= a_definition.minLength && a_definition.maxLength > 0.0F;
    }

    void LeashManager::RefreshActorFactions(const std::vector<RE::FormID>& a_actorFormIDs) {
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        auto* leashedFaction = dataHandler->LookupForm<RE::TESFaction>(kLeashedFactionFormID, kPluginName);
        auto* leasherFaction = dataHandler->LookupForm<RE::TESFaction>(kLeasherFactionFormID, kPluginName);
        if (!leashedFaction || !leasherFaction) {
            SKSE::log::error("Failed to resolve leash factions from {}", kPluginName);
            return;
        }

        std::vector<RE::FormID> uniqueFormIDs;
        uniqueFormIDs.reserve(a_actorFormIDs.size());
        for (const auto formID : a_actorFormIDs) {
            if (formID != 0 && std::ranges::find(uniqueFormIDs, formID) == uniqueFormIDs.end()) {
                uniqueFormIDs.push_back(formID);
            }
        }

        for (const auto formID : uniqueFormIDs) {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(formID);
            if (!actor) {
                continue;
            }

            const auto isLeashed = std::ranges::any_of(_leashes, [&](const auto& a_leash) { return a_leash->GetDefinition().leashedFormID == formID; });
            const auto isLeasher = std::ranges::any_of(_leashes, [&](const auto& a_leash) { return a_leash->GetDefinition().holderFormID == formID; });
            if (isLeashed && !actor->IsInFaction(leashedFaction)) {
                actor->AddToFaction(leashedFaction, 0);
            } else if (!isLeashed && actor->IsInFaction(leashedFaction)) {
                actor->RemoveFromFaction(leashedFaction);
            }
            if (isLeasher && !actor->IsInFaction(leasherFaction)) {
                actor->AddToFaction(leasherFaction, 0);
            } else if (!isLeasher && actor->IsInFaction(leasherFaction)) {
                actor->RemoveFromFaction(leasherFaction);
            }
        }
    }

    void LeashManager::RefreshLoadedActorFactions() {
        std::vector<RE::FormID> actorFormIDs;
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            actorFormIDs.push_back(player->GetFormID());
        }
        if (auto* world = RE::TES::GetSingleton()) {
            world->ForEachReference([&](RE::TESObjectREFR* a_reference) {
                if (auto* actor = a_reference ? a_reference->As<RE::Actor>() : nullptr) {
                    actorFormIDs.push_back(actor->GetFormID());
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }
        RefreshActorFactions(actorFormIDs);
    }
}  // namespace LeashFramework
