"""Acquired save records become player-facing equipment without duplicate upgrades."""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]
FIXTURE = r'''
#include <cassert>
#include "file_select_presentation.h"
using namespace file_select;

const Item* catalog(std::string_view key) {
    for (const auto& item : equipment) if (item.texture == key) return &item;
    assert(false); return nullptr;
}
Progress acquired(std::initializer_list<std::string_view> keys) {
    Progress result;
    for (auto key : keys) { result.owned.push_back(catalog(key)); result.quantities.push_back({}); }
    return result;
}
std::vector<std::string_view> shown(const Progress& p) {
    std::vector<std::string_view> result;
    for (auto index : presentation_items(p)) result.push_back(p.owned[index]->texture);
    return result;
}
bool has(const Progress& p, std::string_view key) {
    const auto keys = shown(p);
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}
int main() {
    // Both starting records can be present in an ordinary child save. One green
    // tunic is shown, using saved age, without changing the decoded records.
    auto p = acquired({"Armor_Adult_Normal", "Obj_Armor_Child_Normal"});
    for (auto age : {std::optional<bool>(true), std::optional<bool>(false), std::optional<bool>()}) {
        p.childhood = age;
        assert(shown(p).size() == 1);
        assert(shown(p)[0] == (age.value_or(true) ? "Obj_Armor_Child_Normal" : "Armor_Adult_Normal"));
        assert(p.owned.size() == 2);
    }
    for (auto key : {"Armor_Adult_Normal", "Obj_Armor_Child_Normal"}) {
        p = acquired({key}); p.childhood = false;
        assert(shown(p).size() == 1 && shown(p)[0] == key); // Never invent a missing variant.
    }

    // Regression matching the reported early-game preview: two clothes records
    // and two historical stick-bag tiers must produce two summary entries.
    p = acquired({"Armor_Adult_Normal", "Item_StickBag_Small", "Obj_Armor_Child_Normal", "Item_StickBag_Medium"});
    p.childhood = true;
    assert(shown(p).size() == 2 && has(p, "Obj_Armor_Child_Normal") && has(p, "Item_StickBag_Medium"));
    assert(!has(p, "Armor_Adult_Normal") && !has(p, "Item_StickBag_Small"));

    // Each capacity family gets one highest acquired tier, regardless of record
    // order; an unowned larger tier is never synthesized.
    for (std::string prefix : {"Item_StickBag_", "Item_NutBag_", "Item_BulletBag_", "Item_BombBag_", "Item_Quiver_", "Item_Wallet_"}) {
        p = acquired({prefix + "Medium", prefix + "Small"});
        assert(shown(p).size() == 1 && shown(p)[0] == prefix + "Medium");
        p.owned.push_back(catalog(prefix + "Large")); p.quantities.push_back({});
        assert(shown(p).size() == 1 && shown(p)[0] == prefix + "Large");
        std::reverse(p.owned.begin(), p.owned.end());
        assert(shown(p).size() == 1 && shown(p)[0] == prefix + "Large");
    }
    p = acquired({"Item_Scale_Silver", "Item_Scale_Gold", "Armor_Adult_Gauntlet_Silver", "Armor_Adult_Gauntlet_Gold",
                  "Armor_Child_Gauntlet_Goron", "Item_Hookshot", "Item_Longshot"});
    assert(shown(p).size() == 4 && has(p, "Item_Scale_Gold") && has(p, "Armor_Adult_Gauntlet_Gold") &&
           has(p, "Armor_Child_Gauntlet_Goron") && has(p, "Item_Longshot"));

    // Distinct equipment remains: the remake has two ocarina slots; sword life
    // and mask selection are not simple upgrade-priority rules.
    p = acquired({"Item_Ocarina_Fairy", "Item_Ocarina_Time", "Weapon_Sword_Kokiri", "Weapon_Sword_Master",
                  "Weapon_Sword_Biggoron", "Weapon_Sword_Giant", "Weapon_Sword_BrokenGiant", "Weapon_Shield_Deku",
                  "Weapon_Shield_Hylian", "Weapon_Shield_Mirror", "Armor_Child_Mask_Keaton", "Armor_Child_Mask_Truth"});
    assert(shown(p).size() == p.owned.size());
    assert(catalog("Item_AgonyStone")->category == ItemCategory::equipment);

    // Indexes still address the original stock entry after a replacement. A
    // consumed supply's zero count and separate songs sharing an icon survive.
    p = acquired({"Item_StickBag_Small", "Item_DekuStick", "Item_StickBag_Large"});
    p.quantities[1] = 0;
    const Item first{101, L"First song", "Item_Ocarina_Time", ItemCategory::quest, 0, 0};
    const Item second{102, L"Second song", "Item_Ocarina_Time", ItemCategory::quest, 0, 0};
    p.owned.push_back(&first); p.owned.push_back(&second);
    p.quantities.push_back({}); p.quantities.push_back({});
    const auto indices = presentation_items(p);
    assert((indices == std::vector<size_t>{2, 1, 3, 4}));
    assert(p.quantities[indices[1]] == 0);
    assert(presentation_items(Progress{}).empty());
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="hookshot-presentation-") as temp:
        source, binary = Path(temp) / "presentation.cpp", Path(temp) / "presentation.exe"
        source.write_text(FIXTURE, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I",
                        str(ROOT / "builder/setup"), str(source), "-o", str(binary)], check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=20)
    print("File Select presentation: tunics, upgrade families, distinct equipment and quantities passed.")


if __name__ == "__main__":
    main()
