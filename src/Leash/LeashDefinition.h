#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace LeashFramework {
    struct HandAnchor {
        bool rightHand{true};
    };

    struct ActorBoneAnchor {
        std::string boneName{};
    };

    struct WorldPositionAnchor {
        std::uint32_t cellFormID{};
        float x{};
        float y{};
        float z{};
    };

    using LeashAnchorDefinition = std::variant<HandAnchor, ActorBoneAnchor, WorldPositionAnchor>;

    struct LeashDefinition {
        std::uint32_t holderFormID{};
        std::uint32_t leashedFormID{};
        LeashAnchorDefinition anchor{};
        std::string parentBone{};
        std::string leashBoneMatch{};
        float minLength{};
        float maxLength{};
        bool persistent{};
    };
}  // namespace LeashFramework
