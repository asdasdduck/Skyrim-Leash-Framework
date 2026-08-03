#include "LeashPapyrus.h"

#include "../Leash/LeashManager.h"
#include "../PCH.h"

namespace LeashFramework::Papyrus {
    namespace {
        constexpr std::string_view kScriptName = "LeashFramework";

        bool ApplyLeash(RE::StaticFunctionTag*, RE::Actor* a_holder, RE::Actor* a_leashed, RE::BSFixedString a_parentBone, RE::BSFixedString a_leashBoneMatch, float a_minLength, float a_maxLength, bool a_persistent) {
            return LeashManager::GetSingleton().Apply(a_holder, a_leashed, a_parentBone, a_leashBoneMatch, a_minLength, a_maxLength, a_persistent);
        }

        bool ApplyLeashToHand(RE::StaticFunctionTag*, RE::Actor* a_holder, RE::Actor* a_leashed, RE::BSFixedString a_parentBone, RE::BSFixedString a_leashBoneMatch, float a_minLength, float a_maxLength,
            bool a_persistent, bool a_rightHand) {
            return LeashManager::GetSingleton().ApplyToHand(a_holder, a_leashed, a_parentBone, a_leashBoneMatch, a_minLength, a_maxLength, a_persistent, a_rightHand);
        }

        bool ApplyLeashToBone(RE::StaticFunctionTag*, RE::Actor* a_holder, RE::Actor* a_leashed, RE::BSFixedString a_holderBone, RE::BSFixedString a_parentBone, RE::BSFixedString a_leashBoneMatch, float a_minLength,
            float a_maxLength, bool a_persistent) {
            return LeashManager::GetSingleton().ApplyToBone(a_holder, a_leashed, a_holderBone, a_parentBone, a_leashBoneMatch, a_minLength, a_maxLength, a_persistent);
        }

        bool ApplyLeashAtPosition(RE::StaticFunctionTag*, RE::Actor* a_leashed, RE::TESObjectCELL* a_anchorCell, float a_x, float a_y, float a_z, RE::BSFixedString a_parentBone, RE::BSFixedString a_leashBoneMatch,
            float a_minLength, float a_maxLength, bool a_persistent) {
            return LeashManager::GetSingleton().ApplyAtPosition(a_leashed, a_anchorCell, a_x, a_y, a_z, a_parentBone, a_leashBoneMatch, a_minLength, a_maxLength, a_persistent);
        }

        bool DisconnectLeash(RE::StaticFunctionTag*, RE::Actor* a_holder, RE::Actor* a_leashed) { return LeashManager::GetSingleton().Disconnect(a_holder, a_leashed); }

        bool UnleashAll(RE::StaticFunctionTag*, RE::Actor* a_actor) { return LeashManager::GetSingleton().UnleashAll(a_actor); }

        bool IsLeashed(RE::StaticFunctionTag*, RE::Actor* a_actor) { return LeashManager::GetSingleton().IsLeashed(a_actor); }

        bool IsLeashHolder(RE::StaticFunctionTag*, RE::Actor* a_actor) { return LeashManager::GetSingleton().IsLeashHolder(a_actor); }

        RE::Actor* GetLeashHolder(RE::StaticFunctionTag*, RE::Actor* a_leashed) { return LeashManager::GetSingleton().GetLeashHolder(a_leashed); }

        std::vector<RE::Actor*> GetLeashedActors(RE::StaticFunctionTag*, RE::Actor* a_holder) { return LeashManager::GetSingleton().GetLeashedActors(a_holder); }

        float GetMinLeashLength(RE::StaticFunctionTag*, RE::Actor* a_leashed) { return LeashManager::GetSingleton().GetMinLength(a_leashed); }

        float GetMaxLeashLength(RE::StaticFunctionTag*, RE::Actor* a_leashed) { return LeashManager::GetSingleton().GetMaxLength(a_leashed); }

        bool SetMinLeashLength(RE::StaticFunctionTag*, RE::Actor* a_leashed, float a_length) { return LeashManager::GetSingleton().SetMinLength(a_leashed, a_length); }

        bool SetMaxLeashLength(RE::StaticFunctionTag*, RE::Actor* a_leashed, float a_length) { return LeashManager::GetSingleton().SetMaxLength(a_leashed, a_length); }
    }  // namespace

    bool Register(RE::BSScript::IVirtualMachine* a_vm) {
        if (!a_vm) {
            return false;
        }

        a_vm->RegisterFunction("ApplyLeash", kScriptName, ApplyLeash);
        a_vm->RegisterFunction("ApplyLeashToHand", kScriptName, ApplyLeashToHand);
        a_vm->RegisterFunction("ApplyLeashToBone", kScriptName, ApplyLeashToBone);
        a_vm->RegisterFunction("ApplyLeashAtPosition", kScriptName, ApplyLeashAtPosition);
        a_vm->RegisterFunction("DisconnectLeash", kScriptName, DisconnectLeash);
        a_vm->RegisterFunction("UnleashAll", kScriptName, UnleashAll);
        a_vm->RegisterFunction("IsLeashed", kScriptName, IsLeashed);
        a_vm->RegisterFunction("IsLeashHolder", kScriptName, IsLeashHolder);
        a_vm->RegisterFunction("GetLeashHolder", kScriptName, GetLeashHolder);
        a_vm->RegisterFunction("GetLeashedActors", kScriptName, GetLeashedActors);
        a_vm->RegisterFunction("GetMinLeashLength", kScriptName, GetMinLeashLength);
        a_vm->RegisterFunction("GetMaxLeashLength", kScriptName, GetMaxLeashLength);
        a_vm->RegisterFunction("SetMinLeashLength", kScriptName, SetMinLeashLength);
        a_vm->RegisterFunction("SetMaxLeashLength", kScriptName, SetMaxLeashLength);
        SKSE::log::info("Registered {} Papyrus API", kScriptName);
        return true;
    }
}  // namespace LeashFramework::Papyrus
