#ifndef HOOKSHOT_FILE_SELECT_LAYOUT_H
#define HOOKSHOT_FILE_SELECT_LAYOUT_H

#include <algorithm>
#include <cstddef>

namespace file_select {

// A pixel-stable client canvas. The system-aware window frame is sized separately
// for the system DPI; increasing caption/border metrics cannot consume this area.
struct SelectorCanvas {
    static constexpr int width = 1004, height = 641;
    static constexpr int grid_top = 316, detail_top = 524, detail_height = 23;
    static constexpr int pager_top = 550, pager_height = 26;
    static constexpr int action_top(int client_height) { return client_height - 61; }
};

// The same fixed cells serve every category, including missing-art fallbacks.
struct InventoryPage {
    static constexpr size_t columns = 4, rows = 3, cells = columns * rows;
    static constexpr int icon_size = 48, cell_height = 62, gap = 8;
    size_t count = 0, page = 0;

    size_t pages() const { return count ? 1 + (count - 1) / cells : 1; }
    size_t first() const { return std::min(page, pages() - 1) * cells; }
    size_t visible() const { return std::min(cells, count - first()); }
    size_t page_for(size_t index) const { return count ? std::min(index, count - 1) / cells : 0; }
    size_t move(size_t index, int delta) const {
        if (!count) return 0;
        index = std::min(index, count - 1);
        if (delta < 0) return index < size_t(-delta) ? 0 : index - size_t(-delta);
        return std::min(count - 1, index + size_t(delta));
    }
    static int cell_width(int available) { return (available - int(columns - 1) * gap) / int(columns); }
};

} // namespace file_select
#endif
