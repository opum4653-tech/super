#pragma once
#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include "imgui.h"
#include "../../game/data.hpp"
#include "../../ui/cfg.hpp"
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>
#include <algorithm>
#include <cmath>

extern float g_sw, g_sh;

namespace func::esp {

    inline bool is_active() {
        if (!cfg::master_enabled) return false;
        return cfg::esp::box || cfg::esp::health || cfg::esp::armor ||
               cfg::esp::name || cfg::esp::distance || cfg::esp::weapon ||
               cfg::esp::tracer || cfg::esp::loot || cfg::esp::crosshair ||
               cfg::esp::firing_ind;
    }

    // HELPER COLOR CONVERSIONS
    inline ImU32 toU32(const ImVec4& c, float alphaMul = 1.f) {
        return IM_COL32(
            (int)(c.x * 255.f),
            (int)(c.y * 255.f),
            (int)(c.z * 255.f),
            (int)(c.w * 255.f * alphaMul));
    }

    inline ImU32 withAlpha(ImU32 c, float mul) {
        float aF = ((float)((c >> 24) & 0xFF)) * mul;
        int a = (int)ImClamp(aF, 0.0f, 255.0f);
        return (c & 0x00FFFFFF) | ((ImU32)(uint32_t)a << 24);
    }

    inline ImU32 fadeDist(ImU32 base, float dist, float maxRange) {
        float t = ImClamp(1.f - dist / maxRange, 0.35f, 1.f);
        return withAlpha(base, t);
    }

    inline ImU32 hpColor(float t) {
        t = ImClamp(t, 0.f, 1.f);
        float r, g, b;
        if (t < 0.5f) { float k = t * 2.f; r = 1.f; g = 0.35f + 0.55f * k; b = 0.15f; }
        else          { float k = (t - 0.5f) * 2.f; r = 1.f - 0.9f * k; g = 0.9f; b = 0.15f + 0.30f * k; }
        return IM_COL32((int)(r*255), (int)(g*255), (int)(b*255), 255);
    }

    inline ImU32 armorColor(float t) {
        t = ImClamp(t, 0.f, 1.f);
        return IM_COL32((int)(80 + 100 * t), (int)(180 + 60 * t), 255, 255);
    }

    // GEOMETRIC & STRING FORMATTING
    inline bool ok(const Vector3& v) { return v.x > 0 && v.y > 0 && v.z > 0.5f; }

    inline void fmtDist(float d, char* out, size_t n) {
        if (d >= 1000.f) snprintf(out, n, "%.1fkm", d / 1000.f);
        else             snprintf(out, n, "%.0fm", d);
    }

    inline void truncName(const char* src, char* out, size_t n) {
        if (!src || !src[0]) { out[0] = 0; return; }
        size_t len = strlen(src);
        if (len <= 12) { snprintf(out, n, "%s", src); return; }
        snprintf(out, n, "%.11s.", src);
    }

    // VISUAL CONSTANTS
    static constexpr ImU32 C_SHADOW = IM_COL32(0, 0, 0, 180);
    static constexpr ImU32 C_BORDER = IM_COL32(255, 255, 255, 40);

    static constexpr float ESP_FONT_SIZE     = 12.f;
    static constexpr float PANEL_ROW_H       = ESP_FONT_SIZE + 5.f; // 17px
    static constexpr float PANEL_TOP_OFFSET   = 35.f;

    // BOUNDING BOX CALCULATION
    struct PlayerBox {
        ImVec2 tl, br;
        bool   valid;
    };

    inline PlayerBox computePlayerBox(const PlayerData& p) {
        PlayerBox pb{ {0,0}, {0,0}, false };
        if (!ok(p.Head) || !ok(p.Toe)) return pb;

        float topY = ImMin(p.Head.y, p.Toe.y);
        float botY = ImMax(p.Head.y, p.Toe.y);
        float height = botY - topY;
        if (height < 2.f) return pb;

        float midX = (p.Head.x + p.Toe.x) * 0.5f;
        float width = height * 0.45f;
        float halfW = width * 0.5f;

        const float padY = height * 0.08f;

        pb.tl = ImVec2(midX - halfW, topY - padY);
        pb.br = ImVec2(midX + halfW, botY + padY * 0.5f);
        pb.valid = true;
        return pb;
    }

} // namespace func::esp
