#pragma once
#include "common.hpp"

namespace func::esp {

    // VERTICAL HEALTH & ARMOR BAR (RIGHT SIDE OF 2D BOX)
    // Strictly controlled by cfg::esp::health and cfg::esp::armor switches.
    // If disabled from the menu, zero bar drawing occurs.
    inline void drawSideHealthBar(ImDrawList* dl, const PlayerData& p, ImVec2 tl, ImVec2 br) {
        if (!cfg::esp::health && !cfg::esp::armor) return;

        float boxH = br.y - tl.y;
        if (boxH <= 4.f) return;

        // Positioned on the RIGHT side of the bounding box
        float barX = br.x + 5.f;
        float barW = 4.f;

        if (cfg::esp::health) {
            float hp01 = p.maxHP > 0 ? (float)p.curHP / (float)p.maxHP : (p.curHP > 0 ? 1.0f : 0.0f);
            hp01 = ImClamp(hp01, 0.f, 1.f);

            // Background track + border
            dl->AddRectFilled({barX - 1.f, tl.y - 1.f}, {barX + barW + 1.f, br.y + 1.f},
                              IM_COL32(10, 14, 20, 220), 1.5f);
            dl->AddRect({barX - 1.f, tl.y - 1.f}, {barX + barW + 1.f, br.y + 1.f},
                        IM_COL32(0, 0, 0, 180), 1.5f, 0, 1.f);

            // Health fill (bottom up with dynamic color based on remaining health)
            float fillH = boxH * hp01;
            dl->AddRectFilled({barX, br.y - fillH}, {barX + barW, br.y},
                              hpColor(hp01), 1.0f);
        }

        if (cfg::esp::armor && p.curArmor > 0) {
            float armX = barX + (cfg::esp::health ? (barW + 3.f) : 0.f);
            float armW = 2.5f;
            float arm01 = ImClamp((float)p.curArmor / 100.f, 0.f, 1.f);
            float fillArm = boxH * arm01;

            dl->AddRectFilled({armX - 1.f, tl.y - 1.f}, {armX + armW + 1.f, br.y + 1.f},
                              IM_COL32(10, 14, 20, 220), 1.f);
            dl->AddRect({armX - 1.f, tl.y - 1.f}, {armX + armW + 1.f, br.y + 1.f},
                        IM_COL32(0, 0, 0, 180), 1.f, 0, 1.f);
            dl->AddRectFilled({armX, br.y - fillArm}, {armX + armW, br.y},
                              armorColor(arm01), 1.0f);
        }
    }

} // namespace func::esp
