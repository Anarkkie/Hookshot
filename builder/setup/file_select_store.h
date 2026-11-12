#ifndef HOOKSHOT_FILE_SELECT_STORE_H
#define HOOKSHOT_FILE_SELECT_STORE_H

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

#include "file_select_save.h"
#include "file_select_metadata.h"

namespace file_select {

namespace fs = std::filesystem;

struct Handle {
    HANDLE value;
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value(value) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

inline bool ordinary(const fs::path& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_REPARSE_POINT);
}

inline void check_tree(const fs::path& root) {
    if (!fs::exists(root)) return;
    if (!ordinary(root)) throw std::runtime_error("The save folder is a link.");
    for (const auto& entry : fs::recursive_directory_iterator(root))
        if (!ordinary(entry.path())) throw std::runtime_error("A save entry is a link.");
}

inline void move_path(const fs::path& from, const fs::path& to, DWORD flags = 0) {
    if (!MoveFileExW(from.c_str(), to.c_str(), flags | MOVEFILE_WRITE_THROUGH))
        throw std::system_error(GetLastError(), std::system_category(), "Could not move the save data");
}

inline void write_number(const fs::path& path, unsigned value) {
    auto temp = path;
    temp += L".new";
    std::ofstream out(temp, std::ios::trunc);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out << value << '\n';
    out.close();
    move_path(temp, path, MOVEFILE_REPLACE_EXISTING);
}

inline unsigned read_number(const fs::path& path) {
    std::ifstream in(path);
    unsigned number = 0;
    std::string extra;
    if (!(in >> number) || number == 0 || (in >> extra))
        throw std::runtime_error("The selected save file could not be read.");
    return number;
}

inline void copy_tree(const fs::path& from, const fs::path& to) {
    check_tree(from);
    fs::create_directories(to);
    for (const auto& entry : fs::recursive_directory_iterator(from)) {
        if (entry.path().filename() == L"launcher_meta.json") continue;
        const auto dest = to / entry.path().lexically_relative(from);
        if (entry.is_directory()) fs::create_directory(dest);
        else if (entry.is_regular_file()) {
            fs::copy_file(entry.path(), dest);
            fs::last_write_time(dest, entry.last_write_time());
        }
    }
}

inline bool has_saves(const fs::path& path) {
    if (!fs::is_directory(path)) return false;
    check_tree(path);
    for (const auto& entry : fs::recursive_directory_iterator(path))
        if (entry.is_regular_file() && entry.path().extension() == L".sav") return true;
    return false;
}

inline Bytes read_bytes(const fs::path& path, size_t limit) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    const auto size = in.tellg();
    if (size <= 0 || size > static_cast<std::streamoff>(limit)) return {};
    Bytes bytes(static_cast<size_t>(size));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) return {};
    return bytes;
}

inline unsigned file_number(const std::wstring& name) {
    if (name.size() < 5 || name.compare(0, 4, L"file") || name[4] == L'0') return 0;
    unsigned number = 0;
    for (size_t i = 4; i < name.size(); ++i) {
        const wchar_t digit = name[i];
        if (digit < L'0' || digit > L'9' ||
            number > (std::numeric_limits<unsigned>::max() - unsigned(digit - L'0')) / 10) return 0;
        number = number * 10 + unsigned(digit - L'0');
    }
    return number;
}

struct Preview {
    bool saved = false;
    Caption caption;
    Progress progress;
};

class Store {
public:
    const fs::path root, user, profiles, mount, game;
    const SaveMetadata metadata;
    explicit Store(const fs::path& folder) :
        root(folder), user(root / L"user"), profiles(user / L"file-select"),
        mount(user / L"save" / L"5a3c9e1f0b7d24680000000000000001"),
        game(root / L"bin" / L"hookshot.exe"), metadata(load_metadata(root)) {}

    static SaveMetadata load_metadata(const fs::path& root) {
        const auto bytes = read_bytes(root / L"bin" / L"file-select-art" / L"save-metadata.tsv", 65536);
        return read_metadata(std::string(bytes.begin(), bytes.end()));
    }

    fs::path profile(unsigned number) const { return profiles / (L"file" + std::to_wstring(number)); }
    unsigned active() const { return read_number(profiles / L"active.txt"); }

    std::vector<unsigned> files() const {
        std::vector<unsigned> result;
        for (const auto& entry : fs::directory_iterator(profiles)) {
            const unsigned number = file_number(entry.path().filename().wstring());
            if (number && entry.is_directory()) result.push_back(number);
        }
        std::sort(result.begin(), result.end());
        return result;
    }

    unsigned next_file() const {
        const auto existing = files();
        if (existing.back() == std::numeric_limits<unsigned>::max())
            throw std::runtime_error("There is no unused file number.");
        return existing.back() + 1;
    }

    void recover() {
        if (game_running()) throw std::runtime_error("Close Hookshot before changing files.");
        if (!ordinary(user) || (fs::exists(mount.parent_path()) && !ordinary(mount.parent_path())))
            throw std::runtime_error("The user or save folder is a link.");
        if (!fs::exists(profiles)) initialize();
        if (!ordinary(profiles)) throw std::runtime_error("The file selector's save folder is a link.");
        recover_swap(mount);
        for (const auto& entry : fs::directory_iterator(profiles)) {
            const auto name = entry.path().filename().wstring();
            if (name.size() > 6 && name[0] == L'.' && name.substr(name.size() - 5) == L".swap") {
                const auto file = name.substr(1, name.size() - 6);
                if (file_number(file)) recover_swap(profiles / file);
            }
        }
        if (!fs::is_directory(profile(active()))) throw std::runtime_error("The active file is missing.");
        if (has_saves(mount)) replace(profile(active()), mount);
        else restore_mount();
    }

    Preview preview(unsigned number) const {
        Preview result;
        const auto latest = latest_slot(number);
        if (latest.empty()) return result;
        result.saved = true;
        result.caption = read_caption(read_bytes(latest / L"caption.sav", 8 * 1024 * 1024));
        result.progress = read_progress(read_bytes(latest / L"progress.sav", 8 * 1024 * 1024), metadata.items(), metadata.age);
        return result;
    }

    fs::path latest_slot(unsigned number) const {
        const auto folder = profile(number);
        if (!fs::is_directory(folder)) return {};
        check_tree(folder);
        fs::path latest;
        fs::file_time_type stamp = fs::file_time_type::min();
        for (const auto& entry : fs::directory_iterator(folder)) {
            if (!entry.is_directory() || entry.path().filename().wstring().compare(0, 5, L"slot_")) continue;
            for (const auto* filename : {L"caption.sav", L"progress.sav"}) {
                const auto path = entry.path() / filename;
                if (!fs::is_regular_file(path)) continue;
                const auto modified = fs::last_write_time(path);
                if (modified > stamp || (modified == stamp && entry.path() > latest)) {
                    latest = entry.path();
                    stamp = modified;
                }
            }
        }
        return latest;
    }

    DWORD play(unsigned number, bool fresh) {
        if (!fs::is_regular_file(game)) throw std::runtime_error("This folder is missing the built game.");
        recover();
        const unsigned old = active();
        if (!fs::is_directory(profile(number))) fs::create_directory(profile(number));
        if (fresh || number != old) replace(mount, fresh ? fs::path() : profile(number), number);

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        SetEnvironmentVariableW(L"HOOKSHOT_NO_UPDATE_CHECK", L"1");
        if (!CreateProcessW(game.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, root.c_str(),
                            &startup, &process)) {
            const DWORD error = GetLastError();
            replace(mount, profile(old), old);
            throw std::system_error(error, std::system_category(), "Could not start Hookshot");
        }
        Handle main_thread(process.hThread), running(process.hProcess);
        if (WaitForSingleObject(running.value, INFINITE) != WAIT_OBJECT_0)
            throw std::runtime_error("Could not wait for Hookshot to close.");
        DWORD code = 1;
        GetExitCodeProcess(running.value, &code);
        // Restart now spawns its replacement game before the first process exits.
        while (game_running()) Sleep(250);
        if (fresh && code && !has_saves(mount)) replace(mount, profile(old), old);
        else replace(profile(number), mount);
        restore_mount();
        return code;
    }

private:
    bool game_running() const {
        Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (snapshot.value == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Could not check whether Hookshot is running.");
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (!Process32FirstW(snapshot.value, &entry)) return false;
        do {
            if (_wcsicmp(entry.szExeFile, L"hookshot.exe")) continue;
            Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
            if (!process.value) continue;
            std::wstring path(32768, L'\0');
            DWORD length = static_cast<DWORD>(path.size());
            if (QueryFullProcessImageNameW(process.value, 0, path.data(), &length) &&
                CompareStringOrdinal(path.data(), length, game.c_str(), -1, TRUE) == CSTR_EQUAL)
                return true;
        } while (Process32NextW(snapshot.value, &entry));
        return false;
    }

    void initialize() const {
        const auto staging = user / L".file-select.import";
        if (fs::exists(staging)) {
            check_tree(staging);
            fs::remove_all(staging);
        }
        fs::create_directory(staging);
        const auto legacy = root / L"profiles";
        unsigned first = 1;
        if (fs::is_regular_file(legacy / L"active.txt")) {
            first = read_number(legacy / L"active.txt");
            if (!fs::is_directory(legacy / (L"file" + std::to_wstring(first))))
                throw std::runtime_error("The previous launcher's active file is missing.");
            check_tree(legacy);
            for (const auto& entry : fs::directory_iterator(legacy)) {
                const auto name = entry.path().filename();
                if (entry.is_directory() && file_number(name.wstring()))
                    copy_tree(entry.path(), staging / name);
            }
        } else {
            fs::create_directory(staging / L"file1");
        }
        write_number(staging / L"active.txt", first);
        move_path(staging, profiles);
    }

    void restore_mount() {
        if (has_saves(mount)) return;
        const unsigned current = active();
        if (has_saves(profile(current))) {
            replace(mount, profile(current), current);
            return;
        }
        for (const unsigned file : files()) {
            if (!has_saves(profile(file))) continue;
            replace(mount, profile(file), file);
            return;
        }
        fs::create_directories(mount);
    }

    static fs::path swap_dir(const fs::path& destination) {
        return destination.parent_path() / (L"." + destination.filename().wstring() + L".swap");
    }

    void recover_swap(const fs::path& destination) const {
        const auto stage = swap_dir(destination);
        if (!fs::exists(stage)) return;
        check_tree(stage);
        const auto marker = stage / L"pending";
        if (fs::exists(marker)) {
            unsigned previous_active = 0;
            bool existed = false;
            std::ifstream in(marker);
            if (!(in >> previous_active >> existed)) throw std::runtime_error("A save swap needs recovery.");
            in.close();
            const auto previous = stage / L"previous";
            const auto incoming = stage / L"incoming";
            if (fs::exists(previous)) {
                if (fs::exists(destination)) move_path(destination, incoming);
                move_path(previous, destination);
            } else if (!existed && fs::exists(destination) && !fs::exists(incoming)) {
                move_path(destination, incoming);
            }
            if (previous_active) write_number(profiles / L"active.txt", previous_active);
            fs::remove(marker);
        }
        fs::remove_all(stage);
    }

    void replace(const fs::path& destination, const fs::path& source, unsigned select = 0) const {
        recover_swap(destination);
        check_tree(destination);
        const auto stage = swap_dir(destination);
        fs::create_directories(stage);
        const auto incoming = stage / L"incoming";
        const auto marker = stage / L"pending";
        try {
            if (source.empty()) fs::create_directory(incoming);
            else copy_tree(source, incoming);
            const bool exists = fs::exists(destination);
            std::ofstream pending(stage / L"pending.new");
            pending.exceptions(std::ios::badbit | std::ios::failbit);
            pending << (select ? active() : 0) << ' ' << exists << '\n';
            pending.close();
            move_path(stage / L"pending.new", marker);
            if (exists) move_path(destination, stage / L"previous");
            move_path(incoming, destination);
            if (select) write_number(profiles / L"active.txt", select);
            fs::remove(marker);
        } catch (...) {
            recover_swap(destination);
            throw;
        }
        recover_swap(destination);
    }
};

} // namespace file_select

#endif
