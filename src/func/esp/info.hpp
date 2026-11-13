#pragma once
#include "common.hpp"

namespace func::esp {

    // TOP INFO: PLAYER NAME, DISTANCE, AND BOT TAG (NO OVERLAP WITH TRACER)
    inline void drawTopInfo(ImDrawList* dl, const PlayerData& p, float cx, float top_y) {
        if (!cfg::esp::name && !cfg::esp::distance) return;

        char nameBuf[24];
        truncName(p.name, nameBuf, sizeof(nameBuf));
        if (nameBuf[0] == 0)
            snprintf(nameBuf, sizeof(nameBuf), "%s", p.isBot ? "BOT" : "Player");

        char distBuf[16];
        fmtDist(p.distance, distBuf, sizeof(distBuf));

        const char* tag = cfg::esp::bot_tag ? (p.isBot ? "AI " : "") : "";
        char fullText[64];
        if (cfg::esp::name && cfg::esp::distance) {
            snprintf(fullText, sizeof(fullText), "%s%s  %s", tag, nameBuf, distBuf);
        } else if (cfg::esp::name) {
            snprintf(fullText, sizeof(fullText), "%s%s", tag, nameBuf);
        } else {
            snprintf(fullText, sizeof(fullText), "%s", distBuf);
        }

        const float fs = ESP_FONT_SIZE + 1.f;
        ImVec2 ts = ImGui::CalcTextSize(fullText);
        float tx = cx - ts.x * 0.5f;
        float ty = top_y - ts.y - 8.f;

        // Shadow outline for readability on any terrain background
        dl->AddText(ImGui::GetFont(), fs, {tx + 1.f, ty + 1.f}, IM_COL32(0, 0, 0, 240), fullText);
        dl->AddText(ImGui::GetFont(), fs, {tx - 1.f, ty}, IM_COL32(0, 0, 0, 180), fullText);
        dl->AddText(ImGui::GetFont(), fs, {tx + 1.f, ty}, IM_COL32(0, 0, 0, 180), fullText);
        dl->AddText(ImGui::GetFont(), fs, {tx, ty - 1.f}, IM_COL32(0, 0, 0, 180), fullText);
        dl->AddText(ImGui::GetFont(), fs, {tx, ty + 1.f}, IM_COL32(0, 0, 0, 180), fullText);

        float curX = tx;
        if (tag[0] != 0) {
            ImVec2 tagSize = ImGui::CalcTextSize(tag);
            ImU32 tagCol = p.isBot ? IM_COL32(255, 75, 75, 255) : IM_COL32(0, 200, 255, 255);
            dl->AddText(ImGui::GetFont(), fs, {curX, ty}, tagCol, tag);
            curX += tagSize.x;
        }

        if (cfg::esp::name) {
            ImVec2 nSize = ImGui::CalcTextSize(nameBuf);
            ImU32 nCol = p.isVisible ? toU32(cfg::esp::name_col) : withAlpha(toU32(cfg::esp::name_col), 0.75f);
            dl->AddText(ImGui::GetFont(), fs, {curX, ty}, nCol, nameBuf);
            curX += nSize.x;
        }

        if (cfg::esp::distance) {
            if (cfg::esp::name) curX += ImGui::CalcTextSize("  ").x;
            ImU32 dCol = p.isVisible ? toU32(cfg::esp::distance_col) : withAlpha(toU32(cfg::esp::distance_col), 0.75f);
            dl->AddText(ImGui::GetFont(), fs, {curX, ty}, dCol, distBuf);
        }
    }

} // namespace func::esp
