#pragma once

#include "../PCH.h"
#include "../../include/SKSEMenuFramework.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <string>

namespace LeashFramework::UI::MenuLayout {
    template <class Widget>
    bool Field(const char* a_label, Widget a_widget, const char* a_help = nullptr) {
        ImGuiMCP::PushID(a_label);
        ImGuiMCP::TextUnformatted(a_label);
        if (a_help && ImGuiMCP::IsItemHovered()) {
            ImGuiMCP::SetTooltip("%s", a_help);
        }
        ImGuiMCP::SetNextItemWidth(-1.0F);
        const bool changed = a_widget("##Value");
        if (a_help && ImGuiMCP::IsItemHovered()) {
            ImGuiMCP::SetTooltip("%s", a_help);
        }
        ImGuiMCP::PopID();
        return changed;
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

    inline void Feedback(const std::string& a_message) {
        if (!a_message.empty()) {
            ImGuiMCP::Spacing();
            ImGuiMCP::TextWrapped("%s", a_message.c_str());
        }
    }
}
