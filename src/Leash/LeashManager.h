#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "../Animation/PullPoseController.h"
#include "../PCH.h"
#include "../Physics/ActorBodyCollision.h"
#include "../Physics/SimulationSettings.h"
#include "../Recovery/ForcedRecoveryController.h"
#include "LeashDefinition.h"
#include "LeashTeleportController.h"
#include "PullController.h"

namespace LeashFramework {
    class LeashInstance;

    // Runtime state is main-thread owned. Any off-thread callback must return or marshal before touching it.
    class LeashManager : public RE::BSTEventSink<RE::PositionPlayerEvent>, public RE::BSTEventSink<RE::TESObjectLoadedEvent> {
    public:
        ~LeashManager();
        [[nodiscard]] static LeashManager& GetSingleton();

        [[nodiscard]] bool Apply(RE::Actor* a_holder, RE::Actor* a_leashed, std::string_view a_parentBone, std::string_view a_leashBoneMatch, float a_minLength, float a_maxLength, bool a_persistent);
        [[nodiscard]] bool ApplyToHand(RE::Actor* a_holder, RE::Actor* a_leashed, std::string_view a_parentBone, std::string_view a_leashBoneMatch, float a_minLength, float a_maxLength, bool a_persistent, bool a_rightHand);
        [[nodiscard]] bool ApplyToBone(RE::Actor* a_holder, RE::Actor* a_leashed, std::string_view a_holderBone, float a_offsetX, float a_offsetY, float a_offsetZ, std::string_view a_parentBone, std::string_view a_leashBoneMatch, float a_minLength, float a_maxLength, bool a_persistent);
        [[nodiscard]] bool ApplyHolderOwnedLeashToBone(RE::Actor* a_holder, RE::Actor* a_leashed, std::string_view a_leashedBone, float a_offsetX, float a_offsetY, float a_offsetZ, std::string_view a_parentBone, std::string_view a_leashBoneMatch, float a_minLength, float a_maxLength, bool a_persistent, std::int32_t a_closedHand);
        [[nodiscard]] bool ApplyAtPosition(RE::Actor* a_leashed, RE::TESObjectCELL* a_anchorCell, float a_x, float a_y, float a_z, std::string_view a_parentBone, std::string_view a_leashBoneMatch, float a_minLength,
            float a_maxLength, bool a_persistent);
        [[nodiscard]] bool Disconnect(RE::Actor* a_holder, RE::Actor* a_leashed);
        [[nodiscard]] bool UnleashAll(RE::Actor* a_actor);
        [[nodiscard]] bool IsLeashed(RE::Actor* a_actor) const;
        [[nodiscard]] bool IsLeashHolder(RE::Actor* a_actor) const;
        [[nodiscard]] RE::Actor* GetLeashHolder(RE::Actor* a_leashed) const;
        [[nodiscard]] std::vector<RE::Actor*> GetLeashedActors(RE::Actor* a_holder) const;
        [[nodiscard]] float GetMinLength(RE::Actor* a_leashed) const;
        [[nodiscard]] float GetMaxLength(RE::Actor* a_leashed) const;
        [[nodiscard]] bool SetMinLength(RE::Actor* a_leashed, float a_length);
        [[nodiscard]] bool SetMaxLength(RE::Actor* a_leashed, float a_length);
        [[nodiscard]] Physics::SimulationSettings GetSimulationSettings() const;
        void SetSimulationSettings(Physics::SimulationSettings a_settings);
        [[nodiscard]] Recovery::ForcedRecoverySettings GetRecoverySettings() const;
        void SetRecoverySettings(Recovery::ForcedRecoverySettings a_settings);
        [[nodiscard]] LeashTeleportSettings GetTeleportSettings() const;
        void SetTeleportSettings(LeashTeleportSettings a_settings);
        [[nodiscard]] Animation::PullPoseSettings GetPullPoseSettings() const;
        void SetPullPoseSettings(Animation::PullPoseSettings a_settings);
        [[nodiscard]] bool PullDiagnosticsEnabled() const;
        void SetPullDiagnosticsEnabled(bool a_enabled);
        void HandlePreLoadGame();
        [[nodiscard]] std::size_t PrepareForSave();
        void HandlePostLoadGame(bool a_succeeded);
        void Tick(float a_deltaTime);
        void ApplyDeferredPoses();
        void Clear();

        [[nodiscard]] std::vector<LeashDefinition> GetDefinitions() const;
        [[nodiscard]] std::vector<LeashDefinition> GetPersistentDefinitions() const;
        void LoadPersistentDefinitions(std::vector<LeashDefinition> a_definitions);

    private:
        LeashManager();
        RE::BSEventNotifyControl ProcessEvent(const RE::PositionPlayerEvent* a_event, RE::BSTEventSource<RE::PositionPlayerEvent>* a_eventSource) override;
        RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent* a_event, RE::BSTEventSource<RE::TESObjectLoadedEvent>* a_eventSource) override;
        [[nodiscard]] bool ApplyDefinition(LeashDefinition a_definition);
        [[nodiscard]] static bool IsValid(const LeashDefinition& a_definition);
        void RefreshActorFactions(const std::vector<RE::FormID>& a_actorFormIDs);
        void RefreshLoadedActorFactions();

        PullController _pullController;
        Recovery::ForcedRecoveryController _recoveryController;
        LeashTeleportController _teleportController;
        Animation::PullPoseController _pullPoseController;
        std::vector<std::unique_ptr<LeashInstance>> _leashes;
        Physics::ActorBodyCollision _actorBodyCollision;
        Physics::SimulationSettings _settings;
        bool _simulationSuspended{};
        bool _positioningPlayer{};
    };
}  // namespace LeashFramework
