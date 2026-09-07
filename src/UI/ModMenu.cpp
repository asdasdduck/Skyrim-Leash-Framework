#include "ModMenu.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <format>
#include <glaze/glaze.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

#include "../../include/SKSEMenuFramework.h"
#include "../Hooks/FrameHook.h"
#include "../Leash/LeashManager.h"
#include "../Leash/LeashTeleportController.h"
#include "../PCH.h"
#include "../Recovery/ForcedRecoveryController.h"
#include "DebugOverlay.h"
#include "MenuLayout.h"

template <>
struct glz::meta<RE::NiPoint3> {
    using T = RE::NiPoint3;
    static constexpr auto value = glz::object(&T::x, &T::y, &T::z);
};

// Todo: Refactor blocks of logic out of here to keep it tidy
namespace LeashFramework::UI::ModMenu {
    struct ArmorEntry {
        char modName[128];
        char formID[64];

        struct glaze {
            using T = ArmorEntry;
            static constexpr auto value = glz::object(&T::modName, &T::formID);
        };
    };

    struct DebugSettings {
        char parentBone[128]{"NPC Spine2 [Spn2]"};
        char leashBoneMatch[128]{"Leash1_1"};
        float minLength{200.0F};
        float maxLength{300.0F};
        RE::NiPoint3 attachmentOffset{};
        bool holderOwnsLeash{};
        int closedHand{};
        bool persistent{true};
        bool enablePullDiagnostics{};
        std::array<ArmorEntry, 5> armorEntries{ArmorEntry{"Leash.esm", "800 #Body Rope"}, ArmorEntry{"Leash.esm", "804 #Neck Rope"}, ArmorEntry{"Leash.esm", "806 #Neck Chain"},
            ArmorEntry{"Leash.esm", "32ce #Magic Rope"}, ArmorEntry{"Leash.esm", "d69 #Leasher-held shield Leash"}};

        struct glaze {
            using T = DebugSettings;
            static constexpr auto value = glz::object(
                &T::parentBone, &T::leashBoneMatch, &T::minLength, &T::maxLength, &T::attachmentOffset, &T::holderOwnsLeash, &T::closedHand, &T::persistent, &T::enablePullDiagnostics, &T::armorEntries);
        };
    };

    struct ModMenuSettings {
        Hooks::FrameHookSettings frameHook;
        Physics::SimulationSettings simulation;
        Animation::PullPoseSettings pullPose;
        LocomotionSettings locomotion;
        Recovery::ForcedRecoverySettings recovery;
        LeashTeleportSettings teleport;
        DebugSettings debug;
    };

    namespace {
        enum class DebugAnchorType : std::uint8_t { kRightHand, kLeftHand, kActorBone, kWorldPosition };

        struct ActorOption {
            std::uint32_t formID{};
            std::string label;
            float distance{};
        };

        struct SkeletonNode {
            std::string name;
            std::string displayName;
            std::vector<SkeletonNode> children;
            bool likelyCandidate{};
        };

        constexpr auto kSettingsPath = "Data/SKSE/Plugins/LeashFramework.json";
        constexpr std::string_view kSMPBoneMarker = "hdtSSEPhysics_";  // Used to help locate non-vanilla bones. Idc
        constexpr std::array kDebugAnchorLabels{"Right hand", "Left hand", "Actor bone", "World position"};
        constexpr std::array kMeshOwnerLabels{"Leashed actor", "Leasher"};
        constexpr std::array kClosedHandLabels{"None", "Right", "Left"};

        std::vector<ActorOption> actorOptions;
        std::uint32_t selectedHolder{};
        std::uint32_t selectedLeashed{};
        std::uint32_t selectedArmor{};
        DebugAnchorType selectedAnchorType{DebugAnchorType::kRightHand};
        char selectedAttachmentBone[128]{};
        RE::NiPoint3 selectedWorldPosition{};
        std::uint32_t selectedWorldCellFormID{};
        bool actorsLoaded{};
        bool actorCollisionDebugEnabled{};
        DebugSettings debugSettings;
        std::string activeStatus;
        std::string skeletonStatus;
        std::string applyStatus;
        std::string armorStatus;
        std::string settingsJson;
        std::vector<SkeletonNode> skeletonDump;
        std::string skeletonDumpActor;
        char skeletonFilter[128]{};
        bool modMenuOpen{};
        SKSEMenuFramework::Model::Event* menuEvent{};

        void LoadSettings() {
            std::error_code fileError;
            if (!std::filesystem::exists(kSettingsPath, fileError)) {
                if (fileError) {
                    SKSE::log::error("Could not inspect menu settings file: {}", fileError.message());
                }
                return;
            }

            ModMenuSettings settings;
            settingsJson.clear();
            if (const auto error = glz::read_file_json(settings, kSettingsPath, settingsJson); error) {
                SKSE::log::error("Failed to load menu settings: {}", glz::format_error(error, settingsJson));
                return;
            }

            auto& manager = LeashManager::GetSingleton();
            manager.SetSimulationSettings(settings.simulation);
            manager.SetPullPoseSettings(settings.pullPose);
            manager.SetLocomotionSettings(settings.locomotion);
            manager.SetRecoverySettings(settings.recovery);
            manager.SetTeleportSettings(settings.teleport);
            Hooks::FrameHook::SetSettings(settings.frameHook);
            debugSettings = settings.debug;
            if (debugSettings.closedHand != 1 && debugSettings.closedHand != 2) {
                debugSettings.closedHand = 0;
            }
            manager.SetPullDiagnosticsEnabled(debugSettings.enablePullDiagnostics);
            SKSE::log::info("Loaded menu settings");
        }

        void SaveSettings() {
            auto& manager = LeashManager::GetSingleton();
            ModMenuSettings settings{.frameHook = Hooks::FrameHook::GetSettings(),
                .simulation = manager.GetSimulationSettings(),
                .pullPose = manager.GetPullPoseSettings(),
                .locomotion = manager.GetLocomotionSettings(),
                .recovery = manager.GetRecoverySettings(),
                .teleport = manager.GetTeleportSettings(),
                .debug = debugSettings};

            settingsJson.clear();
            if (const auto error = glz::write_file_json(settings, kSettingsPath, settingsJson); error) {
                SKSE::log::error("Failed to save menu settings: {}", glz::format_error(error, settingsJson));
                return;
            }
            SKSE::log::info("Saved menu settings");
        }

        void __stdcall OnMenuEvent(SKSEMenuFramework::Model::EventType a_eventType) {
            if (a_eventType == SKSEMenuFramework::Model::kOpenMenu) {
                modMenuOpen = true;
            } else if (a_eventType == SKSEMenuFramework::Model::kCloseMenu && modMenuOpen) {
                modMenuOpen = false;
                SaveSettings();
            }
        }

        [[nodiscard]] std::string DescribeActor(RE::Actor* a_actor) {
            if (!a_actor) {
                return "Unavailable";
            }

            const auto* name = a_actor->GetDisplayFullName();
            return std::format("{} [{:08X}]", name && name[0] != '\0' ? name : "Unnamed actor", a_actor->GetFormID());
        }

        [[nodiscard]] std::string DescribeActor(std::uint32_t a_formID) {
            if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_formID)) {
                return DescribeActor(actor);
            }
            return std::format("Unavailable [{:08X}]", a_formID);
        }

        void RefreshActors() {
            actorOptions.clear();
            std::unordered_set<std::uint32_t> formIDs;
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto addActor = [&](RE::Actor* a_actor) {
                if (a_actor && formIDs.insert(a_actor->GetFormID()).second) {
                    const auto distance = player ? player->GetDistance(a_actor) : 0.0F;
                    actorOptions.push_back({a_actor->GetFormID(), DescribeActor(a_actor), distance});
                }
            };

            addActor(player);
            if (auto* processLists = RE::ProcessLists::GetSingleton()) {
                processLists->ForEachHighActor([&](RE::Actor* a_actor) {
                    addActor(a_actor);
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            }

            const auto playerFormID = player ? player->GetFormID() : 0;
            std::ranges::sort(actorOptions, [&](const ActorOption& a_left, const ActorOption& a_right) {
                if (a_left.formID == playerFormID) {
                    return a_right.formID != playerFormID;
                }
                if (a_right.formID == playerFormID) {
                    return false;
                }
                if (a_left.distance != a_right.distance) {
                    return a_left.distance < a_right.distance;
                }
                return a_left.label < a_right.label;
            });

            const auto contains = [](std::uint32_t a_formID) { return std::ranges::any_of(actorOptions, [&](const ActorOption& a_option) { return a_option.formID == a_formID; }); };
            if (!contains(selectedHolder)) {
                selectedHolder = playerFormID != 0 ? playerFormID : actorOptions.empty() ? 0 : actorOptions.front().formID;
            }
            if (!contains(selectedLeashed)) {
                const auto option = std::ranges::find_if(actorOptions, [](const ActorOption& a_actor) { return a_actor.formID != selectedHolder; });
                selectedLeashed = option != actorOptions.end() ? option->formID : 0;
            }
            actorsLoaded = true;
        }

        void RenderActorDropdown(const char* a_label, std::uint32_t& a_selectedFormID) {
            const auto selected = std::ranges::find_if(actorOptions, [&](const ActorOption& a_actor) { return a_actor.formID == a_selectedFormID; });
            const char* preview = selected != actorOptions.end() ? selected->label.c_str() : "Select actor";
            MenuLayout::Field(a_label, [&](const char* a_id) {
                bool changed{};
                if (ImGuiMCP::BeginCombo(a_id, preview)) {
                    for (const auto& actor : actorOptions) {
                        ImGuiMCP::PushID(static_cast<int>(actor.formID));
                        const bool isSelected = actor.formID == a_selectedFormID;
                        if (ImGuiMCP::Selectable(actor.label.c_str(), isSelected)) {
                            a_selectedFormID = actor.formID;
                            changed = true;
                        }
                        if (isSelected) {
                            ImGuiMCP::SetItemDefaultFocus();
                        }
                        ImGuiMCP::PopID();
                    }
                    ImGuiMCP::EndCombo();
                }
                return changed;
            });
        }

        [[nodiscard]] bool CapturePlayerWorldAnchor() {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* cell = player ? player->GetParentCell() : nullptr;
            if (!player || !cell) {
                return false;
            }

            selectedWorldPosition = player->GetPosition();
            selectedWorldCellFormID = cell->GetFormID();
            return true;
        }

        [[nodiscard]] bool ContainsSMPBone(RE::NiAVObject& a_object) {
            const char* name = a_object.name.c_str();
            if (name && std::string_view{name}.contains(kSMPBoneMarker)) {
                return true;
            }
            auto* node = a_object.AsNode();
            if (!node) {
                return false;
            }
            return std::ranges::any_of(node->GetChildren(), [](const auto& a_child) { return a_child && ContainsSMPBone(*a_child); });
        }

        [[nodiscard]] std::size_t CountMatchingBones(RE::NiAVObject& a_object, std::string_view a_match) {
            const std::string_view name = a_object.name;
            std::size_t count = name.contains(a_match) ? 1 : 0;
            if (auto* node = a_object.AsNode()) {
                for (const auto& child : node->GetChildren()) {
                    if (child) {
                        count += CountMatchingBones(*child, a_match);
                    }
                }
            }
            return count;
        }

        void CollectVisibleBonePointers(RE::NiAVObject& a_object, RE::NiAVObject* a_nearestUnrenamedParent, const std::unordered_set<RE::NiAVObject*>& a_skinnedBones, bool a_requireSMPMarker,
            std::unordered_set<RE::NiAVObject*>& a_visibleBones, std::unordered_set<RE::NiAVObject*>& a_likelyCandidates) {
            auto* node = a_object.AsNode();
            if (!node) {
                return;
            }

            const char* name = a_object.name.c_str();
            const bool hasSMPMarker = name && std::string_view{name}.contains(kSMPBoneMarker);
            if (a_skinnedBones.contains(&a_object) && (!a_requireSMPMarker || hasSMPMarker)) {
                a_visibleBones.insert(&a_object);
                if (a_requireSMPMarker) {
                    a_likelyCandidates.insert(&a_object);
                    if (a_nearestUnrenamedParent) {
                        a_visibleBones.insert(a_nearestUnrenamedParent);
                    }
                }
            }

            auto* nearestUnrenamedParent = hasSMPMarker ? a_nearestUnrenamedParent : &a_object;
            for (const auto& child : node->GetChildren()) {
                if (child) {
                    CollectVisibleBonePointers(*child, nearestUnrenamedParent, a_skinnedBones, a_requireSMPMarker, a_visibleBones, a_likelyCandidates);
                }
            }
        }

        [[nodiscard]] std::vector<SkeletonNode> CaptureVisibleSkeleton(RE::NiAVObject& a_object, const std::unordered_set<RE::NiAVObject*>& a_visibleBones, const std::unordered_set<RE::NiAVObject*>& a_likelyCandidates,
            std::size_t& a_nodeCount) {
            std::vector<SkeletonNode> children;
            if (auto* node = a_object.AsNode()) {
                for (const auto& child : node->GetChildren()) {
                    if (!child) {
                        continue;
                    }
                    auto visibleChildren = CaptureVisibleSkeleton(*child, a_visibleBones, a_likelyCandidates, a_nodeCount);
                    for (auto& visibleChild : visibleChildren) {
                        children.push_back(std::move(visibleChild));
                    }
                }
            }

            if (!a_visibleBones.contains(&a_object)) {
                return children;
            }

            const char* objectName = a_object.name.c_str();
            SkeletonNode result{.name = objectName && objectName[0] != '\0' ? objectName : "<No Name>", .children = std::move(children), .likelyCandidate = a_likelyCandidates.contains(&a_object)};
            result.displayName = result.name;
            if (result.name.contains(kSMPBoneMarker)) {
                if (const auto separator = result.name.find(' '); separator != std::string::npos && separator + 1 < result.name.size()) {
                    result.displayName.erase(0, separator + 1);
                }
            }
            result.likelyCandidate = result.likelyCandidate || std::ranges::any_of(result.children, [](const SkeletonNode& a_child) { return a_child.likelyCandidate; });
            ++a_nodeCount;
            return {std::move(result)};
        }

        [[nodiscard]] bool SkeletonNodeMatchesFilter(const SkeletonNode& a_node, std::string_view a_filter) {
            if (a_filter.empty() || a_node.displayName.contains(a_filter)) {
                return true;
            }
            return std::ranges::any_of(a_node.children, [&](const SkeletonNode& a_child) { return SkeletonNodeMatchesFilter(a_child, a_filter); });
        }

        void RenderSkeletonNode(const SkeletonNode& a_node, std::string_view a_filter) {
            if (!SkeletonNodeMatchesFilter(a_node, a_filter)) {
                return;
            }

            const std::string_view leashMatch{debugSettings.leashBoneMatch};
            const bool isParent = a_node.name == debugSettings.parentBone;
            const bool isLeashMatch = !leashMatch.empty() && a_node.name.contains(leashMatch);
            const auto label = std::format("{} [NiNode]", a_node.displayName);

            ImGuiMCP::ImGuiTreeNodeFlags treeFlags = ImGuiMCP::ImGuiTreeNodeFlags_SpanAvailWidth;
            if (isParent || isLeashMatch) {
                treeFlags |= ImGuiMCP::ImGuiTreeNodeFlags_Selected;
            }
            if (a_node.children.empty()) {
                treeFlags |= ImGuiMCP::ImGuiTreeNodeFlags_Leaf | ImGuiMCP::ImGuiTreeNodeFlags_NoTreePushOnOpen;
            } else if (!a_filter.empty()) {
                ImGuiMCP::SetNextItemOpen(true, ImGuiMCP::ImGuiCond_Always);
            } else if (a_node.likelyCandidate || (!leashMatch.empty() && SkeletonNodeMatchesFilter(a_node, leashMatch))) {
                ImGuiMCP::SetNextItemOpen(true, ImGuiMCP::ImGuiCond_Once);
            }

            ImGuiMCP::PushID(&a_node);
            const bool isOpen = ImGuiMCP::TreeNodeEx("##SkeletonNode", treeFlags, "%s", label.c_str());
            if (!a_node.children.empty() && isOpen) {
                for (const auto& child : a_node.children) {
                    RenderSkeletonNode(child, a_filter);
                }
                ImGuiMCP::TreePop();
            }
            ImGuiMCP::PopID();
        }

        void DumpSelectedSkeleton() {
            skeletonDump.clear();
            skeletonDumpActor.clear();
            const auto meshOwnerFormID = debugSettings.holderOwnsLeash ? selectedHolder : selectedLeashed;
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(meshOwnerFormID);
            if (!actor) {
                skeletonStatus = "Select an available physical leash owner before dumping the skeleton.";
                return;
            }

            auto* root = actor->Get3D(false);
            if (!root) {
                skeletonStatus = std::format("{} has no loaded third-person skeleton.", DescribeActor(actor));
                return;
            }

            auto* npcObject = root->GetObjectByName(RE::BSFixedString("NPC"));
            auto* npcNode = npcObject ? npcObject->AsNode() : nullptr;
            if (!npcNode) {
                skeletonStatus = std::format("{} has no loaded NPC skeleton node.", DescribeActor(actor));
                return;
            }

            std::unordered_set<RE::NiAVObject*> skinnedBones;
            RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry) {
                const auto skin = a_geometry->GetGeometryRuntimeData().skinInstance;
                if (!skin || !skin->bones) {
                    return RE::BSVisit::BSVisitControl::kContinue;
                }
                for (std::uint32_t index = 0; index < skin->numMatrices; ++index) {
                    if (auto* bone = skin->bones[index]; bone && bone->AsNode()) {
                        skinnedBones.insert(bone);
                    }
                }
                return RE::BSVisit::BSVisitControl::kContinue;
            });

            const bool requireSMPMarker = ContainsSMPBone(*npcNode);
            std::unordered_set<RE::NiAVObject*> visibleBones{npcNode};
            std::unordered_set<RE::NiAVObject*> likelyCandidates;
            CollectVisibleBonePointers(*npcNode, npcNode, skinnedBones, requireSMPMarker, visibleBones, likelyCandidates);

            skeletonDumpActor = DescribeActor(actor);
            std::size_t nodeCount{};
            skeletonDump = CaptureVisibleSkeleton(*npcNode, visibleBones, likelyCandidates, nodeCount);
            skeletonStatus = std::format("Displayed {} skeleton node(s) for {}.", nodeCount, skeletonDumpActor);
        }

        void RenderSkeletonDumper() {
            ImGuiMCP::SeparatorText("Skeleton inspector");
            ImGuiMCP::TextWrapped("Inspect skinned bones on the selected physical leash owner. The test leash's parent and matching bones are highlighted.");
            if (ImGuiMCP::Button("Capture skeleton")) {
                DumpSelectedSkeleton();
            }
            MenuLayout::Feedback(skeletonStatus);
            if (skeletonDump.empty()) {
                return;
            }

            ImGuiMCP::TextWrapped("Snapshot: %s", skeletonDumpActor.c_str());
            ImGuiMCP::SetNextItemWidth(-1.0F);
            ImGuiMCP::InputTextWithHint("##SkeletonFilter", "Filter bone names", skeletonFilter, sizeof(skeletonFilter));
            ImGuiMCP::ImVec2 available;
            ImGuiMCP::GetContentRegionAvail(&available);
            const auto lineHeight = ImGuiMCP::GetTextLineHeightWithSpacing();
            const auto height = std::clamp(available.y, lineHeight * 8.0F, lineHeight * 24.0F);
            if (ImGuiMCP::BeginChild("SkeletonDump", {0.0F, height}, ImGuiMCP::ImGuiChildFlags_Border)) {
                const std::string_view filter{skeletonFilter};
                for (const auto& root : skeletonDump) {
                    RenderSkeletonNode(root, filter);
                }
            }
            ImGuiMCP::EndChild();
        }

        std::string DescribeArmor(RE::TESObjectARMO* a_armor) {
            const auto* name = a_armor->GetName();
            return std::format("{} ({:08X})", name && name[0] != '\0' ? name : "Unnamed armor", a_armor->GetFormID());
        }

        void EquipArmor(RE::Actor* a_actor, RE::TESObjectARMO* a_armor) {
            if (!a_actor) {
                armorStatus = "Select an available physical leash owner before equipping armor.";
            } else if (!a_armor) {
                armorStatus = "Select an available armor before equipping.";
            } else if (auto* equipManager = RE::ActorEquipManager::GetSingleton(); !equipManager) {
                armorStatus = "The actor equip manager is unavailable.";
            } else {
                const auto inventory = a_actor->GetInventoryCounts();
                const auto item = inventory.find(a_armor);
                if (item == inventory.end() || item->second <= 0) {
                    a_actor->AddObjectToContainer(a_armor, nullptr, 1, nullptr);
                }
                equipManager->EquipObject(a_actor, a_armor, nullptr, 1, nullptr, true, true);
                armorStatus = std::format("Equipped {} on {}.", DescribeArmor(a_armor), DescribeActor(a_actor));
            }
        }

        void RenderPluginArmorDropdown() {
            ImGuiMCP::SeparatorText("Browse Leash.esm");
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            const auto* plugin = dataHandler ? dataHandler->LookupModByName("Leash.esm") : nullptr;
            if (!plugin || plugin->GetCompileIndex() == 0xFF) {
                ImGuiMCP::TextUnformatted("Leash.esm armor is unavailable.");
                return;
            }

            auto* armor = RE::TESForm::LookupByID<RE::TESObjectARMO>(selectedArmor);
            if (armor && !plugin->IsFormInMod(armor->GetFormID())) {
                armor = nullptr;
            }
            const auto preview = armor ? DescribeArmor(armor) : std::string{"Select armor"};
            ImGuiMCP::SetNextItemWidth(-1.0F);
            if (ImGuiMCP::BeginCombo("##PluginArmor", preview.c_str())) {
                std::vector<RE::TESObjectARMO*> armors;
                for (auto* candidate : dataHandler->GetFormArray<RE::TESObjectARMO>()) {
                    if (candidate && plugin->IsFormInMod(candidate->GetFormID())) {
                        armors.push_back(candidate);
                    }
                }
                std::ranges::sort(armors, {}, DescribeArmor);
                for (auto* candidate : armors) {
                    ImGuiMCP::PushID(static_cast<int>(candidate->GetFormID()));
                    const bool isSelected = candidate->GetFormID() == selectedArmor;
                    const auto label = DescribeArmor(candidate);
                    if (ImGuiMCP::Selectable(label.c_str(), isSelected)) {
                        selectedArmor = candidate->GetFormID();
                        armor = candidate;
                    }
                    if (isSelected) {
                        ImGuiMCP::SetItemDefaultFocus();
                    }
                    ImGuiMCP::PopID();
                }
                if (armors.empty()) {
                    ImGuiMCP::TextUnformatted("No armor found in Leash.esm.");
                }
                ImGuiMCP::EndCombo();
            }
            const auto actorFormID = debugSettings.holderOwnsLeash ? selectedHolder : selectedLeashed;
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorFormID);
            ImGuiMCP::BeginDisabled(!armor || !actor);
            if (ImGuiMCP::Button("Equip selected armor")) {
                EquipArmor(actor, armor);
            }
            ImGuiMCP::EndDisabled();
        }

        void EquipArmorEntry(const ArmorEntry& entry) {
            const auto meshOwnerFormID = debugSettings.holderOwnsLeash ? selectedHolder : selectedLeashed;
            auto* meshOwner = RE::TESForm::LookupByID<RE::Actor>(meshOwnerFormID);
            std::string_view formIDText{entry.formID};
            if (const auto comment = formIDText.find('#'); comment != std::string_view::npos) {
                formIDText = formIDText.substr(0, comment);
            }
            const auto firstCharacter = formIDText.find_first_not_of(" \t\r\n");
            if (firstCharacter == std::string_view::npos) {
                formIDText = {};
            } else {
                const auto lastCharacter = formIDText.find_last_not_of(" \t\r\n");
                formIDText = formIDText.substr(firstCharacter, lastCharacter - firstCharacter + 1);
            }
            if (formIDText.starts_with("0x") || formIDText.starts_with("0X")) {
                formIDText.remove_prefix(2);
            }

            std::uint32_t localFormID{};
            const auto parseResult = std::from_chars(formIDText.data(), formIDText.data() + formIDText.size(), localFormID, 16);
            if (!meshOwner) {
                armorStatus = "Select an available physical leash owner before equipping armor.";
            } else if (entry.modName[0] == '\0' || formIDText.empty()) {
                armorStatus = "Enter a mod name and local FormID.";
            } else if (parseResult.ec != std::errc{} || parseResult.ptr != formIDText.data() + formIDText.size()) {
                armorStatus = std::format("{} is not a valid hexadecimal FormID.", entry.formID);
            } else if (auto* dataHandler = RE::TESDataHandler::GetSingleton(); !dataHandler) {
                armorStatus = "The game data handler is unavailable.";
            } else if (const auto* plugin = dataHandler->LookupModByName(entry.modName); !plugin || plugin->GetCompileIndex() == 0xFF) {
                armorStatus = std::format("Plugin {} is not loaded.", entry.modName);
            } else {
                const auto resolvedFormID = dataHandler->LookupFormID(localFormID, entry.modName);
                auto* form = RE::TESForm::LookupByID(resolvedFormID);
                if (!form) {
                    armorStatus = std::format("Could not find {}:{:X}; resolved runtime FormID {:08X}.", entry.modName, localFormID, resolvedFormID);
                } else if (!form->Is(RE::FormType::Armor)) {
                    armorStatus = std::format("Found a {} record at {}:{:X}; equip requires an ARMO record.", RE::FormTypeToString(form->GetFormType()), entry.modName, localFormID);
                } else {
                    EquipArmor(meshOwner, static_cast<RE::TESObjectARMO*>(form));
                }
            }
        }

        void RenderArmorEntries() {
            ImGuiMCP::SeparatorText("Armor shortcuts");
            const bool editEntries = ImGuiMCP::CollapsingHeader("Edit shortcuts");
            if (editEntries) {
                ImGuiMCP::TextWrapped("Use a plugin name and hexadecimal local FormID. Add # followed by a name to label a shortcut.");
            }
            constexpr auto tableFlags = ImGuiMCP::ImGuiTableFlags_BordersInnerH | ImGuiMCP::ImGuiTableFlags_RowBg | ImGuiMCP::ImGuiTableFlags_SizingStretchProp;
            if (!ImGuiMCP::BeginTable("LeashArmor", 3, tableFlags)) {
                return;
            }

            ImGuiMCP::TableSetupColumn(editEntries ? "Plugin" : "Armor", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn(editEntries ? "Local FormID #Name" : "Source", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn("", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed);
            ImGuiMCP::TableHeadersRow();
            for (std::size_t index = 0; index < debugSettings.armorEntries.size(); ++index) {
                auto& entry = debugSettings.armorEntries[index];
                ImGuiMCP::PushID(static_cast<int>(index));
                ImGuiMCP::TableNextRow();
                ImGuiMCP::TableSetColumnIndex(0);
                if (editEntries) {
                    ImGuiMCP::SetNextItemWidth(-1.0F);
                    ImGuiMCP::InputText("##ModName", entry.modName, sizeof(entry.modName));
                    ImGuiMCP::TableSetColumnIndex(1);
                    ImGuiMCP::SetNextItemWidth(-1.0F);
                    ImGuiMCP::InputText("##FormID", entry.formID, sizeof(entry.formID));
                } else {
                    const std::string_view formID{entry.formID};
                    const auto comment = formID.find('#');
                    const auto name = comment != std::string_view::npos ? formID.substr(comment + 1) : formID;
                    ImGuiMCP::TextWrapped("%.*s", static_cast<int>(name.size()), name.data());
                    ImGuiMCP::TableSetColumnIndex(1);
                    const auto localID = formID.substr(0, comment);
                    ImGuiMCP::TextWrapped("%s / %.*s", entry.modName, static_cast<int>(localID.size()), localID.data());
                }
                ImGuiMCP::TableSetColumnIndex(2);
                if (ImGuiMCP::Button("Equip")) {
                    EquipArmorEntry(entry);
                }
                ImGuiMCP::PopID();
            }
            ImGuiMCP::EndTable();
        }

        void RenderActiveLeashLength(RE::Actor* a_leashed, float a_length, bool a_minimum) {
            ImGuiMCP::SetNextItemWidth(-1.0F);
            ImGuiMCP::BeginDisabled(!a_leashed);
            if (ImGuiMCP::InputFloat(a_minimum ? "##MinLength" : "##MaxLength", &a_length, 1.0F, 10.0F, "%.1f", ImGuiMCP::ImGuiInputTextFlags_EnterReturnsTrue)) {
                auto& manager = LeashManager::GetSingleton();
                const bool updated = a_minimum ? manager.SetMinLength(a_leashed, a_length) : manager.SetMaxLength(a_leashed, a_length);
                const auto label = a_minimum ? "Minimum" : "Maximum";
                activeStatus = updated ? std::format("{} distance for {} set to {:.1f}.", label, DescribeActor(a_leashed), a_length)
                                       : std::format("Could not update {} distance. Minimum must be non-negative, maximum must be positive, and minimum cannot exceed maximum.", a_minimum ? "minimum" : "maximum");
            }
            ImGuiMCP::EndDisabled();
            if (ImGuiMCP::IsItemHovered()) {
                ImGuiMCP::SetTooltip("%s", a_minimum ? "Release / arrival distance. Must be between zero and the current maximum."
                                                   : "Maximum follow distance. Must be positive and at least the current minimum.");
            }
        }

        void RenderActiveLeashes(const std::vector<LeashDefinition>& a_definitions) {
            ImGuiMCP::SeparatorText("Active leashes");
            if (a_definitions.empty()) {
                ImGuiMCP::TextUnformatted("No actors are currently leashed.");
                return;
            }
            ImGuiMCP::TextWrapped("Adjust each leash's follow range below. Press Enter to apply a typed distance; +/- buttons apply immediately.");

            constexpr auto tableFlags = ImGuiMCP::ImGuiTableFlags_BordersInnerH | ImGuiMCP::ImGuiTableFlags_RowBg | ImGuiMCP::ImGuiTableFlags_SizingStretchProp;
            if (!ImGuiMCP::BeginTable("ActiveLeashes", 6, tableFlags)) {
                return;
            }

            ImGuiMCP::TableSetupColumn("Leashed actor", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn("Leasher", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn("Physical owner", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn("Min distance", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, ImGuiMCP::GetFontSize() * 9.0F);
            ImGuiMCP::TableSetupColumn("Max distance", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, ImGuiMCP::GetFontSize() * 9.0F);
            ImGuiMCP::TableSetupColumn("", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed);
            ImGuiMCP::TableHeadersRow();
            for (const auto& definition : a_definitions) {
                ImGuiMCP::PushID(static_cast<int>(definition.leashedFormID));
                auto* leashed = RE::TESForm::LookupByID<RE::Actor>(definition.leashedFormID);
                ImGuiMCP::TableNextRow();
                ImGuiMCP::TableSetColumnIndex(0);
                const auto leashedLabel = DescribeActor(definition.leashedFormID);
                ImGuiMCP::TextWrapped("%s", leashedLabel.c_str());
                ImGuiMCP::TableSetColumnIndex(1);
                const auto holderLabel = definition.holderFormID != 0 ? DescribeActor(definition.holderFormID) : std::string{"World position"};
                ImGuiMCP::TextWrapped("%s", holderLabel.c_str());
                ImGuiMCP::TableSetColumnIndex(2);
                const auto meshOwnerFormID = definition.meshOwner == LeashMeshOwner::kHolder ? definition.holderFormID : definition.leashedFormID;
                const auto meshOwnerLabel = DescribeActor(meshOwnerFormID);
                ImGuiMCP::TextWrapped("%s", meshOwnerLabel.c_str());
                ImGuiMCP::TableSetColumnIndex(3);
                RenderActiveLeashLength(leashed, definition.minLength, true);
                ImGuiMCP::TableSetColumnIndex(4);
                RenderActiveLeashLength(leashed, definition.maxLength, false);
                ImGuiMCP::TableSetColumnIndex(5);
                if (ImGuiMCP::Button("Disconnect")) {
                    auto* holder = RE::TESForm::LookupByID<RE::Actor>(definition.holderFormID);
                    const auto disconnected = LeashManager::GetSingleton().Disconnect(holder, leashed);
                    activeStatus = disconnected ? std::format("Freed {}.", leashedLabel) : std::format("Could not free {}.", leashedLabel);
                }
                ImGuiMCP::PopID();
            }
            ImGuiMCP::EndTable();
        }

        void RenderTestActors() {
            if (!actorsLoaded) {
                RefreshActors();
            }
            const auto previousHolder = selectedHolder;
            const auto previousLeashed = selectedLeashed;
            const auto previousOwner = debugSettings.holderOwnsLeash;
            if (ImGuiMCP::Button("Refresh nearby actors")) {
                RefreshActors();
            }
            ImGuiMCP::SameLine();
            ImGuiMCP::TextDisabled("%zu available", actorOptions.size());
            MenuLayout::Columns("TestActors", 22.0F, {
                [] { RenderActorDropdown("Leashed actor", selectedLeashed); },
                [] {
                    if (!debugSettings.holderOwnsLeash && selectedAnchorType == DebugAnchorType::kWorldPosition) {
                        ImGuiMCP::TextUnformatted("Leasher");
                        ImGuiMCP::TextDisabled("None (world anchor)");
                    } else {
                        RenderActorDropdown("Leasher", selectedHolder);
                    }
                },
                [] {
                    int meshOwnerIndex = debugSettings.holderOwnsLeash ? 1 : 0;
                    if (MenuLayout::Choice("Physical leash owner", meshOwnerIndex, kMeshOwnerLabels)) {
                        debugSettings.holderOwnsLeash = meshOwnerIndex == 1;
                        if (debugSettings.holderOwnsLeash) {
                            selectedAnchorType = DebugAnchorType::kActorBone;
                        }
                    }
                }
            });
            if (previousHolder != selectedHolder || previousLeashed != selectedLeashed || previousOwner != debugSettings.holderOwnsLeash) {
                applyStatus.clear();
                armorStatus.clear();
                skeletonStatus.clear();
                skeletonDump.clear();
                skeletonDumpActor.clear();
            }
        }

        void RenderAnchorSettings() {
            ImGuiMCP::SeparatorText("Anchor");
            if (debugSettings.holderOwnsLeash) {
                ImGuiMCP::TextWrapped("The leasher wears the rope; its endpoint attaches to a bone on the leashed actor.");
            } else {
                auto anchorIndex = static_cast<int>(selectedAnchorType);
                if (MenuLayout::Choice("Attach to", anchorIndex, kDebugAnchorLabels)) {
                    selectedAnchorType = static_cast<DebugAnchorType>(anchorIndex);
                    applyStatus.clear();
                    if (selectedAnchorType == DebugAnchorType::kWorldPosition && !CapturePlayerWorldAnchor()) {
                        applyStatus = "Could not capture the player position and cell.";
                    }
                }
            }

            if (!debugSettings.holderOwnsLeash && selectedAnchorType == DebugAnchorType::kWorldPosition) {
                MenuLayout::Field("World position (X, Y, Z)", [](const char* a_id) { return ImGuiMCP::InputFloat3(a_id, &selectedWorldPosition.x, "%.2f"); });
                ImGuiMCP::Text("Cell: %08X", selectedWorldCellFormID);
                if (ImGuiMCP::Button("Use current player position")) {
                    applyStatus = CapturePlayerWorldAnchor() ? "Captured the current player position and cell." : "Could not capture the player position and cell.";
                }
            } else if (debugSettings.holderOwnsLeash || selectedAnchorType == DebugAnchorType::kActorBone) {
                MenuLayout::Field(debugSettings.holderOwnsLeash ? "Bone on leashed actor" : "Bone on leasher",
                    [](const char* a_id) { return ImGuiMCP::InputText(a_id, selectedAttachmentBone, sizeof(selectedAttachmentBone)); });
                MenuLayout::Field("Attachment offset (X, Y, Z)", [](const char* a_id) { return ImGuiMCP::InputFloat3(a_id, &debugSettings.attachmentOffset.x, "%.2f"); });
                if (debugSettings.holderOwnsLeash) {
                    MenuLayout::Choice("Closed leasher hand", debugSettings.closedHand, kClosedHandLabels);
                }
            } else {
                ImGuiMCP::TextWrapped("Uses the selected leasher's hand with the closed-fist grip.");
            }
        }

        void RenderRopeSettings() {
            ImGuiMCP::SeparatorText("Rope bones");
            MenuLayout::Field("Parent bone", [](const char* a_id) { return ImGuiMCP::InputText(a_id, debugSettings.parentBone, sizeof(debugSettings.parentBone)); },
                "Exact parent bone name on the physical leash owner. Use the Skeleton tab to inspect available bones.");
            MenuLayout::Field("Leash bone match", [](const char* a_id) { return ImGuiMCP::InputText(a_id, debugSettings.leashBoneMatch, sizeof(debugSettings.leashBoneMatch)); },
                "Match text for bones beneath the parent. At least two matching bones are required.");
        }

        void RenderPullDistances() {
            ImGuiMCP::SeparatorText("Pull distances");
            MenuLayout::Columns("PullDistances", 22.0F, {
                [] {
                    MenuLayout::Field("Minimum length", [](const char* a_id) { return ImGuiMCP::InputFloat(a_id, &debugSettings.minLength, 1.0F, 10.0F, "%.1f"); },
                        "The player regains movement control at this distance, even while the holder moves. NPC followers settle here with a small arrival tolerance when the holder stops.\n"
                        "While the holder moves, the preferred gap is configured under Locomotion. World-position leashes stop pulling at this distance.");
                },
                [] {
                    MenuLayout::Field("Maximum length", [](const char* a_id) { return ImGuiMCP::InputFloat(a_id, &debugSettings.maxLength, 1.0F, 10.0F, "%.1f"); },
                        "Player pulling and world-position pulling begin only beyond this distance. Actor-held NPC followers can start earlier to keep pace.\nForced recovery also uses this length.");
                }
            });
        }

        void RenderApplyLeash() {
            RenderTestActors();
            MenuLayout::Columns("LeashSetup", 30.0F, {RenderAnchorSettings, RenderRopeSettings});
            RenderPullDistances();
            ImGuiMCP::Checkbox("Keep leash in saves", &debugSettings.persistent);
            ImGuiMCP::Spacing();
            ImGuiMCP::Separator();
            ImGuiMCP::TextWrapped("Equip a rope on the physical leash owner in Equipment. Use Skeleton to check its bone names.");
            if (ImGuiMCP::Button("Apply test leash")) {
                applyStatus.clear();
                auto* leashed = RE::TESForm::LookupByID<RE::Actor>(selectedLeashed);
                auto* holder = RE::TESForm::LookupByID<RE::Actor>(selectedHolder);
                bool applied{};
                std::string anchorLabel;
                if (!leashed) {
                    applyStatus = "Could not apply leash. Select an available leashed actor.";
                } else if (debugSettings.holderOwnsLeash) {
                    if (!holder) {
                        applyStatus = "Could not apply leash. Select an available leasher.";
                    } else {
                        applied = LeashManager::GetSingleton().ApplyHolderOwnedLeashToBone(holder, leashed, selectedAttachmentBone, debugSettings.attachmentOffset.x, debugSettings.attachmentOffset.y,
                            debugSettings.attachmentOffset.z, debugSettings.parentBone, debugSettings.leashBoneMatch, debugSettings.minLength, debugSettings.maxLength, debugSettings.persistent, debugSettings.closedHand);
                        anchorLabel = std::format("bone '{}' on {}", selectedAttachmentBone, DescribeActor(leashed));
                    }
                } else if (selectedAnchorType == DebugAnchorType::kWorldPosition) {
                    auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(selectedWorldCellFormID);
                    if (!cell) {
                        applyStatus = "Could not apply leash. Capture an available player position and cell.";
                    } else {
                        applied = LeashManager::GetSingleton().ApplyAtPosition(leashed, cell, selectedWorldPosition.x, selectedWorldPosition.y, selectedWorldPosition.z, debugSettings.parentBone,
                            debugSettings.leashBoneMatch, debugSettings.minLength, debugSettings.maxLength, debugSettings.persistent);
                        anchorLabel = std::format("world position ({:.1f}, {:.1f}, {:.1f}) in cell {:08X}", selectedWorldPosition.x, selectedWorldPosition.y, selectedWorldPosition.z, selectedWorldCellFormID);
                    }
                } else {
                    if (!holder) {
                        applyStatus = "Could not apply leash. Select an available leasher.";
                    } else if (selectedAnchorType == DebugAnchorType::kActorBone) {
                        applied = LeashManager::GetSingleton().ApplyToBone(holder, leashed, selectedAttachmentBone, debugSettings.attachmentOffset.x, debugSettings.attachmentOffset.y, debugSettings.attachmentOffset.z,
                            debugSettings.parentBone, debugSettings.leashBoneMatch, debugSettings.minLength, debugSettings.maxLength, debugSettings.persistent);
                        anchorLabel = std::format("bone '{}' on {}", selectedAttachmentBone, DescribeActor(holder));
                    } else {
                        const bool rightHand = selectedAnchorType == DebugAnchorType::kRightHand;
                        applied = LeashManager::GetSingleton().ApplyToHand(holder, leashed, debugSettings.parentBone, debugSettings.leashBoneMatch, debugSettings.minLength, debugSettings.maxLength,
                            debugSettings.persistent, rightHand);
                        anchorLabel = std::format("the {} hand of {}", rightHand ? "right" : "left", DescribeActor(holder));
                    }
                }

                if (applyStatus.empty()) {
                    if (!applied) {
                        applyStatus = "Could not apply leash. Check the selected anchor, bone names, and length values.";
                    } else {
                        auto* meshOwner = debugSettings.holderOwnsLeash ? holder : leashed;
                        auto* root = meshOwner ? meshOwner->Get3D(false) : nullptr;
                        auto* parent = root ? root->GetObjectByName(RE::BSFixedString(debugSettings.parentBone)) : nullptr;
                        auto* parentNode = parent ? parent->AsNode() : nullptr;
                        if (!root) {
                            applyStatus = std::format("Warning: Leash applied, but {} has no currently loaded third-person skeleton.", DescribeActor(meshOwner));
                        } else if (!parent) {
                            applyStatus = std::format("Warning: Leash applied, but {} does not currently contain parent bone '{}'.", DescribeActor(meshOwner), debugSettings.parentBone);
                        } else if (!parentNode) {
                            applyStatus = std::format("Warning: Leash applied, but parent bone '{}' on {} is not a node.", debugSettings.parentBone, DescribeActor(meshOwner));
                        } else {
                            std::size_t matchedBones{};
                            const std::string_view leashMatch{debugSettings.leashBoneMatch};
                            for (const auto& child : parentNode->GetChildren()) {
                                if (child) {
                                    matchedBones += CountMatchingBones(*child, leashMatch);
                                }
                            }
                            if (matchedBones == 0) {
                                applyStatus = std::format("Warning: Leash applied, but {} does not currently contain a bone matching '{}' under '{}'.", DescribeActor(meshOwner), leashMatch, debugSettings.parentBone);
                            } else if (matchedBones == 1) {
                                applyStatus = std::format("Warning: Leash applied, but {} currently contains only one bone matching '{}' under '{}'; at least two are required to bind.", DescribeActor(meshOwner),
                                    leashMatch, debugSettings.parentBone);
                            } else if (debugSettings.holderOwnsLeash) {
                                auto* attachmentRoot = leashed->Get3D(false);
                                if (!attachmentRoot || !attachmentRoot->GetObjectByName(RE::BSFixedString(selectedAttachmentBone))) {
                                    applyStatus = std::format("Warning: Leash applied, but {} does not currently contain attachment bone '{}'.", DescribeActor(leashed), selectedAttachmentBone);
                                } else {
                                    applyStatus = std::format("Leashed {} to {} using the leash equipped by {}.", DescribeActor(leashed), anchorLabel, DescribeActor(holder));
                                }
                            } else {
                                applyStatus = std::format("Leashed {} to {}.", DescribeActor(leashed), anchorLabel);
                            }
                        }
                    }
                }
            }
            MenuLayout::Feedback(applyStatus);
        }

        void RenderActorBodyCapsuleSettings(const char* a_label, Physics::ActorBodyCapsuleSettings& a_settings) {
            if (!ImGuiMCP::TreeNode(a_label)) {
                return;
            }
            ImGuiMCP::InputFloat3("Local offset (X, Y, Z)", &a_settings.offset.x, "%.2f");
            ImGuiMCP::InputFloat("Radius", &a_settings.radius, 0.25F, 1.0F, "%.2f");
            ImGuiMCP::InputFloat("Width", &a_settings.width, 0.5F, 2.0F, "%.2f");
            a_settings.radius = (std::max)(a_settings.radius, 0.0F);
            a_settings.width = (std::max)(a_settings.width, 0.0F);
            ImGuiMCP::TreePop();
        }

        void RenderActorBodySexSettings(Physics::ActorBodySexSettings& a_settings) {
            RenderActorBodyCapsuleSettings("NPC Spine [Spn0]", a_settings.spine);
            RenderActorBodyCapsuleSettings("NPC Spine1 [Spn1]", a_settings.spine1);
            RenderActorBodyCapsuleSettings("NPC Spine2 [Spn2]", a_settings.spine2);
            RenderActorBodyCapsuleSettings("NPC Neck [Neck]", a_settings.neck);
        }

        void RenderActorBodyCollisionSettings(Physics::ActorBodyCollisionSettings& a_settings) {
            if (!ImGuiMCP::CollapsingHeader("Actor Body Collision")) {
                return;
            }
            ImGuiMCP::TextWrapped("Capsules use each bone's local X axis. Offsets are bone-local coordinates. Width is the distance between the capsule cap centers.");
            if (!ImGuiMCP::BeginTabBar("ActorBodyCollisionSex")) {
                return;
            }
            if (ImGuiMCP::BeginTabItem("Male")) {
                RenderActorBodySexSettings(a_settings.male);
                ImGuiMCP::EndTabItem();
            }
            if (ImGuiMCP::BeginTabItem("Female")) {
                RenderActorBodySexSettings(a_settings.female);
                if (ImGuiMCP::TreeNode("Breasts")) {
                    ImGuiMCP::InputFloat3("Local offset (X, Y, Z)", &a_settings.femaleBreast.offset.x, "%.2f");
                    ImGuiMCP::InputFloat("Radius", &a_settings.femaleBreast.radius, 0.25F, 1.0F, "%.2f");
                    a_settings.femaleBreast.radius = (std::max)(a_settings.femaleBreast.radius, 0.0F);
                    ImGuiMCP::TreePop();
                }
                ImGuiMCP::EndTabItem();
            }
            ImGuiMCP::EndTabBar();
        }

        void RenderLocomotionSettings(LocomotionSettings& a_settings) {
            if (!ImGuiMCP::CollapsingHeader("Locomotion")) {
                return;
            }
            ImGuiMCP::SliderFloat("Forward assistance", &a_settings.forwardAssistance, 0.0F, 3.0F, "%.2f");
            if (ImGuiMCP::IsItemHovered()) {
                ImGuiMCP::SetTooltip(
                    "Extra catch-up speed when the leashed player presses toward the pull path (usually W when facing the holder).\n"
                    "Higher values help you reach minimum leash length and regain movement control sooner. Zero disables this boost.\n"
                    "Default: 1.00. Range: 0.00-3.00. A speed of 2.00 is the controller's running-speed reference.");
            }
            ImGuiMCP::SliderFloat("Backward resistance", &a_settings.backwardResistance, 0.0F, 3.0F, "%.2f");
            if (ImGuiMCP::IsItemHovered()) {
                ImGuiMCP::SetTooltip(
                    "Reduces pull speed when the leashed player presses against the pull direction (usually S when facing the holder).\n"
                    "Higher values make resisting more effective, subject to the minimum forced-pull ratio. Zero disables input resistance.\n"
                    "Default: 1.50. Range: 0.00-3.00. Uses the same speed units as forward assistance.");
            }
            ImGuiMCP::SliderFloat("Minimum forced-pull ratio", &a_settings.minimumForcedPullRatio, 0.0F, 1.0F, "%.2f");
            if (ImGuiMCP::IsItemHovered()) {
                ImGuiMCP::SetTooltip(
                    "The fraction of automatic pull speed that player resistance cannot remove at maximum leash length or beyond.\n"
                    "This floor fades toward zero near minimum length. Higher values make a taut leash harder to resist.\n"
                    "Default: 0.50 (50%%). Range: 0.00-1.00. Zero removes the floor; it does not disable pulling or ragdoll recovery.");
            }
            ImGuiMCP::SliderFloat("Maximum catch-up speed", &a_settings.maximumCatchUpSpeed, 0.25F, 10.0F, "%.2f");
            if (ImGuiMCP::IsItemHovered()) {
                ImGuiMCP::SetTooltip(
                    "Caps automatic locomotion speed for leashed players and NPCs. Higher values let them catch faster holders more quickly.\n"
                    "Player forward assistance can raise the final cap by its configured amount. Very low values can prevent catching up.\n"
                    "Default: 3.00. Range: 0.25-5.00. A speed of 2.00 is the controller's running-speed reference. Does not affect ragdoll pulling.");
            }
            ImGuiMCP::SliderFloat("Moving follow gap", &a_settings.movingFollowGap, 0.0F, 1.0F, "%.2f");
            if (ImGuiMCP::IsItemHovered()) {
                ImGuiMCP::SetTooltip(
                    "Preferred distance while an actor holder moves, expressed as a fraction between minimum and maximum leash length.\n"
                    "Zero targets minimum length; one targets maximum length. Lower values keep followers closer.\n"
                    "Default: 0.40 (40%%). For minimum 200 and maximum 300, this targets 240 units.\n"
                    "Player forward assistance can close this gap. Does not change player pull/release thresholds or world-position anchors.");
            }
            ImGuiMCP::SliderFloat("Distance response rate", &a_settings.distanceResponseRate, 0.1F, 10.0F, "%.2f");
            if (ImGuiMCP::IsItemHovered()) {
                ImGuiMCP::SetTooltip(
                    "How strongly remaining distance changes automatic pull speed for players and NPCs.\n"
                    "Higher values close the gap faster, including the return toward minimum length when the holder stops.\n"
                    "Arrival braking and the maximum catch-up speed still apply. Default: 2.50. Range: 0.10-10.00.");
            }
        }

        void __stdcall RenderSettingsPage() {
            auto& manager = LeashManager::GetSingleton();
            auto simulation = manager.GetSimulationSettings();
            auto pullPose = manager.GetPullPoseSettings();
            auto locomotion = manager.GetLocomotionSettings();
            auto recovery = manager.GetRecoverySettings();
            auto teleport = manager.GetTeleportSettings();
            auto frameHook = Hooks::FrameHook::GetSettings();
            ImGuiMCP::Checkbox("Free camera while AI-controlled", &frameHook.freeCameraWhileAIControlled);
            if (ImGuiMCP::IsItemHovered()) {
                ImGuiMCP::SetTooltip("Allows the player to rotate the camera freely while AI Mode or forced walking controls their movement.");
            }
            ImGuiMCP::Checkbox("Ragdoll NPCs", &recovery.enableNPCs);
            ImGuiMCP::Checkbox("Ragdoll Player", &recovery.enablePlayer);
            ImGuiMCP::SetNextItemWidth(ImGuiMCP::CalcItemWidth() * 0.4F);
            ImGuiMCP::InputFloat("Trigger distance multiplier", &recovery.distanceMultiplier, 0.1F, 0.5F, "%.2f");
            ImGuiMCP::SetNextItemWidth(ImGuiMCP::CalcItemWidth() * 0.4F);
            ImGuiMCP::InputFloat("Teleport grace period", &teleport.gracePeriod, 0.1F, 0.5F, "%.2f");
            ImGuiMCP::SetNextItemWidth(ImGuiMCP::CalcItemWidth() * 0.4F);
            ImGuiMCP::InputFloat("Leashed player teleport distance", &teleport.playerDistance, 10.0F, 100.0F, "%.1f");
            ImGuiMCP::SetNextItemWidth(ImGuiMCP::CalcItemWidth() * 0.4F);
            ImGuiMCP::InputFloat("Leashed NPC teleport distance", &teleport.npcDistance, 10.0F, 100.0F, "%.1f");
            ImGuiMCP::Checkbox("Collide with actors", &simulation.collideWithActors);
            ImGuiMCP::Checkbox("Procedural pulling pose", &pullPose.enabled);
            if (pullPose.enabled && ImGuiMCP::TreeNode("Procedural Pulling Pose")) {
                ImGuiMCP::InputFloat("Minimum pull strength", &pullPose.minimumStrength, 0.05F, 0.25F, "%.2f");
                ImGuiMCP::InputFloat("Maximum pull strength", &pullPose.maximumStrength, 0.05F, 0.25F, "%.2f");
                ImGuiMCP::InputFloat("Maximum spine angle", &pullPose.maximumAngleDegrees, 0.5F, 2.0F, "%.1f degrees");
                ImGuiMCP::InputFloat("Pose response rate", &pullPose.responseRate, 0.5F, 2.0F, "%.1f");
                ImGuiMCP::InputFloat("Pose anticipation time", &pullPose.anticipationTime, 0.01F, 0.05F, "%.2f seconds");
                if (ImGuiMCP::IsItemHovered()) {
                    ImGuiMCP::SetTooltip(
                        "Predicts how far the leash endpoints will separate, so leaning can react before the chain stretches.\n"
                        "Higher values react earlier to outward movement; lower values are less anticipatory.\n"
                        "Default: 0.12 seconds. Range: 0.00-0.30. Zero disables prediction, not leaning.\n"
                        "Prediction is capped at 15%% of the chain's actual length or 32 units, whichever is smaller.\n"
                        "Affects the pose only; does not change maxLength or start movement pulling earlier.");
                }
                ImGuiMCP::InputFloat("Pose slack reserve ratio", &pullPose.slackReserveRatio, 0.01F, 0.05F, "%.2f");
                if (ImGuiMCP::IsItemHovered()) {
                    ImGuiMCP::SetTooltip(
                        "Reserves a little of the chain's actual length by starting the lean before it becomes fully taut.\n"
                        "Higher values provide more buffer but may cause leaning while the chain still looks slack.\n"
                        "Default: 0.03 (3%%). Range: 0.00-0.10. Zero disables the reserve, not leaning.\n"
                        "The reserve is capped at 8 units. A 200-unit chain at 0.03 reserves 6 units.\n"
                        "Affects the pose only; does not shorten the chain or change the movement pull threshold.");
                }
                ImGuiMCP::TreePop();
            }
            ImGuiMCP::SetNextItemWidth(ImGuiMCP::CalcItemWidth() * 0.4F);
            ImGuiMCP::SliderScalar("Solve iterations", ImGuiMCP::ImGuiDataType_U32, &simulation.constraintIterations, &Physics::SimulationSettings::kMinimumConstraintIterations,
                &Physics::SimulationSettings::kMaximumConstraintIterations, "%u");
            ImGuiMCP::Spacing();
            ImGuiMCP::TextWrapped(
                "Ragdoll physically pulls a leashed actor after it exceeds maximum length multiplied by this factor. Teleport distance is the allowed distance beyond maximum leash length before an NPC holder moves the "
                "leashed player or NPC after the grace period. A teleport distance of zero or less disables every NPC-holder teleport for that actor type, including load doors. Actor collision includes the holder and "
                "leashed actor. "
                "Solve iterations range from 1 to 128; higher values make the rope more rigid at greater performance cost.");
            ImGuiMCP::Spacing();
            RenderLocomotionSettings(locomotion);
            if (ImGuiMCP::CollapsingHeader("Advanced Physics")) {
                ImGuiMCP::InputFloat3("Gravity (X, Y, Z)", &simulation.gravity.x, "%.1f");
                if (ImGuiMCP::IsItemHovered()) {
                    ImGuiMCP::SetTooltip("Measured in Skyrim units per second squared.");
                }
                ImGuiMCP::SliderFloat("Damping", &simulation.damping, 0.0F, 1.0F, "%.3f");
                if (ImGuiMCP::IsItemHovered()) {
                    ImGuiMCP::SetTooltip("The retained-velocity factor: zero stops motion and one preserves it.");
                }
                ImGuiMCP::InputFloat("Collision padding", &simulation.collisionPadding, 0.1F, 1.0F, "%.2f");
                ImGuiMCP::InputFloat("Stretch compliance", &simulation.stretchCompliance, 1.0e-8F, 1.0e-7F, "%.2e");
                if (ImGuiMCP::IsItemHovered()) {
                    ImGuiMCP::SetTooltip("Higher values make the rope more elastic.");
                }
                ImGuiMCP::InputFloat("Snag release strain", &simulation.snagReleaseStrain, 0.005F, 0.02F, "%.3f");
                if (ImGuiMCP::IsItemHovered()) {
                    ImGuiMCP::SetTooltip("The segment stretch required before snag release can trigger.");
                }
                ImGuiMCP::InputFloat("Snag blocked distance", &simulation.snagBlockedDistance, 0.05F, 0.25F, "%.2f");
                if (ImGuiMCP::IsItemHovered()) {
                    ImGuiMCP::SetTooltip("The minimum collision-blocked movement required.");
                }
            }
            RenderActorBodyCollisionSettings(simulation.actorBodyCollision);
            manager.SetSimulationSettings(std::move(simulation));
            manager.SetPullPoseSettings(pullPose);
            manager.SetLocomotionSettings(locomotion);
            manager.SetRecoverySettings(recovery);
            manager.SetTeleportSettings(teleport);
            Hooks::FrameHook::SetSettings(frameHook);
        }

        void RenderDiagnostics() {
            ImGuiMCP::SeparatorText("Visualization and logging");
            if (ImGuiMCP::Checkbox("Draw actor collision", &actorCollisionDebugEnabled) && actorCollisionDebugEnabled) {
                DebugOverlay::Register();
            }
            if (ImGuiMCP::Checkbox("Enable debug logging", &debugSettings.enablePullDiagnostics)) {
                LeashManager::GetSingleton().SetPullDiagnosticsEnabled(debugSettings.enablePullDiagnostics);
            }
            static const std::string logFilePath = [] {
                if (const auto directory = SKSE::log::log_directory()) {
                    return (*directory / "LeashFramework.log").string();
                }
                return std::string{R"(C:\Users\%USERNAME%\Documents\My Games\Skyrim Special Edition\SKSE\LeashFramework.log)"};
            }();
            ImGuiMCP::TextWrapped("Log file: %s", logFilePath.c_str());
            if (ImGuiMCP::Button("Copy log file path")) {
                ImGuiMCP::SetClipboardText(logFilePath.c_str());
            }
        }

        void RenderEquipment() {
            RenderTestActors();
            const auto ownerID = debugSettings.holderOwnsLeash ? selectedHolder : selectedLeashed;
            ImGuiMCP::TextWrapped("Equip on: %s", DescribeActor(ownerID).c_str());
            MenuLayout::Columns("EquipmentTools", 30.0F, {RenderPluginArmorDropdown, RenderArmorEntries});
            MenuLayout::Feedback(armorStatus);
        }

        void __stdcall RenderDebugPage() {
            const auto em = ImGuiMCP::GetFontSize();
            ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_ItemSpacing, {em * 0.6F, em * 0.4F});
            ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_CellPadding, {em * 0.5F, em * 0.3F});
            ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_FramePadding, {em * 0.4F, em * 0.2F});
            if (ImGuiMCP::BeginTabBar("DebugTools", ImGuiMCP::ImGuiTabBarFlags_FittingPolicyScroll)) {
                if (ImGuiMCP::BeginTabItem("Test leash")) {
                    RenderApplyLeash();
                    ImGuiMCP::EndTabItem();
                }
                const auto definitions = LeashManager::GetSingleton().GetDefinitions();
                const auto activeLabel = std::format("Active leashes ({})###ActiveLeashesTab", definitions.size());
                if (ImGuiMCP::BeginTabItem(activeLabel.c_str())) {
                    RenderActiveLeashes(definitions);
                    MenuLayout::Feedback(activeStatus);
                    ImGuiMCP::EndTabItem();
                }
                if (ImGuiMCP::BeginTabItem("Equipment")) {
                    RenderEquipment();
                    ImGuiMCP::EndTabItem();
                }
                if (ImGuiMCP::BeginTabItem("Skeleton")) {
                    RenderTestActors();
                    const auto ownerID = debugSettings.holderOwnsLeash ? selectedHolder : selectedLeashed;
                    ImGuiMCP::TextWrapped("Inspect: %s", DescribeActor(ownerID).c_str());
                    RenderSkeletonDumper();
                    ImGuiMCP::EndTabItem();
                }
                if (ImGuiMCP::BeginTabItem("Diagnostics")) {
                    RenderDiagnostics();
                    ImGuiMCP::EndTabItem();
                }
                ImGuiMCP::EndTabBar();
            }
            ImGuiMCP::PopStyleVar(3);
        }
    }  // namespace

    bool IsActorCollisionDebugEnabled() { return actorCollisionDebugEnabled; }

    void Register() {
        LoadSettings();
        if (!SKSEMenuFramework::IsInstalled()) {
            SKSE::log::info("SKSE Menu Framework is not installed; mod menu disabled");
            return;
        }

        SKSEMenuFramework::SetSection("Leash Framework");
        SKSEMenuFramework::AddSectionItem("Settings", RenderSettingsPage);
        SKSEMenuFramework::AddSectionItem("Debug", RenderDebugPage);
        menuEvent = new SKSEMenuFramework::Model::Event(OnMenuEvent);
        SKSE::log::info("Registered Leash Framework mod menu");
    }
}  // namespace LeashFramework::UI::ModMenu
