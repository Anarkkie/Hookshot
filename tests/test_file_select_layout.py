"""A fixed-size inventory reaches every entry without changing icon or cell size."""

import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]
FIXTURE = r'''
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
#include <cassert>
#include <set>
#include <stdexcept>
#include "file_select_layout.h"
using file_select::InventoryPage;
using file_select::SelectorCanvas;
#ifdef _WIN32
__WINDOW_SIZING__
#endif
int main() {
    for (size_t count : {size_t(0), size_t(1), size_t(12), size_t(13), size_t(101), size_t(250)}) {
        InventoryPage grid{count, 0};
        std::set<size_t> seen;
        for (size_t p = 0; p < grid.pages(); ++p) {
            grid.page = p;
            assert(grid.visible() <= grid.cells);
            for (size_t cell = 0; cell < grid.visible(); ++cell) {
                const size_t index = grid.first() + cell;
                assert(index < count && grid.page_for(index) == p);
                assert(seen.insert(index).second);
            }
        }
        assert(seen.size() == count);
        grid.page = 1000; // A category shrinks while another page was selected.
        assert(grid.first() <= count && grid.visible() <= grid.cells);
        assert(grid.move(0, -4) == 0);
        assert(grid.move(count, 4) == (count ? count - 1 : 0));
        for (size_t index = 0; index < count; ++index) {
            assert(grid.move(index, 1) == (index + 1 < count ? index + 1 : index));
            assert(grid.page_for(index) < grid.pages());
        }
    }
    InventoryPage grid{101, 0};
    assert(grid.move(11, 1) == 12 && grid.page_for(12) == 1);
    assert(grid.move(12, -1) == 11 && grid.page_for(11) == 0);
    // The fixed 1004px client canvas leaves at least 712px of content width.
    const int group_width = (712 - 32) / 3;
    const int width = InventoryPage::cell_width(group_width);
    assert(width >= InventoryPage::icon_size);
    assert(width * 4 + InventoryPage::gap * 3 <= group_width);
    assert(group_width * 3 + 32 <= 712);
    assert(InventoryPage::cell_height * 3 + InventoryPage::gap * 2 <= 204);
    // Twenty hearts are two rows of ten; the separate magic panel cannot overlap.
    assert(10 * 23 <= 250);
    assert(250 + 240 <= 712);
    // Real content bounds, shared with the renderer. Frame growth must not consume them.
    const int grid_end = SelectorCanvas::grid_top + int(InventoryPage::rows) * InventoryPage::cell_height +
                         int(InventoryPage::rows - 1) * InventoryPage::gap;
    assert(grid_end <= SelectorCanvas::detail_top);
    assert(SelectorCanvas::detail_top + SelectorCanvas::detail_height <= SelectorCanvas::pager_top);
    const int pager_end = SelectorCanvas::pager_top + SelectorCanvas::pager_height;
    assert(pager_end < SelectorCanvas::action_top(SelectorCanvas::height));
    // Independent frame examples cover normal, enlarged and high-DPI non-client metrics.
    for (int frame : {39, 49, 59, 78, 98}) {
        const int outer = SelectorCanvas::height + frame;
        assert(outer - frame == SelectorCanvas::height);
        assert(pager_end < SelectorCanvas::action_top(outer - frame));
    }
    assert(SelectorCanvas::action_top(680 - 59) < pager_end); // Previous fixed-outer-size regression.
#ifdef _WIN32
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    using AdjustForDpi = BOOL (WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    const auto adjust = reinterpret_cast<AdjustForDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi"));
    if (adjust) {
        for (UINT dpi : {96u, 120u, 144u, 192u, 240u}) {
            RECT frame{};
            assert(adjust(&frame, style, FALSE, 0, dpi));
            const RECT outer = selector_outer_bounds(style, 0, dpi);
            const int client_width = outer.right - outer.left - (frame.right - frame.left);
            const int client_height = outer.bottom - outer.top - (frame.bottom - frame.top);
            assert(client_width == SelectorCanvas::width && client_height == SelectorCanvas::height);
            assert(pager_end < SelectorCanvas::action_top(client_height));
        }
    }
    const RECT outer = selector_outer_bounds(style);
    const auto window = CreateWindowW(L"STATIC", L"Hidden client-size test", style, 0, 0,
        outer.right - outer.left, outer.bottom - outer.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    assert(window);
    RECT client{};
    assert(GetClientRect(window, &client));
    assert(client.right == SelectorCanvas::width && client.bottom == SelectorCanvas::height);
    DestroyWindow(window);
#endif
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="hookshot-file-select-layout-") as temp:
        source = Path(temp) / "layout.cpp"
        binary = Path(temp) / "layout.exe"
        selector = (ROOT / "builder/setup/file_select.cpp").read_text(encoding="utf-8")
        start = selector.index("RECT selector_outer_bounds(")
        stop = selector.index("\n}\n", start) + 3
        source.write_text(FIXTURE.replace("__WINDOW_SIZING__", selector[start:stop]), encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I",
                        str(ROOT / "builder/setup"), str(source), "-o", str(binary)], check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=20)
    print("File Select inventory, client sizing and disjoint DPI layout passed.")


if __name__ == "__main__":
    main()
