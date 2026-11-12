#ifndef HOOKSHOT_FILE_SELECT_METADATA_H
#define HOOKSHOT_FILE_SELECT_METADATA_H

#include "file_select_save.h"
#include <charconv>
#include <memory>
#include <set>
#include <sstream>
#include <string_view>

namespace file_select {

struct NamedItem {
    std::wstring name;
    std::string texture;
    Item item;
    NamedItem(std::string_view label, std::string key, uint32_t field)
        : name(label.begin(), label.end()), texture(std::move(key)),
          item{field, name.c_str(), texture.c_str(), ItemCategory::quest, 0, 0} {}
};

struct SaveMetadata {
    std::vector<std::unique_ptr<NamedItem>> songs;
    uint32_t age = 0;
    std::vector<const Item*> items() const {
        std::vector<const Item*> result;
        for (const auto& song : songs) result.push_back(&song->item);
        return result;
    }
};

inline SaveMetadata read_metadata(std::string_view input) {
    // Generated data remains local. Reject malformed or excessive records as a unit.
    if (input.size() > 65536) return {};
    std::istringstream stream{std::string(input)};
    std::string line;
    if (!std::getline(stream, line) || line != "HOOKSHOT_SAVE_METADATA\t1") return {};
    SaveMetadata result;
    std::set<uint32_t> seen;
    std::set<std::string> actors;
    const auto number = [](const std::string& text) {
        uint32_t value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size() ? value : 0;
    };
    const auto key = [](const std::string& text) {
        return !text.empty() && text.size() <= 80 &&
            text.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos;
    };
    while (std::getline(stream, line)) {
        std::vector<std::string> cells;
        std::istringstream row(line);
        for (std::string cell; std::getline(row, cell, '\t');) cells.push_back(cell);
        if (cells.empty()) return {};
        if (cells[0] == "song" && cells.size() == 6) {
            if (result.songs.size() >= 64) return {};
            const auto field = number(cells[4]);
            if (!key(cells[1]) || !actors.insert(cells[1]).second || !key(cells[3]) ||
                cells[2].empty() || cells[2].size() > 80 || !field || !number(cells[5]) ||
                !seen.insert(field).second) return {};
            for (const unsigned char c : cells[2]) if (c < 32 || c > 126) return {};
            result.songs.push_back(std::make_unique<NamedItem>(cells[2], cells[3], field));
        } else if (cells[0] == "age" && cells.size() == 4 && !result.age && cells[2] == "1" && cells[3] == "0") {
            result.age = number(cells[1]);
            if (!result.age || !seen.insert(result.age).second) return {};
        } else if (cells[0] == "defense" && cells.size() == 2 && number(cells[1])) {
            // Already represented by the public inventory catalog.
            if (!seen.insert(number(cells[1])).second) return {};
        } else return {};
    }
    return result;
}

} // namespace file_select
#endif
