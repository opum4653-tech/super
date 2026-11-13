#pragma once
#include "common.hpp"
#include "box.hpp"
#include "line.hpp"
#include "health.hpp"
#include "info.hpp"
#include "weapon.hpp"
#include "loot.hpp"
#include "crosshair.hpp"
#include "firing.hpp"

namespace func::esp {

    // MAIN ESP DRAW PIPELINE
    inline void draw() {
        if (!data::inMatch.load(std::memory_order_relaxed)) return;

        ImDrawList* dl = ImGui::GetForegroundDrawList();

        std::vector<PlayerData> snap;
        {
            std::lock_guard<std::mutex> lk(data::players_mutex);
            snap = data::players;
        }

        for (const auto& p : snap) {
            if (p.Head.z < 0.5f || p.Toe.z < 0.5f) continue;
            if (p.curHP <= 0) continue;
            if (cfg::esp::ignore_bots && p.isBot) continue;
            if (p.distance > cfg::esp::max_range) continue;

            PlayerBox pb = computePlayerBox(p);
            if (!pb.valid) continue;

            ImVec2 tl = pb.tl;
            ImVec2 br = pb.br;

            if (br.x < -30.f || tl.x > g_sw + 30.f) continue;
            if (br.y < -30.f || tl.y > g_sh + 30.f) continue;

            float cx = (tl.x + br.x) * 0.5f;

            ImU32 colBase = p.isVisible ? toU32(cfg::esp::box_visible)
                                        : toU32(cfg::esp::box_col);
            colBase = fadeDist(colBase, p.distance, cfg::esp::max_range);

            // 1. Tracer Line (Top, Center Reticle Dot, or Bottom)
            if (cfg::esp::tracer) {
                drawTracer(dl, p, colBase);
            }

            // 2. Bounding Box
            if (cfg::esp::box) {
                if (cfg::esp::box_filled) {
                    dl->AddRectFilled(tl, br, withAlpha(colBase, 0.15f), cfg::esp::box_rounding);
                }
                drawBox(dl, tl, br, colBase, cfg::esp::box_type);
            }

            // 3. Firing Warning Indicator
            if (cfg::esp::firing_ind) {
                drawFiring(dl, p, cx, tl.y);
            }

            // 4. Player Info (Name, Distance, Bot Tag)
            PlayerData tmp = p;
            if (tmp.name[0] == 0) {
                snprintf(tmp.name, sizeof(tmp.name), "%s", p.isBot ? "BOT" : "Player");
            }
            drawTopInfo(dl, tmp, cx, tl.y);

            // 5. Right-side Vertical Health & Armor Bar (Strictly gated by cfg::esp::health)
            drawSideHealthBar(dl, tmp, tl, br);

            // 6. Weapon Type Pill (Below Feet)
            if (cfg::esp::weapon) {
                drawWeapon(dl, p, {cx, br.y});
            }
        }

        // 7. World Loot Items
        if (cfg::esp::loot) {
            drawLoot();
        }

        // 8. Screen Crosshair & Reticle Center Dot
        if (cfg::esp::crosshair) {
            drawCrosshair(dl);
        }
    }

} // namespace func::esp
