#include "SettingsPage.h"

#include <algorithm>

#include "MenuLayout.h"

namespace LeashFramework::UI::SettingsPage {
    namespace {
        using MenuLayout::Columns;
        using MenuLayout::Defaults;
        using MenuLayout::Field;
        using MenuLayout::Heading;
        using MenuLayout::Note;
        using MenuLayout::Number;
        using MenuLayout::Panel;
        using MenuLayout::Slider;
        using MenuLayout::Style;
        using MenuLayout::Tab;
        using MenuLayout::Toggle;
        using MenuLayout::Vector;

        void Behavior(Values a_settings) {
            Columns(
                [&] {
                    auto& behavior = a_settings.behavior;
                    Panel("Camera & interaction", "Everyday behavior while leashed.", [&] {
                        Toggle("Free camera while AI-controlled", behavior.freeCameraWhileAIControlled, "Rotate the camera freely while AI Mode or forced walking controls player movement.");
                        Toggle("Suppress greetings while leashed", behavior.suppressGreetingsWhileLeashed, "Suppress greetings when the target actor or the player is leashed.");
                    }, Defaults(behavior));
                    auto& recovery = a_settings.recovery;
                    Panel("Ragdoll recovery", "Physically pull actors back when they stray too far.", [&] {
                        Toggle("Ragdoll NPCs", recovery.enableNPCs, "Allow forced ragdoll recovery for leashed NPCs.");
                        Toggle("Ragdoll player", recovery.enablePlayer, "Allow forced ragdoll recovery for the leashed player.");
                        ImGuiMCP::BeginDisabled(!recovery.enableNPCs && !recovery.enablePlayer);
                        Slider("Trigger distance multiplier", recovery.distanceMultiplier, 1.0F, 10.0F,
                            "Triggers beyond maximum leash length multiplied by this value. Ragdoll recovery takes priority over normal pulling.", "%.2fx");
                        ImGuiMCP::EndDisabled();
                    }, Defaults(recovery));
                },
                [&] {
                    auto& teleport = a_settings.teleport;
                    Panel("Teleport recovery", "Let NPC holders bring separated actors back after a short delay.", [&] {
                        Slider("Grace period", teleport.gracePeriod, 0.0F, 30.0F, "How long separation must persist before an eligible NPC holder teleports the leashed actor.", "%.2f s");
                        Number("Player: extra distance", teleport.playerDistance, "Allowed distance beyond maximum leash length before an NPC holder teleports a leashed, AI-driven player.", 10.0F, 100.0F, "%.1f units");
                        Number("NPCs: extra distance", teleport.npcDistance, "Allowed distance beyond maximum leash length before an NPC holder teleports a leashed NPC.", 10.0F, 100.0F, "%.1f units");
                        Note("Set a distance to 0 or less to disable NPC-holder teleports for that actor type, including load doors.");
                    }, Defaults(teleport));
                });
        }

        void Movement(Values a_settings) {
            Columns(
                [&] {
                    auto& movement = a_settings.locomotion;
                    Panel("Locomotion", "Control catch-up speed and how player input affects pulling.", [&] {
                        Slider("Forward assistance", movement.forwardAssistance, 0.0F, 3.0F,
                            "Extra catch-up speed when the player presses toward the pull path. Zero disables the boost. A speed of 2.00 is the running-speed reference.");
                        Slider("Backward resistance", movement.backwardResistance, 0.0F, 3.0F,
                            "Reduces pull speed when the player presses against the pull direction, subject to the minimum forced-pull ratio. Zero disables resistance.");
                        Slider("Minimum forced-pull ratio", movement.minimumForcedPullRatio, 0.0F, 1.0F,
                            "Fraction of automatic pull speed that resistance cannot remove at maximum length. Fades toward zero near minimum length. Zero removes the floor without "
                            "disabling pulling.");
                        Slider("Maximum catch-up speed", movement.maximumCatchUpSpeed, 0.25F, 10.0F,
                            "Caps automatic locomotion for players and NPCs. Player assistance can raise the cap. A speed of 2.00 is the running-speed reference. Does not affect ragdoll "
                            "pulling.");
                        Slider("Moving follow gap", movement.movingFollowGap, 0.0F, 1.0F,
                            "Preferred gap while an actor holder moves: 0 targets minimum leash length, 1 targets maximum. With lengths 200 and 300, 0.40 targets 240 units. Does not change "
                            "pull/release thresholds or world anchors.");
                        Slider("Distance response rate", movement.distanceResponseRate, 0.1F, 10.0F, "Higher values close the gap faster. Arrival braking and maximum catch-up speed still apply.");
                    }, Defaults(movement));
                },
                [&] {
                    auto& pose = a_settings.pose;
                    Panel("Pulling pose", "Lean into a taut leash with a procedural spine pose.", [&] {
                        Toggle("Enable procedural pulling pose", pose.enabled, "Apply a procedural leaning pose while the leash is under tension.");
                        ImGuiMCP::BeginDisabled(!pose.enabled);
                        Slider("Minimum pull strength", pose.minimumStrength, 0.0F, 1.0F, "Minimum pose strength as tension builds. Maximum strength cannot be lower than this value.");
                        pose.maximumStrength = std::max(pose.maximumStrength, pose.minimumStrength);
                        Slider("Maximum pull strength", pose.maximumStrength, pose.minimumStrength, 1.0F, "Upper limit on procedural pose strength.");
                        Slider("Maximum spine angle", pose.maximumAngleDegrees, 0.0F, 50.0F, "Maximum total spine bend at full strength.", "%.1f degrees");
                        Slider("Response rate", pose.responseRate, 0.1F, 30.0F, "How quickly the pose responds to changes in tension. Higher values react faster.", "%.1f");
                        Slider("Anticipation time", pose.anticipationTime, 0.0F, 0.3F,
                            "Predicts endpoint separation so leaning reacts earlier. Zero disables prediction. Capped at 15% of actual chain length or 32 units. Does not change the "
                            "movement pull threshold.", "%.2f s");
                        Slider("Slack reserve ratio", pose.slackReserveRatio, 0.0F, 0.1F,
                            "Start leaning before the chain is fully taut. 0.03 reserves 3% of its actual length, capped at 8 units. Zero disables the reserve. Does not shorten the chain "
                            "or change the movement pull threshold.", "%.3f");
                        ImGuiMCP::EndDisabled();
                    }, Defaults(pose));
                });
        }

        void Rope(Physics::SimulationSettings& a_settings) {
            if (Heading("Rope physics", "Reset rope physics", "Restores rope physics defaults, including the actor collision toggle. Keeps your body shapes.")) {
                a_settings = Physics::SimulationSettings{.actorBodyCollision = a_settings.actorBodyCollision};
            }
            Columns(
                [&] {
                    Panel("Solver & motion", "Balance rope rigidity, movement and performance.", [&] {
                        Field("Solve iterations", [&](const char* a_id) {
                            return ImGuiMCP::SliderScalar(a_id, ImGuiMCP::ImGuiDataType_U32, &a_settings.constraintIterations, &Physics::SimulationSettings::kMinimumConstraintIterations,
                                &Physics::SimulationSettings::kMaximumConstraintIterations, "%u", ImGuiMCP::ImGuiSliderFlags_AlwaysClamp);
                        }, "Higher values make the rope more rigid at greater performance cost. Range: 1-128.");
                        Vector("Gravity (X / Y / Z)", a_settings.gravity, "Acceleration in Skyrim units per second squared.");
                        Slider("Damping", a_settings.damping, 0.0F, 1.0F, "Retained velocity: 0 stops motion, 1 preserves it.", "%.3f");
                        Number("Stretch compliance", a_settings.stretchCompliance, "Higher values make the rope more elastic.", 1.0e-8F, 1.0e-7F, "%.2e");
                    });
                },
                [&] {
                    Panel("Contact & snag release", "Fine-tune how the rope responds to obstacles.", [&] {
                        Number("Collision padding", a_settings.collisionPadding, "Extra clearance around rope collision contacts.", 0.1F, 2.0F, "%.2f units");
                        Number("Snag release strain", a_settings.snagReleaseStrain, "Segment stretch required before snag release can trigger.", 0.005F, 0.02F, "%.3f");
                        Number("Snag blocked distance", a_settings.snagBlockedDistance, "Minimum collision-blocked movement required for snag release.", 0.05F, 0.25F, "%.2f units");
                        Note("Actor collision and body shapes are in the Body collision tab.");
                    });
                });
        }

        void Capsule(const char* a_title, const char* a_bone, Physics::ActorBodyCapsuleSettings& a_settings) {
            Panel(a_title, a_bone, [&] {
                Vector("Local offset (X / Y / Z)", a_settings.offset, "Offset in bone-local coordinates.");
                Number("Radius", a_settings.radius, "Capsule radius in Skyrim units.", 0.25F, 1.0F);
                Number("Width", a_settings.width, "Distance between capsule cap centers along the bone's local X axis.", 0.5F, 2.0F);
                a_settings.radius = std::max(a_settings.radius, 0.0F);
                a_settings.width = std::max(a_settings.width, 0.0F);
            });
        }

        void BodyShapes(Physics::ActorBodySexSettings& a_settings, Physics::ActorBodyBreastSettings* a_breasts = nullptr) {
            Columns(
                [&] { Capsule("Lower spine", "NPC Spine [Spn0]", a_settings.spine); },
                [&] { Capsule("Middle spine", "NPC Spine1 [Spn1]", a_settings.spine1); },
                [&] { Capsule("Upper spine", "NPC Spine2 [Spn2]", a_settings.spine2); },
                [&] { Capsule("Neck", "NPC Neck [Neck]", a_settings.neck); });
            if (a_breasts) {
                ImGuiMCP::PushID("BreastShapes");
                Columns([&] {
                    Panel("Breasts", "Shared spherical collision settings.", [&] {
                        Vector("Local offset (X / Y / Z)", a_breasts->offset, "Offset in bone-local coordinates.");
                        Number("Radius", a_breasts->radius, "Sphere radius in Skyrim units.", 0.25F, 1.0F);
                        a_breasts->radius = std::max(a_breasts->radius, 0.0F);
                    });
                });
                ImGuiMCP::PopID();
            }
        }

        void Body(Physics::SimulationSettings& a_settings) {
            if (Heading("Actor body collision", "Reset body shapes")) {
                a_settings.actorBodyCollision = {};
            }
            Toggle("Collide with actors", a_settings.collideWithActors, "Enable rope collision with actor bodies, including the holder and leashed actor.");
            Note("Capsules follow each bone's local X axis. Offsets are bone-local; width is the distance between cap centers.");
            if (!a_settings.collideWithActors) {
                Note("Actor collision is off. You can still edit the shapes below.");
            }
            if (ImGuiMCP::BeginTabBar("BodyProfiles")) {
                if (ImGuiMCP::BeginTabItem("Male")) {
                    ImGuiMCP::PushID("Male");
                    BodyShapes(a_settings.actorBodyCollision.male);
                    ImGuiMCP::PopID();
                    ImGuiMCP::EndTabItem();
                }
                if (ImGuiMCP::BeginTabItem("Female")) {
                    ImGuiMCP::PushID("Female");
                    BodyShapes(a_settings.actorBodyCollision.female, &a_settings.actorBodyCollision.femaleBreast);
                    ImGuiMCP::PopID();
                    ImGuiMCP::EndTabItem();
                }
                ImGuiMCP::EndTabBar();
            }
        }


    }

    bool Render(Values a_settings) {
        const Style style;
        ImGuiMCP::PushID("LeashSettings");
        const bool resetAll = Heading("LEASH FRAMEWORK", "Reset all settings", "Resets all saved settings, including Debug settings, to their defaults and saves immediately.");
        Note("Changes apply immediately and save when the menu closes. Hover labels or controls for details.");
        ImGuiMCP::Spacing();
        if (ImGuiMCP::BeginTabBar("Settings", ImGuiMCP::ImGuiTabBarFlags_FittingPolicyScroll)) {
            Tab("Behavior", [&] { Behavior(a_settings); });
            Tab("Movement", [&] { Movement(a_settings); });
            Tab("Rope physics", [&] { Rope(a_settings.simulation); });
            Tab("Body collision", [&] { Body(a_settings.simulation); });
            ImGuiMCP::EndTabBar();
        }
        ImGuiMCP::PopID();
        return resetAll;
    }
}
