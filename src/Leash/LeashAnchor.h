#pragma once

#include <array>
#include <optional>
#include <string_view>

#include "../PCH.h"
#include "LeashDefinition.h"

namespace LeashFramework {
    class LeashAnchor {
    public:
        enum class BindResult { kFailed, kUnchanged, kChanged };

        struct Sample {
            RE::NiPoint3 position;
            RE::TESObjectCELL* cell{};
            RE::NiAVObject* poseReference{};
        };

        explicit LeashAnchor(const LeashDefinition& a_definition);

        [[nodiscard]] BindResult Bind(RE::Actor* a_attachmentActor, RE::Actor* a_holder);
        [[nodiscard]] std::optional<Sample> GetSample(RE::Actor* a_attachmentActor) const;
        void ApplyPose();

    private:
        struct HandBinding {
            RE::NiPointer<RE::NiAVObject> root;
            RE::NiPointer<RE::NiAVObject> hand;
            std::array<std::array<RE::NiPointer<RE::NiAVObject>, 3>, 5> fingers{};
        };

        [[nodiscard]] BindResult Bind(const HandAnchor& a_anchor, RE::Actor* a_attachmentActor);
        [[nodiscard]] BindResult Bind(const ActorBoneAnchor& a_anchor, RE::Actor* a_attachmentActor);
        [[nodiscard]] BindResult Bind(const WorldPositionAnchor& a_anchor);
        [[nodiscard]] BindResult BindHand(HandBinding& a_binding, RE::Actor* a_actor, RE::FormID a_actorFormID, bool a_rightHand, bool& a_warningLogged, std::string_view a_role);
        void BindHolderGrip(RE::Actor* a_holder);
        void ApplyHandPose(const HandBinding& a_binding, bool a_rightHand) const;
        void ResetHand(HandBinding& a_binding);

        const LeashDefinition& _definition;
        RE::NiPointer<RE::NiAVObject> _boundRoot;
        RE::NiPointer<RE::NiAVObject> _anchorNode;
        HandBinding _attachmentHand;
        HandBinding _holderGrip;
        RE::TESObjectCELL* _worldCell{};
        bool _bindingWarningLogged{};
        bool _gripWarningLogged{};
    };
}  // namespace LeashFramework
