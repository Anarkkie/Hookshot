#ifndef HOOKSHOT_FILE_SELECT_SAVE_H
#define HOOKSHOT_FILE_SELECT_SAVE_H

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace file_select {

using Bytes = std::vector<unsigned char>;

inline uint32_t read32(const Bytes& bytes, size_t offset) {
    return uint32_t(bytes[offset]) | uint32_t(bytes[offset + 1]) << 8 |
           uint32_t(bytes[offset + 2]) << 16 | uint32_t(bytes[offset + 3]) << 24;
}

inline bool save_header(const Bytes& bytes, size_t end) {
    return bytes.size() >= end && read32(bytes, 0) == 0x01020304 &&
           read32(bytes, 4) == 0x002d80eb && read32(bytes, 8) == end;
}

struct Caption {
    std::string scene;
    Bytes jpeg;
};

inline Caption read_caption(const Bytes& bytes) {
    // Same layout as the save preview in host/control.c.
    constexpr size_t picture = 1396;
    if (bytes.size() < picture + 4 || !save_header(bytes, 0x2a0)) return {};
    Caption caption;
    for (size_t i = 684; i < 748 && bytes[i]; ++i) {
        if (bytes[i] < 0x20 || bytes[i] > 0x7e) return {};
        caption.scene += static_cast<char>(bytes[i]);
    }
    const size_t length = read32(bytes, picture - 4);
    if (length >= 4 && length <= bytes.size() - picture &&
        bytes[picture] == 0xff && bytes[picture + 1] == 0xd8 &&
        bytes[picture + length - 2] == 0xff && bytes[picture + length - 1] == 0xd9) {
        caption.jpeg.assign(bytes.begin() + picture, bytes.begin() + picture + length);
    }
    return caption;
}

enum class ItemCategory { inventory, equipment, quest };

struct Item {
    uint32_t stock;
    const wchar_t* name;
    const char* texture;
    ItemCategory category;
    uint32_t acquired;
    uint32_t max_quantity; // Zero for a single item, otherwise the supported count bound.
};

inline int item_group_order(const Item& item) {
    const std::string_view key(item.texture);
    const auto starts = [&](std::string_view prefix) { return key.substr(0, prefix.size()) == prefix; };
    if (item.category == ItemCategory::quest) {
        if (starts("Item_Stone_")) return 0;
        if (starts("Item_Medal_")) return 1;
        // Supplemental songs share the real ocarina image when no verified song icon is available.
        if (key == "Item_Ocarina_Time") return 2;
        if (starts("Item_Token_")) return 3;
        if (key == "Obj_Heart_Piece") return 4;
        if (starts("Item_FairyBless_")) return 5;
        return 6; // Trading-sequence items.
    }
    if (item.category == ItemCategory::equipment) {
        if (starts("Weapon_Sword_")) return 0;
        if (starts("Weapon_Shield_")) return 1;
        if (starts("Armor_Child_Mask_")) return 7;
        if (key.find("Gauntlet") != std::string_view::npos || starts("Item_Scale_")) return 3;
        if (key.find("Bag_") != std::string_view::npos || starts("Item_Quiver_")) return 4;
        if (starts("Item_Wallet_")) return 5;
        return 2; // Clothing, boots and worn equipment.
    }
    if (starts("Weapon_Range_") || key == "Item_Hookshot" || key == "Item_Longshot" ||
        key == "Item_Boomerang" || key == "Item_Hammer") return 0;
    if (starts("Item_Ocarina_")) return 1;
    if (key == "Item_DinFire" || key == "Item_FaroreWind" || key == "Item_NayruLove") return 2;
    if (starts("Item_Arrow_")) return 3;
    if (starts("Item_Bottle")) return 5;
    return 4; // Consumable supplies.
}

// One catalog feeds both the native preview and the local art extractor.
inline constexpr Item equipment[] = {
#define FILE_SELECT_ITEM(stock, acquired, name, texture, category, maximum) \
    {stock, name, texture, ItemCategory::category, acquired, maximum},
#include "../../nx2/file_select_items.inc"
#undef FILE_SELECT_ITEM
};

// Bottle slots carry separate ownership, validity and content records (the same public catalog).
inline constexpr Item bottle_contents[] = {
    {0xbe69de49, L"Empty Bottle", "Item_Bottle", ItemCategory::inventory, 0, 5},
    {0xeaba6067, L"Red Potion", "Item_Bottle_RedPotion", ItemCategory::inventory, 0, 5},
    {0x049d37c1, L"Green Potion", "Item_Bottle_GreenPotion", ItemCategory::inventory, 0, 5},
    {0xf383d6e5, L"Blue Potion", "Item_Bottle_BluePotion", ItemCategory::inventory, 0, 5},
    {0x1c293517, L"Blue Fire", "Item_Bottle_BlueFire", ItemCategory::inventory, 0, 5},
    {0x1e834ef8, L"Fish", "Item_Bottle_Fish", ItemCategory::inventory, 0, 5},
    {0x0f24e808, L"Letter in a Bottle", "Item_Bottle_Letter", ItemCategory::inventory, 0, 5},
    {0xcdc21471, L"Big Poe", "Item_Bottle_BigPoe", ItemCategory::inventory, 0, 5},
    {0x02084f9e, L"Poe", "Item_Bottle_Poe", ItemCategory::inventory, 0, 5},
    {0x74d62ecb, L"Bug", "Item_Bottle_Bug", ItemCategory::inventory, 0, 5},
    {0x6f49e357, L"Fairy", "Item_Bottle_Fairy", ItemCategory::inventory, 0, 5},
    {0x73356827, L"Milk", "Item_Bottle_Milk", ItemCategory::inventory, 0, 5},
    {0xeef6e4b0, L"Half Bottle of Milk", "Item_Bottle_HalfMilk", ItemCategory::inventory, 0, 5},
};
inline constexpr std::array<std::array<uint32_t, 3>, 5> bottle_slots{{
    {0xd53ce3a7, 0xf39ce410, 0x7d48df76}, {0xfca29a04, 0x000ff6ea, 0x1edd164a},
    {0x893216a1, 0xf63b1194, 0xb80c70a8}, {0x5324f9d3, 0xb7f052ce, 0xe4c1b1fc},
    {0xc92b16be, 0xe27b136d, 0x365e1953},
}};

struct Progress {
    std::optional<uint32_t> health_quarters, capacity_quarters;
    std::optional<uint32_t> magic_tenths, magic_capacity_tenths;
    std::vector<const Item*> owned;
    std::vector<std::optional<uint32_t>> quantities; // Parallel to owned; current stock, including zero.
    std::optional<uint32_t> rupees;
    std::optional<bool> childhood;
    bool double_defense = false;
};

inline constexpr std::array<uint32_t, 4> progress_vitals{0x31ab5580, 0xfbe01da1, 0x76b18200, 0x12d63a97};
inline constexpr std::array<uint32_t, 4> caption_vitals{0x1e55ecc5, 0x74bd05d0, 0x19f26b6a, 0x943f8ec3};

inline std::optional<size_t> find_field(const Bytes& bytes, size_t end, uint32_t id) {
    if (!save_header(bytes, end)) return {};
    std::optional<size_t> field;
    for (size_t offset = 40; offset < end; offset += 8) {
        if (read32(bytes, offset) != id) continue;
        if (field) return {};
        field = offset + 4;
    }
    return field;
}

inline Progress read_progress(const Bytes& bytes, const std::vector<const Item*>& extras = {}, uint32_t age_field = 0) {
    constexpr size_t fields_end = 0x175d8;
    if (!save_header(bytes, fields_end)) return {};
    std::array<std::optional<uint32_t>, progress_vitals.size()> values{};
    std::array<std::optional<uint32_t>, std::size(equipment)> stocks{}, acquired{};
    std::optional<uint32_t> rupees, wallet_limit;
    for (size_t offset = 40; offset < fields_end; offset += 8) {
        const uint32_t id = read32(bytes, offset);
        const uint32_t value = read32(bytes, offset + 4);
        for (size_t i = 0; i < progress_vitals.size(); ++i) {
            if (id != progress_vitals[i]) continue;
            if (values[i]) return {};
            values[i] = value;
        }
        for (size_t i = 0; i < stocks.size(); ++i) {
            if (id == equipment[i].stock) {
                if (stocks[i]) return {};
                stocks[i] = value;
            }
            if (id == equipment[i].acquired) {
                if (acquired[i]) return {};
                acquired[i] = value;
            }
        }
        if (id == 0xa77921d7) {
            if (rupees) return {};
            rupees = value;
        }
        if (id == 0xe0010a50) {
            if (wallet_limit) return {};
            wallet_limit = value;
        }
    }
    Progress progress;
    const auto [health, capacity, magic, magic_capacity] = values;
    if (health && capacity && *capacity >= 12 && *capacity <= 80 &&
        *capacity % 4 == 0 && *health <= *capacity) {
        progress.health_quarters = health;
        progress.capacity_quarters = capacity;
    }
    if (magic && magic_capacity && (*magic_capacity == 0 || *magic_capacity == 240 ||
                                    *magic_capacity == 480) && *magic <= *magic_capacity) {
        progress.magic_tenths = magic;
        progress.magic_capacity_tenths = magic_capacity;
    }
    for (size_t i = 0; i < stocks.size(); ++i) {
        const auto& item = equipment[i];
        const auto limit = item.max_quantity ? item.max_quantity : 1;
        if ((stocks[i] && *stocks[i] > limit) || (acquired[i] && *acquired[i] > 1)) continue;
        if (acquired[i] != 1 && (!stocks[i] || !*stocks[i])) continue;
        progress.owned.push_back(&item);
        progress.quantities.push_back(item.max_quantity ? stocks[i] : std::nullopt);
        if (item.stock == 0x9a826fab) progress.double_defense = true;
    }
    if (rupees && wallet_limit && (*wallet_limit == 200 || *wallet_limit == 500 || *wallet_limit == 1000) &&
        *rupees <= *wallet_limit) progress.rupees = rupees;
    std::array<unsigned, std::size(bottle_contents)> bottles{};
    for (const auto& slot : bottle_slots) {
        const auto stock = find_field(bytes, fields_end, slot[0]);
        const auto valid = find_field(bytes, fields_end, slot[1]);
        const auto content = find_field(bytes, fields_end, slot[2]);
        if (!stock || !valid || !content || read32(bytes, *stock) != 1 || read32(bytes, *valid) != 1) continue;
        for (size_t i = 0; i < bottles.size(); ++i)
            if (read32(bytes, *content) == bottle_contents[i].stock) ++bottles[i];
    }
    for (size_t i = 0; i < bottles.size(); ++i) {
        if (!bottles[i]) continue;
        progress.owned.push_back(&bottle_contents[i]);
        progress.quantities.push_back(bottles[i]);
    }
    // Supplemental fields are validated against the installation's local schema at build time.
    for (const auto* item : extras) {
        std::optional<uint32_t> value;
        bool duplicate = false;
        for (size_t offset = 40; offset < fields_end; offset += 8) {
            if (read32(bytes, offset) != item->stock) continue;
            if (value) duplicate = true;
            value = read32(bytes, offset + 4);
        }
        if (!duplicate && value == 1) {
            progress.owned.push_back(item);
            progress.quantities.push_back(std::nullopt);
        }
    }
    if (age_field) {
        bool duplicate = false;
        for (size_t offset = 40; offset < fields_end; offset += 8) {
            if (read32(bytes, offset) != age_field) continue;
            if (progress.childhood) duplicate = true;
            const auto value = read32(bytes, offset + 4);
            if (value > 1) duplicate = true;
            progress.childhood = value != 0;
        }
        if (duplicate) progress.childhood.reset();
    }
    return progress;
}

} // namespace file_select

#endif
