#define NOMINMAX
#include "file_select_store.h"

#include <commctrl.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <charconv>
#include <cwctype>
#include <memory>
#include <thread>

#include "file_select.h"
#include "file_select_layout.h"
#include "file_select_presentation.h"

namespace file_select {
namespace {

constexpr wchar_t releases[] = L"https://github.com/NotRexed/Hookshot/releases";
constexpr UINT finished = WM_APP + 1;
constexpr COLORREF background = RGB(18, 23, 29), panel = RGB(32, 40, 50);
constexpr COLORREF ink = RGB(238, 232, 215), muted = RGB(170, 181, 190), gold = RGB(215, 179, 97);
enum { list_id = 101, play_id, new_id, rename_id, update_id,
       inventory_id, equipment_id, quest_id, previous_id = 120, next_id = 130, cell_id = 200 };

RECT selector_outer_bounds(DWORD style, DWORD extended_style = 0, UINT dpi = 0) {
    RECT bounds{0, 0, SelectorCanvas::width, SelectorCanvas::height};
    // Keep all content in the established pixel coordinate system. Only Windows'
    // caption and frame follow system DPI. Older Windows uses its system-aware API.
    const auto user32 = GetModuleHandleW(L"user32.dll");
    using GetDpi = UINT (WINAPI*)();
    using AdjustForDpi = BOOL (WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    const auto get_dpi = reinterpret_cast<GetDpi>(GetProcAddress(user32, "GetDpiForSystem"));
    const auto adjust = reinterpret_cast<AdjustForDpi>(GetProcAddress(user32, "AdjustWindowRectExForDpi"));
    const BOOL ok = adjust && (dpi || get_dpi) ? adjust(&bounds, style, FALSE, extended_style, dpi ? dpi : get_dpi())
                                    : AdjustWindowRectEx(&bounds, style, FALSE, extended_style);
    if (!ok) throw std::runtime_error("Windows could not size the File Select window.");
    return bounds;
}

std::wstring wide(std::string_view text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
    std::wstring result(length, L'\0');
    if (length) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), result.data(), length);
    return result;
}

std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(length, '\0');
    if (length) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), int(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::string installed(const fs::path& root) {
    std::ifstream marker(root / L"hookshot_version.txt");
    for (std::string line; std::getline(marker, line);) {
        if (line.compare(0, 3, "\xef\xbb\xbf") == 0) line.erase(0, 3);
        if (line.compare(0, 8, "version=")) continue;
        const auto last = line.find_last_not_of(" \t\r\n");
        if (last < 8) return {};
        const auto version = line.substr(8, last - 7);
        return version.size() <= 80 && version.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-+") ==
                                            std::string::npos ? version : std::string();
    }
    return {};
}

struct Version { std::array<unsigned, 3> number; bool prerelease; };

std::optional<Version> version(std::string_view value) {
    if (!value.empty() && value[0] == 'v') value.remove_prefix(1);
    Version result{};
    for (size_t i = 0; i < result.number.size(); ++i) {
        const auto first = value.data(), end = first + value.size();
        const auto converted = std::from_chars(first, end, result.number[i]);
        if (converted.ec != std::errc()) return {};
        value.remove_prefix(converted.ptr - first);
        if (i + 1 == result.number.size()) break;
        if (value.empty() || value.front() != '.') return {};
        value.remove_prefix(1);
    }
    if (!value.empty() && ((value.front() != '-' && value.front() != '+') || value.size() == 1)) return {};
    result.prerelease = !value.empty() && value.front() == '-';
    return result;
}

void update_dialog(HWND parent, const fs::path& root, const std::string& latest) {
    const auto available = version(latest);
    if (!available || available->prerelease) {
        MessageBoxW(parent, L"Could not check GitHub Releases. Try again when you're online.",
                    L"Check for updates", MB_OK | MB_ICONINFORMATION);
        return;
    }
    const auto have = installed(root);
    const auto current = version(have);
    const auto offer = [&](const std::wstring& message) {
        if (MessageBoxW(parent, message.c_str(), L"Hookshot releases", MB_YESNO | MB_ICONINFORMATION) == IDYES)
            ShellExecuteW(parent, L"open", releases, nullptr, nullptr, SW_SHOWNORMAL);
    };
    if (!current) {
        offer(L"Latest stable release: " + wide(latest) + L"\n\nOpen GitHub Releases?");
    } else if (current->number < available->number ||
               (current->number == available->number && current->prerelease)) {
        offer(L"Installed: " + wide(have) + L"\nLatest stable: " + wide(latest) + L"\n\nOpen GitHub Releases?");
    } else {
        const auto message = current->number == available->number
            ? L"Hookshot " + wide(have) + L" is the latest stable release."
            : L"Installed: " + wide(have) + L"\nLatest stable: " + wide(latest);
        MessageBoxW(parent, message.c_str(), L"Check for updates", MB_OK | MB_ICONINFORMATION);
    }
}

std::wstring scene_name(std::string_view scene) {
    std::wstring result;
    for (size_t i = 0; i < scene.size(); ++i) {
        const wchar_t c = scene[i];
        if (i && iswupper(c) && iswlower(scene[i - 1])) result += L' ';
        result += c == L'_' ? L' ' : c;
    }
    return result;
}

void report(const std::wstring& line) {
    const std::string data = utf8(line + L"\r\n");
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    if (out && out != INVALID_HANDLE_VALUE) WriteFile(out, data.data(), DWORD(data.size()), &written, nullptr);
}

std::wstring display_units(uint32_t value, unsigned scale) {
    auto result = std::to_wstring(value / scale);
    const unsigned remainder = value % scale;
    if (remainder) result += scale == 4 ? (remainder == 1 ? L".25" : remainder == 2 ? L".5" : L".75")
                                       : L"." + std::to_wstring(remainder);
    return result;
}

void text(HDC dc, HFONT font, COLORREF color, std::wstring_view label, RECT rect, UINT align = DT_LEFT) {
    const auto old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, label.data(), int(label.size()), &rect, align | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);
}

void fill(HDC dc, RECT area, COLORREF color) {
    const HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &area, brush);
    DeleteObject(brush);
}

std::unique_ptr<Gdiplus::Image> picture_from(const Bytes& bytes) {
    if (bytes.empty()) return {};
    auto* stream = SHCreateMemStream(bytes.data(), UINT(bytes.size()));
    if (!stream) return {};
    std::unique_ptr<Gdiplus::Image> image;
    {
        Gdiplus::Bitmap source(stream);
        if (source.GetLastStatus() == Gdiplus::Ok && source.GetWidth() && source.GetHeight())
            image.reset(source.Clone(0, 0, source.GetWidth(), source.GetHeight(), PixelFormat32bppPARGB));
    }
    stream->Release();
    return image;
}

void draw_image(Gdiplus::Graphics& graphics, Gdiplus::Image& picture, int x, int y, int width, int height, bool center = true) {
    const auto scale = std::min(double(width) / picture.GetWidth(), double(height) / picture.GetHeight());
    const int w = int(scale * picture.GetWidth()), h = int(scale * picture.GetHeight());
    graphics.DrawImage(&picture, center ? x + (width - w) / 2 : x, y + (height - h) / 2, w, h);
}

struct NameDialog {
    std::wstring value;
    HWND edit = nullptr;
    static INT_PTR CALLBACK proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<NameDialog*>(GetWindowLongPtrW(window, DWLP_USER));
        if (message == WM_INITDIALOG) {
            self = reinterpret_cast<NameDialog*>(lp);
            SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
            self->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", self->value.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 15, 16, 270, 27, window,
                nullptr, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(self->edit, EM_SETLIMITTEXT, 40, 0);
            SendMessageW(self->edit, EM_SETSEL, 0, -1);
            CreateWindowW(L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                115, 55, 80, 28, window, reinterpret_cast<HMENU>(IDOK), GetModuleHandleW(nullptr), nullptr);
            CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                205, 55, 80, 28, window, reinterpret_cast<HMENU>(IDCANCEL), GetModuleHandleW(nullptr), nullptr);
            SetFocus(self->edit);
            return FALSE;
        }
        if (message == WM_COMMAND && LOWORD(wp) == IDOK) {
            wchar_t name[41]{};
            GetWindowTextW(self->edit, name, int(std::size(name)));
            self->value = name;
            const auto first = self->value.find_first_not_of(L" \t");
            const auto last = self->value.find_last_not_of(L" \t");
            self->value = first == std::wstring::npos ? L"" : self->value.substr(first, last - first + 1);
            EndDialog(window, IDOK);
            return TRUE;
        }
        if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wp) == IDCANCEL)) {
            EndDialog(window, IDCANCEL);
            return TRUE;
        }
        return FALSE;
    }

    bool ask(HWND parent, HINSTANCE instance) {
        struct alignas(4) {
            DLGTEMPLATE dialog;
            WORD menu, window_class;
            wchar_t title[12];
        } form{};
        form.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER;
        form.dialog.cx = 185;
        form.dialog.cy = 60;
        wcscpy(form.title, L"Name a file");
        return DialogBoxIndirectParamW(instance, &form.dialog, parent, proc, reinterpret_cast<LPARAM>(this)) == IDOK;
    }
};

struct Window {
    Store& store;
    HINSTANCE instance;
    HWND window = nullptr, list = nullptr, play = nullptr, fresh = nullptr, rename = nullptr, update = nullptr;
    std::array<HWND, 3> tabs{};
    std::array<HWND, InventoryPage::cells * 3> cells{};
    std::array<HWND, 3> previous{}, next{};
    HWND item_detail = nullptr, tooltip = nullptr;
    std::wstring tooltip_text;
    unsigned tab = 0;
    size_t focused_item = 0;
    std::array<InventoryPage, 3> inventories{};
    std::array<std::vector<size_t>, 3> groups;
    HFONT heading = nullptr, subhead = nullptr, body = nullptr, small = nullptr;
    HBRUSH list_background = nullptr;
    HICON icon = nullptr;
    std::vector<unsigned> files;
    unsigned selected = 0;
    Preview preview;
    std::unique_ptr<Gdiplus::Image> picture;
    std::unique_ptr<Gdiplus::Image> heart, empty_heart, magic;
    std::vector<std::unique_ptr<Gdiplus::Image>> item_images;
    std::thread worker;
    bool busy = false, checking = false, closing_after_check = false;
    std::string latest, error;
    std::wstring status;
    DWORD exit_code = 0;

    Window(Store& store, HINSTANCE instance) : store(store), instance(instance) {}
    ~Window() {
        if (worker.joinable()) worker.join();
        if (icon) DestroyIcon(icon);
        DeleteObject(heading);
        DeleteObject(subhead);
        DeleteObject(body);
        DeleteObject(small);
        DeleteObject(list_background);
    }

    fs::path name_path(unsigned number) const {
        return store.profiles / L"names" / (std::to_wstring(number) + L".txt");
    }

    std::wstring name(unsigned number) const {
        const Bytes bytes = read_bytes(name_path(number), 240);
        auto value = wide(std::string(bytes.begin(), bytes.end()));
        if (value.size() > 40) value.clear();
        return value.empty() ? L"File " + std::to_wstring(number) : value;
    }

    HWND button(int id, const wchar_t* title) {
        return CreateWindowW(L"BUTTON", title, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | BS_NOTIFY,
                             0, 0, 0, 0, window, reinterpret_cast<HMENU>(INT_PTR(id)), instance, nullptr);
    }

    std::unique_ptr<Gdiplus::Image> art(const char* key) const {
        const auto png = store.root / L"bin" / L"file-select-art" / (std::string(key) + ".png");
        std::unique_ptr<Gdiplus::Image> image(Gdiplus::Image::FromFile(png.c_str()));
        if (image && image->GetLastStatus() != Gdiplus::Ok) image.reset();
        return image;
    }

    void create() {
        heading = CreateFontW(-34, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                              CLEARTYPE_QUALITY, 0, L"Georgia");
        subhead = CreateFontW(-22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                              CLEARTYPE_QUALITY, 0, L"Georgia");
        body = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                           CLEARTYPE_QUALITY, 0, L"Segoe UI");
        small = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                            CLEARTYPE_QUALITY, 0, L"Segoe UI");
        list_background = CreateSolidBrush(panel);
        ExtractIconExW(store.game.c_str(), 0, &icon, nullptr, 1);
        SendMessageW(window, WM_SETICON, ICON_BIG, LPARAM(icon));
        SendMessageW(window, WM_SETICON, ICON_SMALL, LPARAM(icon));
        list = CreateWindowW(L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                             LBS_NOTIFY | LBS_HASSTRINGS | LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT,
                             0, 0, 0, 0, window, reinterpret_cast<HMENU>(INT_PTR(list_id)), instance, nullptr);
        SendMessageW(list, LB_SETITEMHEIGHT, 0, 47);
        play = button(play_id, L"Start Game");
        fresh = button(new_id, L"New Game");
        rename = button(rename_id, L"Rename");
        update = button(update_id, L"Check for updates");
        tabs = {button(inventory_id, L"Items"), button(equipment_id, L"Equipment"), button(quest_id, L"Quest")};
        for (size_t i = 0; i < tabs.size(); ++i) {
            previous[i] = button(previous_id + int(i), L"‹");
            next[i] = button(next_id + int(i), L"›");
        }
        for (size_t i = 0; i < cells.size(); ++i) cells[i] = button(cell_id + int(i), L"");
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
        InitCommonControlsEx(&controls);
        tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                  CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                  window, nullptr, instance, nullptr);
        if (tooltip) {
            SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, 320);
            SendMessageW(tooltip, TTM_SETDELAYTIME, TTDT_INITIAL, 250);
            for (const auto cell : cells) {
                TOOLINFOW info{};
                info.cbSize = sizeof(info);
                info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
                info.hwnd = window;
                info.uId = UINT_PTR(cell);
                info.lpszText = LPSTR_TEXTCALLBACKW;
                SendMessageW(tooltip, TTM_ADDTOOLW, 0, LPARAM(&info));
            }
        }
        item_detail = CreateWindowW(L"STATIC", L"", WS_CHILD | SS_LEFT | SS_NOPREFIX,
                                   0, 0, 0, 0, window, nullptr, instance, nullptr);
        SendMessageW(item_detail, WM_SETFONT, WPARAM(body), TRUE);
        heart = art("heart");
        empty_heart = art("empty-heart");
        magic = art("magic");
        refresh(store.active());
    }

    void refresh(unsigned number) {
        files = store.files();
        files.push_back(0);
        const auto active = store.active();
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (const auto file : files) {
            auto label = file ? name(file) : L"+ New File";
            if (file && file == active) label += L"  •";
            SendMessageW(list, LB_ADDSTRING, 0, LPARAM(label.c_str()));
        }
        const auto found = std::find(files.begin(), files.end(), number);
        const auto index = found == files.end() ? 0 : found - files.begin();
        SendMessageW(list, LB_SETCURSEL, index, 0);
        select();
    }

    void select() {
        const auto index = SendMessageW(list, LB_GETCURSEL, 0, 0);
        if (index == LB_ERR || size_t(index) >= files.size()) return;
        selected = files[index];
        preview = selected ? store.preview(selected) : Preview();
        picture = picture_from(preview.caption.jpeg);
        item_images.clear();
        for (const Item* item : preview.progress.owned) item_images.push_back(art(item->texture));
        EnableWindow(rename, selected && !busy);
        EnableWindow(fresh, preview.saved && !busy);
        SetWindowTextW(play, selected && preview.saved ? L"Start Game" : L"New Game");
        for (auto& inventory : inventories) inventory.page = 0;
        focused_item = 0;
        filter_items();
        InvalidateRect(window, nullptr, FALSE);
    }

    std::wstring item_label(size_t index) const {
        std::wstring label = preview.progress.owned[index]->name;
        if (index < preview.progress.quantities.size() && preview.progress.quantities[index])
            label += L" ×" + std::to_wstring(*preview.progress.quantities[index]);
        return label;
    }

    std::wstring tooltip_label(HWND control) const {
        const auto found = std::find(cells.begin(), cells.end(), control);
        if (found == cells.end()) return {};
        const size_t cell = size_t(found - cells.begin()), group = cell / InventoryPage::cells;
        const size_t index = inventories[group].first() + cell % InventoryPage::cells;
        return index < groups[group].size() ? item_label(groups[group][index]) : std::wstring();
    }

    void filter_items() {
        for (auto& group : groups) group.clear();
        for (const size_t i : presentation_items(preview.progress)) {
            const auto category = preview.progress.owned[i]->category;
            const size_t group = category == ItemCategory::quest ? 2 : category == ItemCategory::equipment ? 1 : 0;
            groups[group].push_back(i);
        }
        for (size_t group = 0; group < groups.size(); ++group) {
            std::stable_sort(groups[group].begin(), groups[group].end(), [&](size_t a, size_t b) {
                return item_group_order(*preview.progress.owned[a]) < item_group_order(*preview.progress.owned[b]);
            });
            auto& inventory = inventories[group];
            inventory.count = groups[group].size();
            inventory.page = std::min(inventory.page, inventory.pages() - 1);
        }
        focused_item = inventories[tab].count ? std::min(focused_item, inventories[tab].count - 1) : 0;
        update_items();
    }

    void update_items() {
        for (size_t group = 0; group < groups.size(); ++group) {
            const auto& inventory = inventories[group];
            for (size_t cell = 0; cell < InventoryPage::cells; ++cell) {
                const auto control = cells[group * InventoryPage::cells + cell];
                const bool present = preview.saved && cell < inventory.visible();
                ShowWindow(control, present ? SW_SHOW : SW_HIDE);
                EnableWindow(control, present && !busy);
                if (present) SetWindowTextW(control, item_label(groups[group][inventory.first() + cell]).c_str());
                InvalidateRect(control, nullptr, FALSE);
            }
            ShowWindow(tabs[group], preview.saved ? SW_SHOW : SW_HIDE);
            for (const auto control : {previous[group], next[group]})
                ShowWindow(control, preview.saved && inventory.pages() > 1 ? SW_SHOW : SW_HIDE);
            EnableWindow(previous[group], !busy && inventory.page > 0);
            EnableWindow(next[group], !busy && inventory.page + 1 < inventory.pages());
            EnableWindow(tabs[group], preview.saved && !busy);
        }
        ShowWindow(item_detail, preview.saved ? SW_SHOW : SW_HIDE);
        SetWindowTextW(item_detail, preview.saved && inventories[tab].count
            ? item_label(groups[tab][focused_item]).c_str() : L"");
        InvalidateRect(window, nullptr, FALSE);
    }

    void change_tab(unsigned value) {
        if (busy || !preview.saved) return;
        tab = value;
        focused_item = inventories[tab].first();
        update_items();
        for (const auto control : tabs) InvalidateRect(control, nullptr, TRUE);
    }

    void page(unsigned group, int direction) {
        if (busy || !inventories[group].count) return;
        tab = group;
        auto& inventory = inventories[group];
        if (direction < 0 && inventory.page) --inventory.page;
        if (direction > 0 && inventory.page + 1 < inventory.pages()) ++inventory.page;
        focused_item = inventory.first();
        update_items();
        SetFocus(cells[tab * InventoryPage::cells]);
    }

    void focus_item(size_t index) {
        auto& inventory = inventories[tab];
        if (!inventory.count || busy) return;
        focused_item = std::min(index, inventory.count - 1);
        inventory.page = inventory.page_for(focused_item);
        update_items();
        SetFocus(cells[tab * InventoryPage::cells + focused_item - inventory.first()]);
    }

    void layout() {
        RECT r{};
        GetClientRect(window, &r);
        MoveWindow(list, 24, 91, 210, r.bottom - 162, TRUE);
        MoveWindow(rename, 24, r.bottom - 56, 210, 34, TRUE);
        MoveWindow(update, r.right - 182, 24, 158, 34, TRUE);
        MoveWindow(play, 260, SelectorCanvas::action_top(r.bottom), 166, 39, TRUE);
        MoveWindow(fresh, 437, SelectorCanvas::action_top(r.bottom), 122, 39, TRUE);
        const int content = r.right - 284, group_width = (content - 32) / 3;
        for (size_t group = 0; group < groups.size(); ++group) {
            const int left = 260 + int(group) * (group_width + 16);
            MoveWindow(tabs[group], left, 284, group_width, 26, TRUE);
            const int width = InventoryPage::cell_width(group_width);
            for (size_t cell = 0; cell < InventoryPage::cells; ++cell)
                MoveWindow(cells[group * InventoryPage::cells + cell],
                    left + int(cell % InventoryPage::columns) * (width + InventoryPage::gap),
                    SelectorCanvas::grid_top + int(cell / InventoryPage::columns) * (InventoryPage::cell_height + InventoryPage::gap),
                    width, InventoryPage::cell_height, TRUE);
            MoveWindow(previous[group], left, SelectorCanvas::pager_top, 32, SelectorCanvas::pager_height, TRUE);
            MoveWindow(next[group], left + group_width - 32, SelectorCanvas::pager_top, 32, SelectorCanvas::pager_height, TRUE);
        }
        MoveWindow(item_detail, 260, SelectorCanvas::detail_top, content, SelectorCanvas::detail_height, TRUE);
    }

    void draw_vitals(HDC dc, int left, int top, int right) const {
        const auto& progress = preview.progress;
        if (progress.capacity_quarters && progress.health_quarters) {
            const unsigned quarters = *progress.health_quarters;
            if (heart && empty_heart) {
                Gdiplus::Graphics graphics(dc);
                for (unsigned i = 0; i < *progress.capacity_quarters / 4; ++i) {
                    const int x = left + int(i % 10) * 23, y = top + int(i / 10) * 24;
                    draw_image(graphics, *empty_heart, x, y, 21, 21);
                    const int count = std::clamp(int(quarters) - int(i) * 4, 0, 4);
                    if (count) {
                        graphics.SetClip(Gdiplus::Rect(x, y, (21 * count + 3) / 4, 21));
                        draw_image(graphics, *heart, x, y, 21, 21);
                        graphics.ResetClip();
                    }
                }
            } else {
                const auto label = L"Hearts  " + display_units(quarters, 4) + L" / " +
                                   display_units(*progress.capacity_quarters, 4);
                text(dc, body, ink, label, {left, top, left + 230, top + 45});
            }
        }
        const int x = left;
        if (progress.magic_capacity_tenths && progress.magic_tenths && *progress.magic_capacity_tenths) {
            const auto label = *progress.magic_capacity_tenths == 480 ? L"Double magic" : L"Magic";
            text(dc, body, ink, label, {x, top + 56, right, top + 77});
            constexpr int width = 220;
            fill(dc, {x, top + 85, x + width, top + 93}, panel);
            const int level = int(width * *progress.magic_tenths / *progress.magic_capacity_tenths);
            if (magic) {
                Gdiplus::Graphics graphics(dc);
                graphics.SetClip(Gdiplus::Rect(x, top + 85, level, 8));
                graphics.DrawImage(magic.get(), x, top + 85, width, 8);
            } else fill(dc, {x, top + 85, x + level, top + 93}, RGB(108, 178, 121));
        }
    }

    void draw(HDC dc) const {
        RECT r{};
        GetClientRect(window, &r);
        fill(dc, r, background);
        if (icon) DrawIconEx(dc, 25, 26, icon, 32, 32, 0, nullptr, DI_NORMAL);
        text(dc, heading, gold, L"Select Your File", {70, 16, 423, 67});
        const auto tag = installed(store.root);
        if (!tag.empty()) text(dc, body, muted, L"Hookshot " + wide(tag), {425, 23, r.right - 197, 60}, DT_RIGHT);
        fill(dc, {24, 76, r.right - 24, 78}, gold);
        const int left = 260, right = r.right - 24;
        text(dc, subhead, ink, selected ? name(selected) : L"New File", {left, 87, right, 119});
        if (!preview.saved) {
            text(dc, body, muted, L"Start a new adventure", {left, 120, right, 149});
            text(dc, subhead, ink, L"Your adventure begins here", {left, 282, right, 320}, DT_CENTER);
            text(dc, body, muted, L"Saved previews, items, equipment and quest status appear here.",
                 {left, 325, right, 359}, DT_CENTER);
        } else {
            const auto scene = scene_name(preview.caption.scene);
            if (!scene.empty()) text(dc, body, muted, scene, {left, 120, right, 149});
            if (picture) {
                Gdiplus::Graphics graphics(dc);
                graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                draw_image(graphics, *picture, left, 154, 224, 126, false);
            } else {
                text(dc, body, muted, L"No save preview available", {left, 154, left + 224, 280}, DT_CENTER);
            }
            draw_vitals(dc, left + 248, 159, right);
            if (preview.progress.rupees)
                text(dc, subhead, gold, std::to_wstring(*preview.progress.rupees) + L" rupees",
                     {left + 493, 159, right, 188});
            if (preview.progress.double_defense)
                text(dc, body, ink, L"Double defense", {left + 493, 194, right, 219});
            if (preview.progress.childhood)
                text(dc, body, muted, *preview.progress.childhood ? L"Child" : L"Adult", {left + 493, 229, right, 254});
            const int group_width = (right - left - 32) / 3;
            for (size_t group = 0; group < groups.size(); ++group) {
                const auto& inventory = inventories[group];
                const int x = left + int(group) * (group_width + 16);
                if (!inventory.count)
                    text(dc, body, muted, L"Nothing recorded yet", {x, 365, x + group_width, 397}, DT_CENTER);
                if (inventory.pages() > 1) {
                    const auto summary = std::to_wstring(inventory.page + 1) + L" / " +
                        std::to_wstring(inventory.pages()) + L"  ·  " + std::to_wstring(inventory.count);
                    text(dc, body, muted, summary, {x + 34, SelectorCanvas::pager_top, x + group_width - 34,
                         SelectorCanvas::pager_top + SelectorCanvas::pager_height}, DT_CENTER);
                }
            }
        }

        text(dc, body, muted, status, {572, r.bottom - 60, right, r.bottom - 20}, DT_RIGHT);
    }

    void draw_item(const DRAWITEMSTRUCT& item) const {
        if (item.CtlID >= cell_id && item.CtlID < cell_id + cells.size()) {
            const size_t cell = item.CtlID - cell_id, group = cell / InventoryPage::cells;
            const size_t position = inventories[group].first() + cell % InventoryPage::cells;
            const bool selected_item = group == tab && position == focused_item;
            fill(item.hDC, item.rcItem, selected_item ? panel : background);
            if (position >= groups[group].size()) return;
            const size_t index = groups[group][position];
            const int width = item.rcItem.right - item.rcItem.left;
            const int x = item.rcItem.left + (width - InventoryPage::icon_size) / 2;
            const int y = item.rcItem.top + 4;
            if (index < item_images.size() && item_images[index]) {
                Gdiplus::Graphics graphics(item.hDC);
                graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                draw_image(graphics, *item_images[index], x, y, InventoryPage::icon_size, InventoryPage::icon_size);
            } else {
                RECT fallback{x, y, x + InventoryPage::icon_size, y + InventoryPage::icon_size};
                const auto old = SelectObject(item.hDC, small);
                SetBkMode(item.hDC, TRANSPARENT);
                SetTextColor(item.hDC, muted);
                std::wstring label = preview.progress.owned[index]->name;
                const std::wstring suffix = L" Medallion";
                if (label.size() > suffix.size() && label.compare(label.size() - suffix.size(), suffix.size(), suffix) == 0)
                    label.resize(label.size() - suffix.size());
                else if (label == L"Goron Ruby") label = L"Ruby";
                else if (label == L"Kokiri Emerald") label = L"Emerald";
                else if (label == L"Zora Sapphire") label = L"Sapphire";
                RECT measured = fallback;
                DrawTextW(item.hDC, label.c_str(), int(label.size()), &measured, DT_CENTER | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
                fallback.top += std::max(0L, (fallback.bottom - fallback.top - (measured.bottom - measured.top)) / 2);
                DrawTextW(item.hDC, label.c_str(), int(label.size()), &fallback, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
                SelectObject(item.hDC, old);
            }
            if (index < preview.progress.quantities.size() && preview.progress.quantities[index]) {
                const auto quantity = std::to_wstring(*preview.progress.quantities[index]);
                SIZE extent{};
                const auto old = SelectObject(item.hDC, body);
                GetTextExtentPoint32W(item.hDC, quantity.c_str(), int(quantity.size()), &extent);
                SelectObject(item.hDC, old);
                const RECT badge{item.rcItem.right - extent.cx - 7, item.rcItem.bottom - 21,
                                 item.rcItem.right - 1, item.rcItem.bottom - 1};
                fill(item.hDC, badge, background);
                text(item.hDC, body, gold, quantity, badge, DT_CENTER);
            }
            if (item.itemState & ODS_FOCUS) {
                RECT focus = item.rcItem;
                InflateRect(&focus, -2, -2);
                DrawFocusRect(item.hDC, &focus);
            }
            return;
        }
        if (item.CtlID >= inventory_id && item.CtlID <= quest_id) {
            fill(item.hDC, item.rcItem, background);
            const wchar_t* titles[]{L"Items", L"Equipment", L"Quest Status"};
            text(item.hDC, body, gold, titles[item.CtlID - inventory_id], item.rcItem);
            if (item.itemState & ODS_FOCUS) DrawFocusRect(item.hDC, &item.rcItem);
            return;
        }
        const bool file = item.CtlID == list_id;
        const bool primary = item.CtlID == play_id || (file && (item.itemState & ODS_SELECTED));
        fill(item.hDC, item.rcItem, primary ? gold : panel);
        std::wstring label;
        if (file && item.itemID != UINT(-1)) {
            const auto size = SendMessageW(list, LB_GETTEXTLEN, item.itemID, 0);
            if (size >= 0) {
                label.resize(size_t(size) + 1);
                SendMessageW(list, LB_GETTEXT, item.itemID, LPARAM(label.data()));
                label.resize(size_t(size));
            }
        } else if (!file) {
            wchar_t title[64]{};
            GetWindowTextW(item.hwndItem, title, int(std::size(title)));
            label = title;
        }
        RECT area = item.rcItem;
        area.left += 12;
        area.right -= 9;
        text(item.hDC, body, primary ? background : (item.itemState & ODS_DISABLED) ? muted : ink,
             label, area, file ? DT_LEFT : DT_CENTER);
        if (item.itemState & ODS_FOCUS) DrawFocusRect(item.hDC, &item.rcItem);
    }

    void set_busy(bool value, std::wstring message) {
        busy = value;
        status = std::move(message);
        for (HWND control : {list, play, fresh, rename, update}) EnableWindow(control, !busy);
        update_items();
        InvalidateRect(window, nullptr, FALSE);
    }

    void start(bool reset) {
        if (busy) return;
        const unsigned file = selected ? selected : store.next_file();
        if (reset && preview.saved) {
            const auto prompt = L"Replace the save in " + name(file) + L" with a new game?";
            if (MessageBoxW(window, prompt.c_str(), L"New Game", MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES)
                return;
            if (MessageBoxW(window, L"Confirm that you want to clear this file.", L"New Game",
                            MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES) return;
        }
        set_busy(true, L"Playing " + name(file) + L"…");
        worker = std::thread([this, file, reset] {
            try { exit_code = store.play(file, reset); }
            catch (const std::exception& e) { error = e.what(); }
            PostMessageW(window, finished, 0, LPARAM(file));
        });
    }

    void check_updates() {
        checking = true;
        set_busy(true, L"Checking for updates…");
        worker = std::thread([this] {
            char tag[96]{};
            if (hookshot_file_select_latest(tag, int(std::size(tag)))) latest = tag;
            PostMessageW(window, finished, 1, 0);
        });
    }

    void change_name() {
        if (!selected || busy) return;
        NameDialog dialog{name(selected)};
        if (!dialog.ask(window, instance)) return;
        const auto path = name_path(selected);
        fs::create_directories(path.parent_path());
        auto temporary = path;
        temporary += L".new";
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.exceptions(std::ios::badbit | std::ios::failbit);
        out << utf8(dialog.value);
        out.close();
        move_path(temporary, path, MOVEFILE_REPLACE_EXISTING);
        refresh(selected);
    }

    void complete(bool checked, unsigned file) {
        worker.join();
        checking = false;
        if (closing_after_check) {
            DestroyWindow(window);
            return;
        }
        set_busy(false, L"");
        if (checked) {
            select();
            update_dialog(window, store.root, latest);
            latest.clear();
            return;
        }
        refresh(file);
        if (!error.empty()) {
            MessageBoxW(window, wide(error).c_str(), L"Hookshot", MB_OK | MB_ICONERROR);
            error.clear();
        } else if (exit_code) {
            status = L"Hookshot closed unexpectedly";
        }
        InvalidateRect(window, nullptr, FALSE);
    }

    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        if (message == WM_NCCREATE) {
            auto* self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->window = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!self) return DefWindowProcW(window, message, wp, lp);
        try {
            switch (message) {
            case WM_CREATE: self->create(); return 0;
            case WM_SIZE: self->layout(); return 0;
            case WM_ACTIVATE: InvalidateRect(window, nullptr, FALSE); break;
            case WM_ERASEBKGND: return 1;
            case WM_PAINT: {
                PAINTSTRUCT paint{};
                const HDC dc = BeginPaint(window, &paint);
                RECT area{};
                GetClientRect(window, &area);
                HDC frame = CreateCompatibleDC(dc);
                HBITMAP bitmap = CreateCompatibleBitmap(dc, area.right, area.bottom);
                const auto old = SelectObject(frame, bitmap);
                self->draw(frame);
                BitBlt(dc, 0, 0, area.right, area.bottom, frame, 0, 0, SRCCOPY);
                SelectObject(frame, old);
                DeleteObject(bitmap);
                DeleteDC(frame);
                EndPaint(window, &paint);
                return 0;
            }
            case WM_DRAWITEM: self->draw_item(*reinterpret_cast<DRAWITEMSTRUCT*>(lp)); return TRUE;
            case WM_NOTIFY: {
                auto* info = reinterpret_cast<NMTTDISPINFOW*>(lp);
                if (info->hdr.hwndFrom == self->tooltip && info->hdr.code == TTN_GETDISPINFOW) {
                    self->tooltip_text = self->tooltip_label(reinterpret_cast<HWND>(info->hdr.idFrom));
                    info->lpszText = self->tooltip_text.data();
                    return 0;
                }
                break;
            }
            case WM_CTLCOLORLISTBOX:
                SetBkColor(reinterpret_cast<HDC>(wp), panel);
                return reinterpret_cast<LRESULT>(self->list_background);
            case WM_CTLCOLORSTATIC:
                SetBkColor(reinterpret_cast<HDC>(wp), background);
                SetTextColor(reinterpret_cast<HDC>(wp), ink);
                SetDCBrushColor(reinterpret_cast<HDC>(wp), background);
                return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
            case WM_MOUSEWHEEL:
                if (self->preview.saved && !self->busy) self->page(self->tab, GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1);
                return 0;
            case WM_COMMAND:
                if (LOWORD(wp) == list_id && HIWORD(wp) == LBN_SELCHANGE) self->select();
                else if ((LOWORD(wp) == play_id && HIWORD(wp) == BN_CLICKED) ||
                         (LOWORD(wp) == list_id && HIWORD(wp) == LBN_DBLCLK))
                    self->start(false);
                else if (LOWORD(wp) == new_id && HIWORD(wp) == BN_CLICKED) self->start(true);
                else if (LOWORD(wp) == rename_id && HIWORD(wp) == BN_CLICKED) self->change_name();
                else if (LOWORD(wp) == update_id && HIWORD(wp) == BN_CLICKED) self->check_updates();
                else if (LOWORD(wp) >= inventory_id && LOWORD(wp) <= quest_id && HIWORD(wp) == BN_CLICKED)
                    self->change_tab(LOWORD(wp) - inventory_id);
                else if (LOWORD(wp) >= previous_id && LOWORD(wp) < previous_id + 3 && HIWORD(wp) == BN_CLICKED)
                    self->page(LOWORD(wp) - previous_id, -1);
                else if (LOWORD(wp) >= next_id && LOWORD(wp) < next_id + 3 && HIWORD(wp) == BN_CLICKED)
                    self->page(LOWORD(wp) - next_id, 1);
                else if (LOWORD(wp) >= cell_id && LOWORD(wp) < cell_id + self->cells.size() &&
                         (HIWORD(wp) == BN_CLICKED || HIWORD(wp) == BN_SETFOCUS)) {
                    const size_t cell = LOWORD(wp) - cell_id;
                    self->tab = unsigned(cell / InventoryPage::cells);
                    self->focused_item = self->inventories[self->tab].first() + cell % InventoryPage::cells;
                    self->update_items();
                }
                return 0;
            case finished: self->complete(wp != 0, unsigned(lp)); return 0;
            case WM_CLOSE:
                if (self->checking) {
                    self->closing_after_check = true;
                    ShowWindow(window, SW_HIDE);
                    return 0;
                }
                if (self->busy) return 0;
                break;
            case WM_DESTROY: PostQuitMessage(0); return 0;
            }
        } catch (const std::exception& e) {
            MessageBoxW(window, wide(e.what()).c_str(), L"Hookshot", MB_OK | MB_ICONERROR);
            if (message == WM_CREATE) return -1;
        }
        return DefWindowProcW(window, message, wp, lp);
    }

    int run(int show) {
        WNDCLASSW type{};
        type.lpfnWndProc = proc;
        type.hInstance = instance;
        type.lpszClassName = L"HookshotFileSelect";
        type.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        if (!RegisterClassW(&type)) return 1;
        constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
        const RECT bounds = selector_outer_bounds(style);
        const HWND handle = CreateWindowW(type.lpszClassName, L"Hookshot — Select Your File", style,
            CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
            nullptr, nullptr, instance, this);
        if (!handle) return 1;
        ShowWindow(handle, show);
        SetFocus(list);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (message.message == WM_KEYDOWN && !busy) {
                const auto cell = std::find(cells.begin(), cells.end(), message.hwnd);
                const auto category = std::find(tabs.begin(), tabs.end(), message.hwnd);
                const auto prev = std::find(previous.begin(), previous.end(), message.hwnd);
                const auto following = std::find(next.begin(), next.end(), message.hwnd);
                const bool content = cell != cells.end() || category != tabs.end() ||
                                     prev != previous.end() || following != next.end();
                const unsigned keyboard_group = category != tabs.end() ? unsigned(category - tabs.begin()) :
                    prev != previous.end() ? unsigned(prev - previous.begin()) :
                    following != next.end() ? unsigned(following - next.begin()) : tab;
                if ((GetKeyState(VK_CONTROL) & 0x8000) && message.wParam >= '1' && message.wParam <= '3') {
                    change_tab(unsigned(message.wParam - '1'));
                    SetFocus(tabs[tab]);
                    continue;
                }
                if (category != tabs.end() && (message.wParam == VK_LEFT || message.wParam == VK_RIGHT)) {
                    change_tab((keyboard_group + (message.wParam == VK_LEFT ? unsigned(tabs.size() - 1) : 1)) % unsigned(tabs.size()));
                    SetFocus(tabs[tab]);
                    continue;
                }
                if (preview.saved && content && (message.wParam == VK_PRIOR || message.wParam == VK_NEXT)) {
                    page(keyboard_group, message.wParam == VK_PRIOR ? -1 : 1);
                    continue;
                }
                if (cell != cells.end()) {
                    int delta = 0;
                    if (message.wParam == VK_LEFT) delta = -1;
                    if (message.wParam == VK_RIGHT) delta = 1;
                    if (message.wParam == VK_UP) delta = -int(InventoryPage::columns);
                    if (message.wParam == VK_DOWN) delta = int(InventoryPage::columns);
                    if (delta) { focus_item(inventories[tab].move(focused_item, delta)); continue; }
                    if (message.wParam == VK_HOME || message.wParam == VK_END) {
                        focus_item(message.wParam == VK_HOME ? 0 : inventories[tab].count - 1);
                        continue;
                    }
                }
            }
            if (message.message == WM_KEYDOWN && message.hwnd == list && message.wParam == VK_RETURN) {
                SendMessageW(window, WM_COMMAND, play_id, 0);
                continue;
            }
            if (message.message == WM_KEYDOWN && message.wParam == VK_F2 && !busy) {
                SendMessageW(window, WM_COMMAND, rename_id, 0);
                continue;
            }
            if (!IsDialogMessageW(window, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        return int(message.wParam);
    }
};

} // namespace
} // namespace file_select

int hookshot_file_select_command(const wchar_t* args) {
    return !*args || !wcscmp(args, L"--list") || !wcsncmp(args, L"--launch-file ", 14);
}

int hookshot_file_select(HINSTANCE instance, const wchar_t* folder, const wchar_t* args, int show) {
    using namespace file_select;
    const bool console = *args != L'\0';
    try {
        Store store(folder);
        if (console) AttachConsole(ATTACH_PARENT_PROCESS);
        fs::create_directories(store.user);
        if (!ordinary(store.user)) throw std::runtime_error("The user folder is a link.");
        Handle lock(CreateFileW((store.user / L".file-select.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (lock.value == INVALID_HANDLE_VALUE) throw std::runtime_error("File Select is already open or its folder is read-only.");
        store.recover();
        if (console) {
            if (!wcscmp(args, L"--list")) {
                report(L"Installed Hookshot: " + wide(installed(store.root)));
                for (const unsigned number : store.files()) {
                    const auto info = store.preview(number);
                    auto line = L"File " + std::to_wstring(number) + L": " +
                        (info.saved ? scene_name(info.caption.scene) : L"New Game");
                    if (info.progress.capacity_quarters && info.progress.health_quarters)
                        line += L" (" + std::to_wstring(*info.progress.health_quarters) + L"/" +
                                std::to_wstring(*info.progress.capacity_quarters) + L" quarter-hearts)";
                    report(line);
                }
                report(L"Active file: " + std::to_wstring(store.active()));
                return 0;
            }
            const unsigned number = file_number(L"file" + std::wstring(args + 14));
            if (!number || !fs::is_directory(store.profile(number))) throw std::runtime_error("Unknown file number.");
            return int(store.play(number, false));
        }
        Gdiplus::GdiplusStartupInput config;
        ULONG_PTR graphics = 0;
        if (Gdiplus::GdiplusStartup(&graphics, &config, nullptr) != Gdiplus::Ok)
            throw std::runtime_error("Windows could not start the image viewer.");
        const int result = Window(store, instance).run(show);
        Gdiplus::GdiplusShutdown(graphics);
        return result;
    } catch (const std::exception& error) {
        const auto message = wide(error.what());
        if (console) report(message);
        else MessageBoxW(nullptr, message.c_str(), L"Hookshot", MB_OK | MB_ICONERROR);
        return 1;
    }
}
