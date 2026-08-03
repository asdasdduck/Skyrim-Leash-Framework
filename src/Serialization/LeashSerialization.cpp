#include "LeashSerialization.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <glaze/glaze.hpp>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "../Leash/LeashManager.h"
#include "../PCH.h"

template <>
struct glz::meta<LeashFramework::LeashAnchorDefinition> {
    static constexpr std::string_view tag = "type";
    static constexpr auto ids = std::array{"hand", "actorBone", "worldPosition"};
};

namespace LeashFramework::Serialization {
    struct SavedState {
        std::vector<LeashDefinition> leashes;
    };

    namespace {
        constexpr std::uint32_t MakeRecordType(char a_first, char a_second, char a_third, char a_fourth) {
            return static_cast<std::uint32_t>(a_first) | static_cast<std::uint32_t>(a_second) << 8 | static_cast<std::uint32_t>(a_third) << 16 | static_cast<std::uint32_t>(a_fourth) << 24;
        }

        constexpr auto kSerializationID = MakeRecordType('L', 'F', 'W', 'K');
        constexpr auto kDataRecord = MakeRecordType('L', 'S', 'H', 'S');
        constexpr std::uint32_t kRecordVersion = 3;
        constexpr std::uint32_t kMaximumRecordSize = 16U * 1024U * 1024U;

        void DiscardRecord(SKSE::SerializationInterface* a_interface, std::uint32_t a_length) {
            std::array<std::byte, 4096> buffer{};
            while (a_length > 0) {
                const auto amount = (std::min)(a_length, static_cast<std::uint32_t>(buffer.size()));
                const auto read = a_interface->ReadRecordData(buffer.data(), amount);
                if (read == 0) {
                    return;
                }
                a_length -= read;
            }
        }

        void Save(SKSE::SerializationInterface* a_interface) {
            auto& manager = LeashManager::GetSingleton();
            SavedState state{.leashes = manager.GetPersistentDefinitions()};
            if (manager.PullDiagnosticsEnabled()) {
                SKSE::log::info("[PullDiag] serialization Save persistentLeashes={}", state.leashes.size());
            }
            std::string json;
            if (const auto error = glz::write_json(state, json); error) {
                SKSE::log::error("Failed to serialize persistent leashes");
                return;
            }
            if (json.size() > std::numeric_limits<std::uint32_t>::max() || !a_interface->WriteRecord(kDataRecord, kRecordVersion, json.data(), static_cast<std::uint32_t>(json.size()))) {
                SKSE::log::error("Failed to write persistent leash record");
            }
        }

        void Load(SKSE::SerializationInterface* a_interface) {
            std::uint32_t type{};
            std::uint32_t version{};
            std::uint32_t length{};
            while (a_interface->GetNextRecordInfo(type, version, length)) {
                if (type != kDataRecord || version != kRecordVersion || length > kMaximumRecordSize) {
                    SKSE::log::warn("Ignoring unsupported leash record type={:08X}, version={}, length={}", type, version, length);
                    DiscardRecord(a_interface, length);
                    continue;
                }

                std::string json(length, '\0');
                if (a_interface->ReadRecordData(json.data(), length) != length) {
                    SKSE::log::error("Failed to read persistent leash record");
                    continue;
                }

                SavedState state;
                if (const auto error = glz::read_json(state, json); error) {
                    SKSE::log::error("Failed to parse persistent leash record: {}", glz::format_error(error, json));
                    continue;
                }

                std::vector<LeashDefinition> resolved;
                resolved.reserve(state.leashes.size());
                for (auto& definition : state.leashes) {
                    auto leashedFormID = definition.leashedFormID;
                    if (!a_interface->ResolveFormID(leashedFormID, definition.leashedFormID)) {
                        SKSE::log::warn("Dropping persistent leash with unresolved actor form");
                        continue;
                    }
                    if (definition.holderFormID != 0) {
                        auto holderFormID = definition.holderFormID;
                        if (!a_interface->ResolveFormID(holderFormID, definition.holderFormID)) {
                            SKSE::log::warn("Dropping persistent leash with unresolved holder form");
                            continue;
                        }
                    }
                    if (auto* anchor = std::get_if<WorldPositionAnchor>(&definition.anchor)) {
                        auto cellFormID = anchor->cellFormID;
                        if (!a_interface->ResolveFormID(cellFormID, anchor->cellFormID)) {
                            SKSE::log::warn("Dropping persistent world leash with unresolved cell form");
                            continue;
                        }
                    }
                    resolved.push_back(std::move(definition));
                }
                LeashManager::GetSingleton().LoadPersistentDefinitions(std::move(resolved));
            }
        }

        void Revert(SKSE::SerializationInterface*) { LeashManager::GetSingleton().Clear(); }
    }  // namespace

    bool Install() {
        const auto* serialization = SKSE::GetSerializationInterface();
        if (!serialization) {
            SKSE::log::error("Wtf serialization interface is unavailable");
            return false;
        }

        serialization->SetUniqueID(kSerializationID);
        serialization->SetSaveCallback(Save);
        serialization->SetLoadCallback(Load);
        serialization->SetRevertCallback(Revert);
        SKSE::log::info("Registered leash serialization");
        return true;
    }
}  // namespace LeashFramework::Serialization
