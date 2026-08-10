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
        DebugAnchorType selectedAnchorType{DebugAnchorType::kRightHand};
        char selectedAttachmentBone[128]{};
        RE::NiPoint3 selectedWorldPosition{};
        std::uint32_t selectedWorldCellFormID{};
        bool actorsLoaded{};
        bool actorCollisionDebugEnabled{};
        DebugSettings debugSettings;
        std::string status;
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
            if (!ImGuiMCP::BeginCombo(a_label, preview)) {
                return;
            }

            for (const auto& actor : actorOptions) {
                ImGuiMCP::PushID(static_cast<int>(actor.formID));
                const bool isSelected = actor.formID == a_selectedFormID;
                if (ImGuiMCP::Selectable(actor.label.c_str(), isSelected)) {
                    a_selectedFormID = actor.formID;
                }
                if (isSelected) {
                    ImGuiMCP::SetItemDefaultFocus();
                }
                ImGuiMCP::PopID();
            }
            ImGuiMCP::EndCombo();
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
                status = "Select an available physical leash owner before dumping the skeleton.";
                return;
            }

            auto* root = actor->Get3D(false);
            if (!root) {
                status = std::format("{} has no loaded third-person skeleton.", DescribeActor(actor));
                return;
            }

            auto* npcObject = root->GetObjectByName(RE::BSFixedString("NPC"));
            auto* npcNode = npcObject ? npcObject->AsNode() : nullptr;
            if (!npcNode) {
                status = std::format("{} has no loaded NPC skeleton node.", DescribeActor(actor));
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
            status = std::format("Displayed {} skeleton node(s) for {}.", nodeCount, skeletonDumpActor);
        }

        void RenderSkeletonDumper() {
            ImGuiMCP::SeparatorText("Skeleton visualizer");
            ImGuiMCP::TextWrapped("Shows skinned bones beneath the physical leash owner's NPC node. Current parent and leash matches are highlighted.");
            if (ImGuiMCP::Button("Dump physical owner's skeleton")) {
                DumpSelectedSkeleton();
            }
            if (skeletonDump.empty()) {
                return;
            }

            ImGuiMCP::SameLine();
            ImGuiMCP::TextUnformatted(skeletonDumpActor.c_str());
            ImGuiMCP::InputTextWithHint("##SkeletonFilter", "Filter bone names", skeletonFilter, sizeof(skeletonFilter));
            if (ImGuiMCP::BeginChild("SkeletonDump", {0.0F, 1080.0F}, ImGuiMCP::ImGuiChildFlags_Border)) {
                const std::string_view filter{skeletonFilter};
                for (const auto& root : skeletonDump) {
                    RenderSkeletonNode(root, filter);
                }
            }
            ImGuiMCP::EndChild();
        }

        void RenderArmorEntries() {
            ImGuiMCP::SeparatorText("Equip leash armor");
            constexpr auto tableFlags = ImGuiMCP::ImGuiTableFlags_Borders | ImGuiMCP::ImGuiTableFlags_RowBg | ImGuiMCP::ImGuiTableFlags_SizingStretchSame;
            if (!ImGuiMCP::BeginTable("LeashArmor", 3, tableFlags)) {
                return;
            }

            ImGuiMCP::TableSetupColumn("Mod name", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn("ARMO Local FormID", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn("", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed);
            ImGuiMCP::TableHeadersRow();
            for (std::size_t index = 0; index < debugSettings.armorEntries.size(); ++index) {
                auto& entry = debugSettings.armorEntries[index];
                ImGuiMCP::PushID(static_cast<int>(index));
                ImGuiMCP::TableNextRow();
                ImGuiMCP::TableSetColumnIndex(0);
                ImGuiMCP::SetNextItemWidth(-1.0F);
                ImGuiMCP::InputText("##ModName", entry.modName, sizeof(entry.modName));
                ImGuiMCP::TableSetColumnIndex(1);
                ImGuiMCP::SetNextItemWidth(-1.0F);
                ImGuiMCP::InputText("##FormID", entry.formID, sizeof(entry.formID));
                ImGuiMCP::TableSetColumnIndex(2);
                if (ImGuiMCP::Button("Equip")) {
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
                        } else if (auto* equipManager = RE::ActorEquipManager::GetSingleton(); !equipManager) {
                            armorStatus = "The actor equip manager is unavailable.";
                        } else {
                            auto* armor = static_cast<RE::TESObjectARMO*>(form);
                            const auto inventory = meshOwner->GetInventoryCounts();
                            const auto item = inventory.find(armor);
                            if (item == inventory.end() || item->second <= 0) {
                                meshOwner->AddObjectToContainer(armor, nullptr, 1, nullptr);
                            }
                            equipManager->EquipObject(meshOwner, armor, nullptr, 1, nullptr, true, true);
                            const auto* armorName = armor->GetName();
                            armorStatus = std::format("Equipped {} ({:08X}) on {}.", armorName && armorName[0] != '\0' ? armorName : "unnamed armor", resolvedFormID, DescribeActor(meshOwner));
                        }
                    }
                }
                ImGuiMCP::PopID();
            }
            ImGuiMCP::EndTable();
            if (!armorStatus.empty()) {
                ImGuiMCP::Spacing();
                ImGuiMCP::TextWrapped("%s", armorStatus.c_str());
            }
        }

        void RenderActiveLeashes() {
            ImGuiMCP::SeparatorText("Active leashes");
            const auto definitions = LeashManager::GetSingleton().GetDefinitions();
            if (definitions.empty()) {
                ImGuiMCP::TextUnformatted("No actors are currently leashed.");
                return;
            }

            constexpr auto tableFlags = ImGuiMCP::ImGuiTableFlags_Borders | ImGuiMCP::ImGuiTableFlags_RowBg | ImGuiMCP::ImGuiTableFlags_SizingStretchProp;
            if (!ImGuiMCP::BeginTable("ActiveLeashes", 4, tableFlags)) {
                return;
            }

            ImGuiMCP::TableSetupColumn("Leashed actor", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn("Leasher", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn("Physical owner", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn("", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed);
            ImGuiMCP::TableHeadersRow();
            for (const auto& definition : definitions) {
                ImGuiMCP::TableNextRow();
                ImGuiMCP::TableSetColumnIndex(0);
                const auto leashedLabel = DescribeActor(definition.leashedFormID);
                ImGuiMCP::TextUnformatted(leashedLabel.c_str());
                ImGuiMCP::TableSetColumnIndex(1);
                const auto holderLabel = definition.holderFormID != 0 ? DescribeActor(definition.holderFormID) : std::string{"World position"};
                ImGuiMCP::TextUnformatted(holderLabel.c_str());
                ImGuiMCP::TableSetColumnIndex(2);
                const auto meshOwnerFormID = definition.meshOwner == LeashMeshOwner::kHolder ? definition.holderFormID : definition.leashedFormID;
                const auto meshOwnerLabel = DescribeActor(meshOwnerFormID);
                ImGuiMCP::TextUnformatted(meshOwnerLabel.c_str());
                ImGuiMCP::TableSetColumnIndex(3);
                ImGuiMCP::PushID(static_cast<int>(definition.leashedFormID));
                if (ImGuiMCP::Button("Free")) {
                    auto* leashed = RE::TESForm::LookupByID<RE::Actor>(definition.leashedFormID);
                    auto* holder = RE::TESForm::LookupByID<RE::Actor>(definition.holderFormID);
                    const auto disconnected = LeashManager::GetSingleton().Disconnect(holder, leashed);
                    status = disconnected ? std::format("Freed {}.", leashedLabel) : std::format("Could not free {}.", leashedLabel);
                }
                ImGuiMCP::PopID();
            }
            ImGuiMCP::EndTable();
        }

        void RenderApplyLeash() {
            ImGuiMCP::SeparatorText("Create test leash");
            if (!actorsLoaded) {
                RefreshActors();
            }
            if (ImGuiMCP::Button("Refresh nearby actors")) {
                RefreshActors();
            }
            ImGuiMCP::SameLine();
            ImGuiMCP::Text("%zu actor(s)", actorOptions.size());

            RenderActorDropdown("Leashed", selectedLeashed);
            const auto meshOwnerIndex = debugSettings.holderOwnsLeash ? 1U : 0U;
            if (ImGuiMCP::BeginCombo("Physical leash owner", kMeshOwnerLabels[meshOwnerIndex])) {
                for (std::size_t index = 0; index < kMeshOwnerLabels.size(); ++index) {
                    const bool isSelected = index == meshOwnerIndex;
                    if (ImGuiMCP::Selectable(kMeshOwnerLabels[index], isSelected) && !isSelected) {
                        debugSettings.holderOwnsLeash = index == 1;
                        if (debugSettings.holderOwnsLeash) {
                            selectedAnchorType = DebugAnchorType::kActorBone;
                        }
                        applyStatus.clear();
                    }
                    if (isSelected) {
                        ImGuiMCP::SetItemDefaultFocus();
                    }
                }
                ImGuiMCP::EndCombo();
            }

            if (debugSettings.holderOwnsLeash) {
                RenderActorDropdown("Leasher", selectedHolder);
                ImGuiMCP::InputText("Leashed attachment bone", selectedAttachmentBone, sizeof(selectedAttachmentBone));
                ImGuiMCP::InputFloat3("Attachment offset (X, Y, Z)", &debugSettings.attachmentOffset.x, "%.2f");
                const auto closedHandIndex = debugSettings.closedHand == 1 || debugSettings.closedHand == 2 ? static_cast<std::size_t>(debugSettings.closedHand) : 0U;
                if (ImGuiMCP::BeginCombo("Closed leasher hand", kClosedHandLabels[closedHandIndex])) {
                    for (std::size_t index = 0; index < kClosedHandLabels.size(); ++index) {
                        const bool isSelected = index == closedHandIndex;
                        if (ImGuiMCP::Selectable(kClosedHandLabels[index], isSelected)) {
                            debugSettings.closedHand = static_cast<int>(index);
                        }
                        if (isSelected) {
                            ImGuiMCP::SetItemDefaultFocus();
                        }
                    }
                    ImGuiMCP::EndCombo();
                }
            } else {
                const auto selectedAnchorIndex = static_cast<std::size_t>(selectedAnchorType);
                if (ImGuiMCP::BeginCombo("Leash anchor", kDebugAnchorLabels[selectedAnchorIndex])) {
                    for (std::size_t index = 0; index < kDebugAnchorLabels.size(); ++index) {
                        const auto anchorType = static_cast<DebugAnchorType>(index);
                        const bool isSelected = anchorType == selectedAnchorType;
                        if (ImGuiMCP::Selectable(kDebugAnchorLabels[index], isSelected) && !isSelected) {
                            selectedAnchorType = anchorType;
                            applyStatus.clear();
                            if (selectedAnchorType == DebugAnchorType::kWorldPosition && !CapturePlayerWorldAnchor()) {
                                applyStatus = "Could not capture the player position and cell.";
                            }
                        }
                        if (isSelected) {
                            ImGuiMCP::SetItemDefaultFocus();
                        }
                    }
                    ImGuiMCP::EndCombo();
                }

                if (selectedAnchorType == DebugAnchorType::kWorldPosition) {
                    ImGuiMCP::InputFloat3("Anchor position (X, Y, Z)", &selectedWorldPosition.x, "%.2f");
                    ImGuiMCP::Text("Anchor cell FormID: %08X", selectedWorldCellFormID);
                    if (ImGuiMCP::Button("Use current player position")) {
                        applyStatus = CapturePlayerWorldAnchor() ? "Captured the current player position and cell." : "Could not capture the player position and cell.";
                    }
                } else {
                    RenderActorDropdown("Leasher", selectedHolder);
                    if (selectedAnchorType == DebugAnchorType::kActorBone) {
                        ImGuiMCP::InputText("Leasher attachment bone", selectedAttachmentBone, sizeof(selectedAttachmentBone));
                        ImGuiMCP::InputFloat3("Attachment offset (X, Y, Z)", &debugSettings.attachmentOffset.x, "%.2f");
                    }
                }
            }
            ImGuiMCP::InputText("Leash parent bone", debugSettings.parentBone, sizeof(debugSettings.parentBone));
            ImGuiMCP::InputText("Leash bone match", debugSettings.leashBoneMatch, sizeof(debugSettings.leashBoneMatch));
            ImGuiMCP::InputFloat("Minimum length", &debugSettings.minLength, 1.0F, 10.0F);
            ImGuiMCP::InputFloat("Maximum length", &debugSettings.maxLength, 1.0F, 10.0F);
            ImGuiMCP::Checkbox("Persistent", &debugSettings.persistent);

            if (ImGuiMCP::Button("Leash actor")) {
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
            if (!applyStatus.empty()) {
                ImGuiMCP::SameLine();
                ImGuiMCP::TextWrapped("%s", applyStatus.c_str());
            }

            ImGuiMCP::Spacing();
            ImGuiMCP::Spacing();
            RenderArmorEntries();
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

        void __stdcall RenderSettingsPage() {
            auto& manager = LeashManager::GetSingleton();
            auto simulation = manager.GetSimulationSettings();
            auto pullPose = manager.GetPullPoseSettings();
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
                ImGuiMCP::TextWrapped("Higher response rates follow tension changes faster.");
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
            manager.SetRecoverySettings(recovery);
            manager.SetTeleportSettings(teleport);
            Hooks::FrameHook::SetSettings(frameHook);
        }

        void __stdcall RenderDebugPage() {
            if (ImGuiMCP::Checkbox("Draw actor collision", &actorCollisionDebugEnabled) && actorCollisionDebugEnabled) {
                DebugOverlay::Register();
            }
            if (ImGuiMCP::Checkbox("Enable Debug Logging", &debugSettings.enablePullDiagnostics)) {
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
            ImGuiMCP::Spacing();
            RenderActiveLeashes();
            ImGuiMCP::Spacing();
            ImGuiMCP::Spacing();
            RenderApplyLeash();
            ImGuiMCP::Dummy({0.0F, ImGuiMCP::GetTextLineHeight() * 3.0F});
            RenderSkeletonDumper();
            if (!status.empty()) {
                ImGuiMCP::Spacing();
                ImGuiMCP::TextWrapped("%s", status.c_str());
            }
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
