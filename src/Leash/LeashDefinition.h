#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace LeashFramework {
    enum class LeashMeshOwner : std::uint8_t { kLeashed, kHolder };

    enum class ClosedHand : std::uint8_t { kNone, kRight, kLeft };

    struct HandAnchor {
        bool rightHand{true};
    };

    struct ActorBoneAnchor {
        std::string boneName{};
        float offsetX{};
        float offsetY{};
        float offsetZ{};
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
        LeashMeshOwner meshOwner{LeashMeshOwner::kLeashed};
        LeashAnchorDefinition anchor{};
        ClosedHand closedHand{ClosedHand::kNone};
        std::string parentBone{};
        std::string leashBoneMatch{};
        float minLength{};
        float maxLength{};
        bool persistent{};
    };
}  // namespace LeashFramework
