#pragma once
#include "common.hpp"

namespace func::esp {

    inline const char* lootCat(uint32_t resID) {
        uint32_t c = (resID >> 24) & 0xFF;
        switch (c) {
            case 0x01: return "GUN";
            case 0x02: return "AMMO";
            case 0x03: return "ARMOR";
            case 0x04: return "HEAL";
            case 0x05: return "NADE";
            case 0x06: return "BAG";
            default:   return "ITEM";
        }
    }

    inline ImU32 lootColor(const char* cat) {
        if (strcmp(cat, "GUN")   == 0) return IM_COL32(255, 100, 100, 255);
        if (strcmp(cat, "AMMO")  == 0) return IM_COL32(255, 200, 80, 255);
        if (strcmp(cat, "ARMOR") == 0) return IM_COL32(80, 180, 255, 255);
        if (strcmp(cat, "HEAL")  == 0) return IM_COL32(80, 255, 120, 255);
        if (strcmp(cat, "NADE")  == 0) return IM_COL32(200, 80, 255, 255);
        if (strcmp(cat, "BAG")   == 0) return IM_COL32(160, 120, 60, 255);
        return IM_COL32(200, 200, 200, 255);
    }

    inline void drawLootLabel(ImDrawList* dl, const LootItem& li) {
        const char* cat = lootCat(li.resID);
        ImU32 col = lootColor(cat);

        char buf[64];
        snprintf(buf, sizeof(buf), "%s  %.0fm", cat, li.distance);

        ImVec2 ts = ImGui::CalcTextSize(buf);
        float pad = 4.f;
        float px = li.screenPos.x - ts.x * 0.5f - pad;
        float py = li.screenPos.y;

        dl->AddRectFilled({px + 1, py + 1},
                          {px + ts.x + pad * 2 + 1, py + ts.y + pad * 2 + 1},
                          C_SHADOW, 4.f);
        dl->AddRectFilled({px, py},
                          {px + ts.x + pad * 2, py + ts.y + pad * 2},
                          IM_COL32(15, 15, 20, 220), 4.f);
        dl->AddRect({px, py},
                    {px + ts.x + pad * 2, py + ts.y + pad * 2},
                    col, 4.f, 0, 1.5f);

        dl->AddText(ImGui::GetFont(), ESP_FONT_SIZE - 2.f,
                    {px + pad, py + pad}, col, buf);
        dl->AddCircleFilled({li.screenPos.x, li.screenPos.y}, 3.f, col);
    }

    inline void drawLoot() {
        if (!cfg::esp::loot || !data::inMatch.load(std::memory_order_relaxed)) return;
        ImDrawList* dl = ImGui::GetForegroundDrawList();

        std::vector<LootItem> snap;
        {
            std::lock_guard<std::mutex> lk(data::loots_mutex);
            snap = data::loots;
        }

        for (const auto& li : snap) {
            if (li.screenPos.x < 0 || li.screenPos.y < 0) continue;
            if (li.screenPos.x > g_sw || li.screenPos.y > g_sh) continue;
            drawLootLabel(dl, li);
        }
    }

} // namespace func::esp
