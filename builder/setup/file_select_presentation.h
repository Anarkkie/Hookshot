#ifndef HOOKSHOT_FILE_SELECT_PRESENTATION_H
#define HOOKSHOT_FILE_SELECT_PRESENTATION_H

#include "file_select_save.h"
#include <algorithm>

namespace file_select {

// Decoding preserves acquired records. Presentation summarizes replacement tiers
// without claiming an active capacity or hiding independent equipment and songs.
enum class ItemFamily { none, clothes, stick_bag, nut_bag, seed_bag, bomb_bag,
                        quiver, wallet, scale, adult_gauntlets, hookshot };
struct ItemChoice { ItemFamily family = ItemFamily::none; int rank = 0; };

inline ItemChoice item_choice(const Item& item, std::optional<bool> childhood) {
    const std::string_view key(item.texture);
    if (item.category == ItemCategory::inventory) {
        if (key == "Item_Hookshot") return {ItemFamily::hookshot, 1};
        if (key == "Item_Longshot") return {ItemFamily::hookshot, 2};
        return {};
    }
    if (item.category != ItemCategory::equipment) return {};
    if (key == "Obj_Armor_Child_Normal") return {ItemFamily::clothes, childhood.value_or(true) ? 2 : 1};
    if (key == "Armor_Adult_Normal") return {ItemFamily::clothes, childhood.value_or(true) ? 1 : 2};
    const std::pair<std::string_view, ItemFamily> bags[]{
        {"Item_StickBag_", ItemFamily::stick_bag}, {"Item_NutBag_", ItemFamily::nut_bag},
        {"Item_BulletBag_", ItemFamily::seed_bag}, {"Item_BombBag_", ItemFamily::bomb_bag},
        {"Item_Quiver_", ItemFamily::quiver}, {"Item_Wallet_", ItemFamily::wallet},
    };
    for (const auto& [prefix, family] : bags) {
        if (key.substr(0, prefix.size()) != prefix) continue;
        const auto tier = key.substr(prefix.size());
        if (tier == "Small") return {family, 1};
        if (tier == "Medium") return {family, 2};
        if (tier == "Large") return {family, 3};
    }
    if (key == "Item_Scale_Silver") return {ItemFamily::scale, 1};
    if (key == "Item_Scale_Gold") return {ItemFamily::scale, 2};
    if (key == "Armor_Adult_Gauntlet_Silver") return {ItemFamily::adult_gauntlets, 1};
    if (key == "Armor_Adult_Gauntlet_Gold") return {ItemFamily::adult_gauntlets, 2};
    return {};
}

inline std::vector<size_t> presentation_items(const Progress& progress) {
    std::vector<size_t> result;
    for (size_t index = 0; index < progress.owned.size(); ++index) {
        const auto choice = item_choice(*progress.owned[index], progress.childhood);
        auto previous = result.end();
        if (choice.family != ItemFamily::none)
            previous = std::find_if(result.begin(), result.end(), [&](size_t old) {
                return item_choice(*progress.owned[old], progress.childhood).family == choice.family;
            });
        if (previous == result.end()) result.push_back(index);
        else if (choice.rank > item_choice(*progress.owned[*previous], progress.childhood).rank) *previous = index;
    }
    // Keep original indexes so quantities, icons and accessible names remain paired.
    return result;
}

} // namespace file_select
#endif
