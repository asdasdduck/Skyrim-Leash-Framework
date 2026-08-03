#include "DebugOverlay.h"

#include <cmath>
#include <mutex>
#include <utility>
#include <vector>

#include "../../include/SKSEMenuFramework.h"
#include "../PCH.h"

namespace LeashFramework::UI::DebugOverlay {
    namespace {
        constexpr float kProjectionTolerance = 1.0e-5F;

        std::mutex callbackLock;
        std::vector<DrawCallback> pendingCallbacks;
        SKSEMenuFramework::Model::HudElement* hudElement{};

        [[nodiscard]] bool Project(RE::NiCamera& a_camera, const RE::NiPoint3& a_worldPosition, const ImGuiMCP::ImVec2& a_displaySize, ImGuiMCP::ImVec2& a_screenPosition) {
            float screenX{};
            float screenY{};
            float depth{};
            if (!a_camera.WorldPtToScreenPt3(a_worldPosition, screenX, screenY, depth, kProjectionTolerance) || depth <= 0.0F || !std::isfinite(screenX) || !std::isfinite(screenY)) {
                return false;
            }
            a_screenPosition = {screenX * a_displaySize.x, (1.0F - screenY) * a_displaySize.y};
            return true;
        }

        void __stdcall Render() {
            std::vector<DrawCallback> callbacks;
            {
                std::lock_guard lock{callbackLock};
                callbacks.swap(pendingCallbacks);
            }
            if (SKSEMenuFramework::IsAnyBlockingWindowOpened()) {
                return;
            }
            for (const auto& callback : callbacks) {
                callback();
            }
        }
    }  // namespace

    void Register() {
        if (!hudElement) {
            hudElement = SKSEMenuFramework::AddHudElement(Render);
        }
    }

    void Queue(DrawCallback a_callback) {
        if (!hudElement || !a_callback) {
            return;
        }
        std::lock_guard lock{callbackLock};
        pendingCallbacks.push_back(std::move(a_callback));
    }

    void DrawWorldLines(std::span<const WorldLine> a_lines) {
        auto* camera = RE::Main::WorldRootCamera();
        auto* io = ImGuiMCP::GetIO();
        auto* drawList = ImGuiMCP::GetForegroundDrawList();
        if (!camera || !io || !drawList || io->DisplaySize.x <= 0.0F || io->DisplaySize.y <= 0.0F) {
            return;
        }

        for (const auto& line : a_lines) {
            ImGuiMCP::ImVec2 start;
            ImGuiMCP::ImVec2 end;
            if (Project(*camera, line.start, io->DisplaySize, start) && Project(*camera, line.end, io->DisplaySize, end)) {
                ImGuiMCP::ImDrawListManager::AddLine(drawList, start, end, line.color, line.thickness);
            }
        }
    }
}  // namespace LeashFramework::UI::DebugOverlay
