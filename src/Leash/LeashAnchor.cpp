#include "LeashAnchor.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <type_traits>

#include "../PCH.h"

namespace LeashFramework {
    namespace {
        struct GripTransform {
            RE::NiPoint3 translate;
            std::array<RE::NiPoint3, 3> rotation;
        };

        struct ResolvedNode {
            RE::NiAVObject* root{};
            RE::NiAVObject* object{};
        };

        struct HandBoneNames {
            std::string_view hand;
            std::array<std::array<std::string_view, 3>, 5> fingers;
        };

        constexpr HandBoneNames kRightHandBones{.hand = "NPC R Hand [RHnd]",
            .fingers = {std::array<std::string_view, 3>{"NPC R Finger00 [RF00]", "NPC R Finger01 [RF01]", "NPC R Finger02 [RF02]"},
                std::array<std::string_view, 3>{"NPC R Finger10 [RF10]", "NPC R Finger11 [RF11]", "NPC R Finger12 [RF12]"},
                std::array<std::string_view, 3>{"NPC R Finger20 [RF20]", "NPC R Finger21 [RF21]", "NPC R Finger22 [RF22]"},
                std::array<std::string_view, 3>{"NPC R Finger30 [RF30]", "NPC R Finger31 [RF31]", "NPC R Finger32 [RF32]"},
                std::array<std::string_view, 3>{"NPC R Finger40 [RF40]", "NPC R Finger41 [RF41]", "NPC R Finger42 [RF42]"}}};
        constexpr HandBoneNames kLeftHandBones{.hand = "NPC L Hand [LHnd]",
            .fingers = {std::array<std::string_view, 3>{"NPC L Finger00 [LF00]", "NPC L Finger01 [LF01]", "NPC L Finger02 [LF02]"},
                std::array<std::string_view, 3>{"NPC L Finger10 [LF10]", "NPC L Finger11 [LF11]", "NPC L Finger12 [LF12]"},
                std::array<std::string_view, 3>{"NPC L Finger20 [LF20]", "NPC L Finger21 [LF21]", "NPC L Finger22 [LF22]"},
                std::array<std::string_view, 3>{"NPC L Finger30 [LF30]", "NPC L Finger31 [LF31]", "NPC L Finger32 [LF32]"},
                std::array<std::string_view, 3>{"NPC L Finger40 [LF40]", "NPC L Finger41 [LF41]", "NPC L Finger42 [LF42]"}}};
        constexpr std::size_t kMiddleFinger = 2;
        constexpr RE::NiPoint3 kGripOffset{0.0F, -2.5F, -1.5F};
        constexpr std::array<std::array<GripTransform, 3>, 5> kRightGripTransforms{
            std::array<GripTransform, 3>{GripTransform{.translate = {2.860F, -0.920F, 3.167F},
                                             .rotation = {RE::NiPoint3{-0.326688F, 0.944182F, 0.042387F}, RE::NiPoint3{-0.616443F, -0.178866F, -0.766815F}, RE::NiPoint3{-0.716431F, -0.276638F, 0.640467F}}},
                GripTransform{.translate = {2.998F, -3.402F, 5.240F},
                    .rotation = {RE::NiPoint3{-0.316206F, 0.854514F, -0.412093F}, RE::NiPoint3{-0.728440F, -0.496977F, -0.471582F}, RE::NiPoint3{-0.607774F, 0.151068F, 0.779608F}}},
                GripTransform{.translate = {1.828F, -4.740F, 7.451F},
                    .rotation = {RE::NiPoint3{-0.336266F, 0.128514F, -0.932958F}, RE::NiPoint3{-0.750199F, -0.635422F, 0.182866F}, RE::NiPoint3{-0.569321F, 0.761395F, 0.310082F}}}},
            std::array<GripTransform, 3>{GripTransform{.translate = {2.545F, -0.109F, 8.395F},
                                             .rotation = {RE::NiPoint3{0.960787F, -0.039090F, -0.274520F}, RE::NiPoint3{-0.250585F, 0.301499F, -0.919949F}, RE::NiPoint3{0.118729F, 0.952665F, 0.279881F}}},
                GripTransform{.translate = {1.870F, -2.371F, 9.083F},
                    .rotation = {RE::NiPoint3{0.960787F, -0.268334F, -0.069902F}, RE::NiPoint3{-0.250585F, -0.732278F, -0.633227F}, RE::NiPoint3{0.118729F, 0.625913F, -0.770803F}}},
                GripTransform{.translate = {1.738F, -3.569F, 7.624F},
                    .rotation = {RE::NiPoint3{0.960787F, -0.175073F, 0.215032F}, RE::NiPoint3{-0.250585F, -0.880236F, 0.402980F}, RE::NiPoint3{0.118729F, -0.441061F, -0.889589F}}}},
            std::array<GripTransform, 3>{GripTransform{.translate = {0.368F, -0.083F, 8.262F},
                                             .rotation = {RE::NiPoint3{0.993558F, -0.110608F, 0.024691F}, RE::NiPoint3{0.027978F, 0.028266F, -0.999209F}, RE::NiPoint3{0.109823F, 0.993462F, 0.031178F}}},
                GripTransform{.translate = {0.447F, -3.282F, 8.362F},
                    .rotation = {RE::NiPoint3{0.993558F, 0.002891F, 0.113293F}, RE::NiPoint3{0.027978F, -0.974990F, -0.220481F}, RE::NiPoint3{0.109823F, 0.222231F, -0.968789F}}},
                GripTransform{.translate = {0.672F, -3.721F, 6.434F},
                    .rotation = {RE::NiPoint3{0.993558F, 0.106034F, 0.040008F}, RE::NiPoint3{0.027978F, -0.571582F, 0.820068F}, RE::NiPoint3{0.109823F, -0.813665F, -0.570866F}}}},
            std::array<GripTransform, 3>{GripTransform{.translate = {-1.444F, -0.639F, 7.976F},
                                             .rotation = {RE::NiPoint3{0.980060F, -0.095225F, 0.174399F}, RE::NiPoint3{0.182920F, 0.089572F, -0.979039F}, RE::NiPoint3{0.077608F, 0.991418F, 0.105204F}}},
                GripTransform{.translate = {-0.978F, -3.255F, 8.257F},
                    .rotation = {RE::NiPoint3{0.980060F, 0.173180F, 0.097426F}, RE::NiPoint3{0.182920F, -0.977826F, -0.101960F}, RE::NiPoint3{0.077608F, 0.117748F, -0.990006F}}},
                GripTransform{.translate = {-0.783F, -3.460F, 6.270F},
                    .rotation = {RE::NiPoint3{0.980060F, 0.187421F, -0.066002F}, RE::NiPoint3{0.182920F, -0.721253F, 0.668082F}, RE::NiPoint3{0.077608F, -0.666833F, -0.741155F}}}},
            std::array<GripTransform, 3>{GripTransform{.translate = {-3.000F, -1.583F, 7.421F},
                                             .rotation = {RE::NiPoint3{0.930561F, -0.256161F, 0.261607F}, RE::NiPoint3{0.291213F, 0.084739F, -0.952898F}, RE::NiPoint3{0.221927F, 0.962912F, 0.153453F}}},
                GripTransform{.translate = {-2.540F, -3.261F, 7.691F},
                    .rotation = {RE::NiPoint3{0.930561F, 0.176124F, 0.320994F}, RE::NiPoint3{0.291213F, -0.887426F, -0.357311F}, RE::NiPoint3{0.221927F, 0.425977F, -0.877093F}}},
                GripTransform{.translate = {-1.967F, -3.898F, 6.127F},
                    .rotation = {RE::NiPoint3{0.930561F, 0.344939F, -0.122776F}, RE::NiPoint3{0.291213F, -0.494031F, 0.819224F}, RE::NiPoint3{0.221927F, -0.798091F, -0.560177F}}}}};
        constexpr std::array<std::array<GripTransform, 3>, 5> kLeftGripTransforms{
            std::array<GripTransform, 3>{GripTransform{.translate = {-2.859F, -0.922F, 3.167F},
                                             .rotation = {RE::NiPoint3{0.050744F, -0.902417F, -0.427865F}, RE::NiPoint3{0.778579F, 0.304063F, -0.548965F}, RE::NiPoint3{0.625493F, -0.305270F, 0.718032F}}},
                GripTransform{.translate = {-4.245F, -2.697F, 5.491F},
                    .rotation = {RE::NiPoint3{0.050744F, -0.908281F, 0.415272F}, RE::NiPoint3{0.778579F, -0.224443F, -0.586039F}, RE::NiPoint3{0.625493F, 0.353060F, 0.695780F}}},
                GripTransform{.translate = {-3.066F, -4.359F, 7.465F},
                    .rotation = {RE::NiPoint3{0.050744F, 0.013500F, 0.998621F}, RE::NiPoint3{0.778579F, -0.626777F, -0.031090F}, RE::NiPoint3{0.625493F, 0.779083F, -0.042316F}}}},
            std::array<GripTransform, 3>{GripTransform{.translate = {-2.544F, -0.109F, 8.395F},
                                             .rotation = {RE::NiPoint3{0.991762F, 0.127580F, -0.011536F}, RE::NiPoint3{-0.007963F, -0.028488F, -0.999563F}, RE::NiPoint3{-0.127852F, 0.991420F, -0.027238F}}},
                GripTransform{.translate = {-2.573F, -2.564F, 8.328F},
                    .rotation = {RE::NiPoint3{0.991762F, -0.003661F, -0.128048F}, RE::NiPoint3{-0.007963F, -0.999421F, -0.033092F}, RE::NiPoint3{-0.127852F, 0.033839F, -0.991217F}}},
                GripTransform{.translate = {-2.814F, -2.627F, 6.450F},
                    .rotation = {RE::NiPoint3{0.991762F, -0.113722F, 0.058966F}, RE::NiPoint3{-0.007963F, 0.404696F, 0.914417F}, RE::NiPoint3{-0.127852F, -0.907353F, 0.400457F}}}},
            std::array<GripTransform, 3>{GripTransform{.translate = {-0.368F, -0.080F, 8.263F},
                                             .rotation = {RE::NiPoint3{0.982890F, 0.158399F, -0.094010F}, RE::NiPoint3{-0.096663F, 0.009109F, -0.995276F}, RE::NiPoint3{-0.156794F, 0.987334F, 0.024264F}}},
                GripTransform{.translate = {-0.667F, -3.270F, 8.340F},
                    .rotation = {RE::NiPoint3{0.982890F, -0.084081F, -0.163885F}, RE::NiPoint3{-0.096663F, -0.992828F, -0.070354F}, RE::NiPoint3{-0.156794F, 0.084991F, -0.983968F}}},
                GripTransform{.translate = {-0.995F, -3.410F, 6.382F},
                    .rotation = {RE::NiPoint3{0.982890F, -0.168750F, 0.073834F}, RE::NiPoint3{-0.096663F, -0.131332F, 0.986615F}, RE::NiPoint3{-0.156794F, -0.976871F, -0.145396F}}}},
            std::array<GripTransform, 3>{GripTransform{.translate = {1.446F, -0.639F, 7.975F},
                                             .rotation = {RE::NiPoint3{0.969185F, 0.149523F, -0.195768F}, RE::NiPoint3{-0.180949F, -0.107129F, -0.977641F}, RE::NiPoint3{-0.167152F, 0.982938F, -0.076772F}}},
                GripTransform{.translate = {0.922F, -3.250F, 7.770F},
                    .rotation = {RE::NiPoint3{0.969185F, -0.186193F, -0.161289F}, RE::NiPoint3{-0.180949F, -0.982381F, 0.046749F}, RE::NiPoint3{-0.167152F, -0.016123F, -0.985800F}}},
                GripTransform{.translate = {0.598F, -3.154F, 5.791F},
                    .rotation = {RE::NiPoint3{0.969185F, -0.056356F, 0.239804F}, RE::NiPoint3{-0.180949F, 0.497683F, 0.848276F}, RE::NiPoint3{-0.167152F, -0.865528F, 0.472148F}}}},
            std::array<GripTransform, 3>{GripTransform{.translate = {3.001F, -1.582F, 7.420F},
                                             .rotation = {RE::NiPoint3{0.867086F, 0.409460F, -0.283733F}, RE::NiPoint3{-0.227767F, -0.180686F, -0.956805F}, RE::NiPoint3{-0.443040F, 0.894258F, -0.063409F}}},
                GripTransform{.translate = {2.502F, -3.268F, 7.309F},
                    .rotation = {RE::NiPoint3{0.867086F, -0.257991F, -0.426148F}, RE::NiPoint3{-0.227767F, -0.966113F, 0.121450F}, RE::NiPoint3{-0.443040F, -0.008245F, -0.896465F}}},
                GripTransform{.translate = {1.742F, -3.053F, 5.712F},
                    .rotation = {RE::NiPoint3{0.867086F, -0.182325F, 0.463594F}, RE::NiPoint3{-0.227767F, 0.682550F, 0.694442F}, RE::NiPoint3{-0.443040F, -0.707733F, 0.550302F}}}}};

        [[nodiscard]] bool IsDescendantOf(const RE::NiAVObject* a_object, const RE::NiAVObject* a_root) {
            for (auto* object = a_object; object; object = object->parent) {
                if (object == a_root) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] ResolvedNode ResolveActorNode(RE::Actor& a_actor, std::string_view a_name) {
            std::array<RE::NiAVObject*, 3> roots{a_actor.Get3D()};
            if (a_actor.IsPlayerRef()) {
                roots[1] = a_actor.Get3D(false);
                roots[2] = a_actor.Get3D(true);
            }

            RE::NiAVObject* firstRoot{};
            for (std::size_t index = 0; index < roots.size(); ++index) {
                auto* root = roots[index];
                const auto previousEnd = roots.begin() + static_cast<std::ptrdiff_t>(index);
                if (!root || std::ranges::find(roots.begin(), previousEnd, root) != previousEnd) {
                    continue;
                }
                firstRoot = firstRoot ? firstRoot : root;
                if (auto* object = root->GetObjectByName(RE::BSFixedString(a_name))) {
                    return {.root = root, .object = object};
                }
            }
            return {.root = firstRoot};
        }

    }  // namespace

    LeashAnchor::LeashAnchor(const LeashDefinition& a_definition) : _definition(a_definition) {
        if (auto* holder = RE::TESForm::LookupByID<RE::Actor>(_definition.holderFormID)) {
            _holder = holder->GetHandle();
        }
    }

    LeashAnchor::BindResult LeashAnchor::Bind() {
        return std::visit([&](const auto& a_anchor) { return Bind(a_anchor); }, _definition.anchor);
    }

    LeashAnchor::BindResult LeashAnchor::Bind(const HandAnchor& a_anchor) {
        const auto& boneNames = a_anchor.rightHand ? kRightHandBones : kLeftHandBones;
        auto* holder = ResolveHolder();
        const auto resolved = holder ? ResolveActorNode(*holder, boneNames.hand) : ResolvedNode{};
        const auto nodesAttached = _hand && IsDescendantOf(_hand.get(), resolved.root) && std::ranges::all_of(_fingers, [&](const auto& a_finger) {
            return std::ranges::all_of(a_finger, [&](const auto& a_bone) { return !a_bone || IsDescendantOf(a_bone.get(), resolved.root); });
        });
        if (_boundRoot.get() == resolved.root && _hand.get() == resolved.object && nodesAttached && _fingers[kMiddleFinger][0]) {
            return BindResult::kUnchanged;
        }

        Reset();
        if (!holder || !resolved.root || !resolved.object) {
            if (!_bindingWarningLogged) {
                SKSE::log::warn("Unable to bind leash holder {:08X}: {} hand '{}' was not found", _definition.holderFormID, a_anchor.rightHand ? "right" : "left", boneNames.hand);
                _bindingWarningLogged = true;
            }
            return BindResult::kFailed;
        }

        _boundRoot.reset(resolved.root);
        _hand.reset(resolved.object);
        for (std::size_t fingerIndex = 0; fingerIndex < boneNames.fingers.size(); ++fingerIndex) {
            for (std::size_t boneIndex = 0; boneIndex < boneNames.fingers[fingerIndex].size(); ++boneIndex) {
                _fingers[fingerIndex][boneIndex].reset(resolved.root->GetObjectByName(RE::BSFixedString(boneNames.fingers[fingerIndex][boneIndex])));
            }
        }
        if (!_fingers[kMiddleFinger][0]) {
            Reset();
            if (!_bindingWarningLogged) {
                SKSE::log::warn("Unable to bind leash holder {:08X}: grip anchor '{}' was not found", _definition.holderFormID, boneNames.fingers[kMiddleFinger][0]);
                _bindingWarningLogged = true;
            }
            return BindResult::kFailed;
        }

        _bindingWarningLogged = false;
        return BindResult::kChanged;
    }

    LeashAnchor::BindResult LeashAnchor::Bind(const ActorBoneAnchor& a_anchor) {
        auto* holder = ResolveHolder();
        const auto resolved = holder ? ResolveActorNode(*holder, a_anchor.boneName) : ResolvedNode{};
        if (_boundRoot.get() == resolved.root && _anchorNode.get() == resolved.object && _anchorNode && IsDescendantOf(_anchorNode.get(), resolved.root)) {
            return BindResult::kUnchanged;
        }

        Reset();
        if (!holder || !resolved.root || !resolved.object) {
            if (!_bindingWarningLogged) {
                SKSE::log::warn("Unable to bind leash holder {:08X}: bone '{}' was not found", _definition.holderFormID, a_anchor.boneName);
                _bindingWarningLogged = true;
            }
            return BindResult::kFailed;
        }

        _boundRoot.reset(resolved.root);
        _anchorNode.reset(resolved.object);
        _bindingWarningLogged = false;
        return BindResult::kChanged;
    }

    LeashAnchor::BindResult LeashAnchor::Bind(const WorldPositionAnchor& a_anchor) {
        auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(a_anchor.cellFormID);
        if (_worldCell == cell && cell) {
            return BindResult::kUnchanged;
        }

        Reset();
        if (!cell) {
            if (!_bindingWarningLogged) {
                SKSE::log::warn("Unable to bind world leash anchor: cell {:08X} was not found", a_anchor.cellFormID);
                _bindingWarningLogged = true;
            }
            return BindResult::kFailed;
        }

        _worldCell = cell;
        _bindingWarningLogged = false;
        return BindResult::kChanged;
    }

    RE::Actor* LeashAnchor::ResolveHolder() {
        if (auto holder = _holder.get()) {
            return holder.get();
        }
        auto* holder = RE::TESForm::LookupByID<RE::Actor>(_definition.holderFormID);
        if (holder) {
            _holder = holder->GetHandle();
        }
        return holder;
    }

    std::optional<LeashAnchor::Sample> LeashAnchor::GetSample() const {
        return std::visit(
            [&](const auto& a_anchor) -> std::optional<Sample> {
                using Anchor = std::decay_t<decltype(a_anchor)>;
                if constexpr (std::is_same_v<Anchor, HandAnchor>) {
                    auto holder = _holder.get();
                    if (!holder || !_hand || !_fingers[kMiddleFinger][0]) {
                        return std::nullopt;
                    }
                    const auto position = _fingers[kMiddleFinger][0]->world.translate + _hand->world.rotate * kGripOffset * _hand->world.scale;
                    return Sample{.position = position, .pullGoal = holder->GetPosition(), .cell = holder->GetParentCell()};
                } else if constexpr (std::is_same_v<Anchor, ActorBoneAnchor>) {
                    auto holder = _holder.get();
                    if (!holder || !_anchorNode) {
                        return std::nullopt;
                    }
                    return Sample{.position = _anchorNode->world.translate, .pullGoal = holder->GetPosition(), .cell = holder->GetParentCell()};
                } else {
                    return Sample{.position = {a_anchor.x, a_anchor.y, a_anchor.z}, .pullGoal = {a_anchor.x, a_anchor.y, a_anchor.z}, .cell = _worldCell};
                }
            },
            _definition.anchor);
    }

    void LeashAnchor::ApplyPose() {
        const auto* handAnchor = std::get_if<HandAnchor>(&_definition.anchor);
        if (!handAnchor || !_hand) {
            return;
        }
        for (const auto& finger : _fingers) {
            if (std::ranges::contains(finger, nullptr)) {
                return;
            }
        }

        const auto& gripTransforms = handAnchor->rightHand ? kRightGripTransforms : kLeftGripTransforms;
        for (std::size_t fingerIndex = 0; fingerIndex < _fingers.size(); ++fingerIndex) {
            for (std::size_t boneIndex = 0; boneIndex < _fingers[fingerIndex].size(); ++boneIndex) {
                const auto& target = gripTransforms[fingerIndex][boneIndex];
                const RE::NiMatrix3 targetRotation{target.rotation[0], target.rotation[1], target.rotation[2]};
                auto* bone = _fingers[fingerIndex][boneIndex].get();
                bone->world.translate = _hand->world * target.translate;
                bone->world.rotate = _hand->world.rotate * targetRotation;
            }
        }
    }

    void LeashAnchor::Reset() {
        _fingers = {};
        _hand.reset();
        _anchorNode.reset();
        _boundRoot.reset();
        _worldCell = nullptr;
    }
}  // namespace LeashFramework
