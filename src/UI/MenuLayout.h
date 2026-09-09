#pragma once

#include "../PCH.h"
#include "../../include/SKSEMenuFramework.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <type_traits>
#include <utility>

namespace LeashFramework::UI::MenuLayout {
    inline constexpr ImGuiMCP::ImVec4 kAccent{0.72F, 0.16F, 0.14F, 1.0F};

    inline const std::array kColors{
        std::pair{ImGuiMCP::ImGuiCol_Text, ImGuiMCP::ImVec4{0.93F, 0.93F, 0.90F, 1.0F}},
        std::pair{ImGuiMCP::ImGuiCol_TextDisabled, ImGuiMCP::ImVec4{0.66F, 0.67F, 0.67F, 1.0F}},

        std::pair{ImGuiMCP::ImGuiCol_ChildBg, ImGuiMCP::ImVec4{0.04F, 0.04F, 0.04F, 0.65F}},
        std::pair{ImGuiMCP::ImGuiCol_Border, ImGuiMCP::ImVec4{0.52F, 0.16F, 0.14F, 0.28F}},

        std::pair{ImGuiMCP::ImGuiCol_FrameBg, ImGuiMCP::ImVec4{0.13F, 0.11F, 0.11F, 0.90F}},
        std::pair{ImGuiMCP::ImGuiCol_FrameBgHovered, ImGuiMCP::ImVec4{0.24F, 0.10F, 0.09F, 1.0F}},
        std::pair{ImGuiMCP::ImGuiCol_FrameBgActive, ImGuiMCP::ImVec4{0.32F, 0.10F, 0.09F, 1.0F}},

        std::pair{ImGuiMCP::ImGuiCol_CheckMark, kAccent},
        std::pair{ImGuiMCP::ImGuiCol_SliderGrab, kAccent},
        std::pair{ImGuiMCP::ImGuiCol_SliderGrabActive, ImGuiMCP::ImVec4{0.92F, 0.22F, 0.18F, 1.0F}},

        std::pair{ImGuiMCP::ImGuiCol_Button, ImGuiMCP::ImVec4{0.21F, 0.08F, 0.08F, 0.85F}},
        std::pair{ImGuiMCP::ImGuiCol_ButtonHovered, ImGuiMCP::ImVec4{0.36F, 0.10F, 0.09F, 1.0F}},
        std::pair{ImGuiMCP::ImGuiCol_ButtonActive, ImGuiMCP::ImVec4{0.47F, 0.11F, 0.10F, 1.0F}},

        std::pair{ImGuiMCP::ImGuiCol_Tab, ImGuiMCP::ImVec4{0.12F, 0.09F, 0.09F, 0.90F}},
        std::pair{ImGuiMCP::ImGuiCol_TabHovered, ImGuiMCP::ImVec4{0.36F, 0.10F, 0.09F, 1.0F}},
        std::pair{ImGuiMCP::ImGuiCol_TabActive, ImGuiMCP::ImVec4{0.29F, 0.08F, 0.07F, 1.0F}},

        std::pair{ImGuiMCP::ImGuiCol_Header, ImGuiMCP::ImVec4{0.27F, 0.08F, 0.07F, 0.85F}},
        std::pair{ImGuiMCP::ImGuiCol_HeaderHovered, ImGuiMCP::ImVec4{0.38F, 0.10F, 0.09F, 1.0F}},
        std::pair{ImGuiMCP::ImGuiCol_HeaderActive, ImGuiMCP::ImVec4{0.47F, 0.11F, 0.10F, 1.0F}},
    };

    struct Style {
        Style() {
            const auto em = ImGuiMCP::GetFontSize();
            ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_ItemSpacing, {em * 0.65F, em * 0.45F});
            ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_CellPadding, {em * 0.4F, em * 0.35F});
            ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_FramePadding, {em * 0.45F, em * 0.25F});
            ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_WindowPadding, {em * 0.8F, em * 0.7F});
            ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_FrameRounding, em * 0.15F);
            ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_ChildRounding, em * 0.25F);
            ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_FrameBorderSize, 0.0F);
            for (const auto& [color, value] : kColors) {
                ImGuiMCP::PushStyleColor(color, value);
            }
        }

        ~Style() {
            ImGuiMCP::PopStyleColor(static_cast<int>(kColors.size()));
            ImGuiMCP::PopStyleVar(7);
        }
    };

    inline void Note(const char* a_text) {
        ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Text, *ImGuiMCP::GetStyleColorVec4(ImGuiMCP::ImGuiCol_TextDisabled));
        ImGuiMCP::TextWrapped("%s", a_text);
        ImGuiMCP::PopStyleColor();
    }

    inline void Help(const char* a_text, bool a_labelHovered = false) {
        if (a_text && (a_labelHovered || ImGuiMCP::IsItemHovered(ImGuiMCP::ImGuiHoveredFlags_AllowWhenDisabled)) && ImGuiMCP::BeginTooltip()) {
            ImGuiMCP::PushTextWrapPos(ImGuiMCP::GetFontSize() * 26.0F);
            ImGuiMCP::TextUnformatted(a_text);
            ImGuiMCP::PopTextWrapPos();
            ImGuiMCP::EndTooltip();
        }
    }

    inline void Title(const char* a_title, const char* a_description = nullptr) {
        ImGuiMCP::ImVec2 available;
        ImGuiMCP::GetContentRegionAvail(&available);
        ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Text, kAccent);
        ImGuiMCP::TextWrapped("%s", a_title);
        ImGuiMCP::PopStyleColor();
        if (a_description && a_description[0] != '\0') {
            ImGuiMCP::ImVec2 titleSize;
            ImGuiMCP::ImVec2 descriptionSize;
            ImGuiMCP::CalcTextSize(&titleSize, a_title, nullptr, false, -1.0F);
            ImGuiMCP::CalcTextSize(&descriptionSize, a_description, nullptr, false, -1.0F);
            const auto em = ImGuiMCP::GetFontSize();
            const auto gap = em * 0.65F;
            if (titleSize.x + gap + std::min(descriptionSize.x, em * 12.0F) <= available.x) {
                ImGuiMCP::SameLine(0.0F, gap);
            }
            Note(a_description);
        }
    }

    inline bool Heading(const char* a_title, const char* a_button, const char* a_help = "Restores the defaults for this section.", const char* a_description = nullptr) {
        bool pressed{};
        if (ImGuiMCP::BeginTable("Heading", 2, ImGuiMCP::ImGuiTableFlags_SizingStretchProp)) {
            ImGuiMCP::TableSetupColumn("Title", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn("Action", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed);
            ImGuiMCP::TableNextColumn();
            ImGuiMCP::AlignTextToFramePadding();
            Title(a_title, a_description);
            ImGuiMCP::TableNextColumn();
            pressed = ImGuiMCP::Button(a_button);
            Help(a_help);
            ImGuiMCP::EndTable();
        }
        return pressed;
    }

    template <class Settings>
    auto Defaults(Settings& a_settings) {
        return [&a_settings] { a_settings = Settings{}; };
    }

    template <class Content, class Action = std::nullptr_t>
    void Panel(const char* a_title, const char* a_description, Content a_content, Action a_action = nullptr, const char* a_actionLabel = "Reset",
        const char* a_actionHelp = "Restores the defaults for this section.") {
        if (ImGuiMCP::BeginChild(a_title, {0.0F, 0.0F}, ImGuiMCP::ImGuiChildFlags_Border | ImGuiMCP::ImGuiChildFlags_AutoResizeY)) {
            if constexpr (std::is_same_v<Action, std::nullptr_t>) {
                Title(a_title, a_description);
            } else if (Heading(a_title, a_actionLabel, a_actionHelp, a_description)) {
                a_action();
            }
            a_content();
        }
        ImGuiMCP::EndChild();
    }

    template <class... Panels>
    void Columns(Panels... a_panels) {
        ImGuiMCP::ImVec2 available;
        ImGuiMCP::GetContentRegionAvail(&available);
        const auto columns = std::clamp(static_cast<int>(available.x / (ImGuiMCP::GetFontSize() * 28.0F)), 1, 2);
        const ImGuiMCP::ImVec2 size{std::min(available.x, ImGuiMCP::GetFontSize() * 90.0F), 0.0F};
        if (ImGuiMCP::BeginTable("Panels", columns, ImGuiMCP::ImGuiTableFlags_SizingStretchSame, size)) {
            ((ImGuiMCP::TableNextColumn(), a_panels()), ...);
            ImGuiMCP::EndTable();
        }
    }

    inline void Toggle(const char* a_label, bool& a_value, const char* a_help) {
        ImGuiMCP::Checkbox(a_label, &a_value);
        Help(a_help);
    }

    template <class Widget>
    bool Field(const char* a_label, Widget a_widget, const char* a_help = nullptr) {
        ImGuiMCP::PushID(a_label);
        ImGuiMCP::TextUnformatted(a_label);
        const bool labelHovered = ImGuiMCP::IsItemHovered(ImGuiMCP::ImGuiHoveredFlags_AllowWhenDisabled);
        ImGuiMCP::SetNextItemWidth(-1.0F);
        const bool changed = a_widget("##Value");
        Help(a_help, labelHovered);
        ImGuiMCP::PopID();
        return changed;
    }

    inline void Slider(const char* a_label, float& a_value, float a_minimum, float a_maximum, const char* a_help, const char* a_format = "%.2f") {
        Field(a_label, [&](const char* a_id) { return ImGuiMCP::SliderFloat(a_id, &a_value, a_minimum, a_maximum, a_format, ImGuiMCP::ImGuiSliderFlags_AlwaysClamp); }, a_help);
    }

    inline void Number(const char* a_label, float& a_value, const char* a_help, float a_step = 0.1F, float a_fastStep = 1.0F, const char* a_format = "%.2f") {
        Field(a_label, [&](const char* a_id) { return ImGuiMCP::InputFloat(a_id, &a_value, a_step, a_fastStep, a_format); }, a_help);
    }

    inline void Vector(const char* a_label, RE::NiPoint3& a_value, const char* a_help) {
        Field(a_label, [&](const char* a_id) { return ImGuiMCP::InputFloat3(a_id, &a_value.x, "%.2f"); }, a_help);
    }

    template <std::size_t N>
    bool Choice(const char* a_label, int& a_selected, const std::array<const char*, N>& a_choices) {
        return Field(a_label, [&](const char* a_id) { return ImGuiMCP::Combo(a_id, &a_selected, a_choices.data(), static_cast<int>(N)); });
    }

    inline void Columns(const char* a_id, float a_minimumWidthInEms, std::initializer_list<void (*)()> a_panels) {
        ImGuiMCP::ImVec2 available;
        ImGuiMCP::GetContentRegionAvail(&available);
        const auto columns = std::clamp(static_cast<int>(available.x / (ImGuiMCP::GetFontSize() * a_minimumWidthInEms)), 1, static_cast<int>(a_panels.size()));
        if (ImGuiMCP::BeginTable(a_id, columns, ImGuiMCP::ImGuiTableFlags_SizingStretchSame)) {
            for (const auto panel : a_panels) {
                ImGuiMCP::TableNextColumn();
                panel();
            }
            ImGuiMCP::EndTable();
        }
    }

    template <class Content>
    void Tab(const char* a_label, Content a_content) {
        if (ImGuiMCP::BeginTabItem(a_label)) {
            ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_ChildBg, ImGuiMCP::ImVec4{0.0F, 0.0F, 0.0F, 0.0F});
            const bool visible = ImGuiMCP::BeginChild(a_label);
            ImGuiMCP::PopStyleColor();
            if (visible) {
                a_content();
            }
            ImGuiMCP::EndChild();
            ImGuiMCP::EndTabItem();
        }
    }

    inline void Feedback(const std::string& a_message) {
        if (!a_message.empty()) {
            ImGuiMCP::Spacing();
            ImGuiMCP::TextColored(kAccent, "Last result");
            ImGuiMCP::TextWrapped("%s", a_message.c_str());
        }
    }
}
