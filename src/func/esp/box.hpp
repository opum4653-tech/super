#pragma once
#include "common.hpp"

namespace func::esp {

    inline void drawBox(ImDrawList* dl, ImVec2 tl, ImVec2 br, ImU32 col, int type) {
        float thick = cfg::esp::box_thickness;
        float round = cfg::esp::box_rounding;

        if (type == 0) {
            // Corner Bracket Box
            float len = (br.x - tl.x) * cfg::esp::bracket_frac;
            ImVec2 pts[8][2] = {
                {tl,                 {tl.x + len, tl.y}},
                {tl,                 {tl.x, tl.y + len}},
                {{br.x - len, tl.y}, {br.x, tl.y}},
                {{br.x, tl.y},       {br.x, tl.y + len}},
                {{tl.x, br.y - len}, {tl.x, br.y}},
                {{tl.x, br.y},       {tl.x + len, br.y}},
                {{br.x - len, br.y}, {br.x, br.y}},
                {{br.x, br.y - len}, {br.x, br.y}},
            };
            for (auto& s : pts) {
                dl->AddLine(s[0], s[1], withAlpha(col, 0.25f), thick + 3.f);
                dl->AddLine({s[0].x + 1, s[0].y + 1},
                            {s[1].x + 1, s[1].y + 1}, C_SHADOW, thick + 1.f);
                dl->AddLine(s[0], s[1], col, thick);
            }
        } else if (type == 1) {
            // Full Chaturbhuj (Rectangular) 2D Box
            dl->AddRect({tl.x - 1.f, tl.y - 1.f}, {br.x + 1.f, br.y + 1.f},
                        IM_COL32(0, 0, 0, 180), round, 0, thick + 1.5f);
            dl->AddRect(tl, br, col, round, 0, thick);
            dl->AddRectFilled(tl, br, withAlpha(col, 0.05f), round);
        } else {
            // Outline Box with Dual Alpha Borders
            dl->AddRect(tl, br, withAlpha(col, 0.18f), round, 0, thick + 5.f);
            dl->AddRect(tl, br, withAlpha(col, 0.45f), round, 0, thick + 2.f);
            dl->AddRect(tl, br, col, round, 0, thick);
            dl->AddRectFilled(tl, br, withAlpha(col, 0.06f), round);
        }
    }

} // namespace func::esp
