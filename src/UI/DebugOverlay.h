#pragma once

#include <cstdint>
#include <functional>
#include <span>

#include "../PCH.h"

namespace LeashFramework::UI::DebugOverlay {
    struct WorldLine {
        RE::NiPoint3 start;
        RE::NiPoint3 end;
        std::uint32_t color{0xDCFFFF00};
        float thickness{1.5F};
    };

    using DrawCallback = std::function<void()>;

    void Register();
    void Queue(DrawCallback a_callback);
    void DrawWorldLines(std::span<const WorldLine> a_lines);
}  // namespace LeashFramework::UI::DebugOverlay
