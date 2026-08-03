#pragma once

#include <array>
#include <optional>

#include "../PCH.h"
#include "LeashDefinition.h"

namespace LeashFramework {
    class LeashAnchor {
    public:
        enum class BindResult { kFailed, kUnchanged, kChanged };

        struct Sample {
            RE::NiPoint3 position;
            RE::NiPoint3 pullGoal;
            RE::TESObjectCELL* cell{};
        };

        explicit LeashAnchor(const LeashDefinition& a_definition);

        [[nodiscard]] BindResult Bind();
        [[nodiscard]] std::optional<Sample> GetSample() const;
        void ApplyPose();

    private:
        [[nodiscard]] BindResult Bind(const HandAnchor& a_anchor);
        [[nodiscard]] BindResult Bind(const ActorBoneAnchor& a_anchor);
        [[nodiscard]] BindResult Bind(const WorldPositionAnchor& a_anchor);
        [[nodiscard]] RE::Actor* ResolveHolder();
        void Reset();

        const LeashDefinition& _definition;
        RE::ActorHandle _holder;
        RE::NiPointer<RE::NiAVObject> _boundRoot;
        RE::NiPointer<RE::NiAVObject> _anchorNode;
        RE::NiPointer<RE::NiAVObject> _hand;
        std::array<std::array<RE::NiPointer<RE::NiAVObject>, 3>, 5> _fingers{};
        RE::TESObjectCELL* _worldCell{};
        bool _bindingWarningLogged{};
    };
}  // namespace LeashFramework
