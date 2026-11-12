"""The Windows selector can read save previews without the game."""

import argparse
import ast
import os
from pathlib import Path
import subprocess
import sys
import tempfile


RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include <cassert>
#include "file_select_metadata.h"

using namespace file_select;

void put(Bytes& bytes, size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) bytes[offset + i] = static_cast<unsigned char>(value >> (i * 8));
}

Bytes save(size_t length, uint32_t table) {
    Bytes bytes(length);
    put(bytes, 0, 0x01020304);
    put(bytes, 4, 0x002d80eb);
    put(bytes, 8, table);
    return bytes;
}

int main() {
    Bytes preview = save(1402, 0x2a0);
    preview[684] = 'A';
    put(preview, 1392, 6);
    preview[1396] = 0xff;
    preview[1397] = 0xd8;
    preview[1400] = 0xff;
    preview[1401] = 0xd9;
    assert(read_caption(preview).scene == "A");
    assert(read_caption(preview).jpeg.size() == 6);
    put(preview, 1392, UINT32_MAX);
    assert(read_caption(preview).jpeg.empty());
    preview.resize(1397);
    assert(read_caption(preview).scene.empty());

    Bytes progress = save(0x175d8, 0x175d8);
    const std::array<std::pair<uint32_t, uint32_t>, 7> fields{{
        {0x31ab5580, 11}, {0xfbe01da1, 16}, {0x76b18200, 163}, {0x12d63a97, 240},
        {equipment[0].stock, 1}, {equipment[1].stock, 0}, {equipment[2].stock, UINT32_MAX},
    }};
    for (size_t i = 0; i < fields.size(); ++i) {
        put(progress, 40 + i * 8, fields[i].first);
        put(progress, 44 + i * 8, fields[i].second);
    }
    const auto decoded = read_progress(progress);
    assert(decoded.health_quarters == 11 && decoded.capacity_quarters == 16);
    assert(decoded.magic_tenths == 163 && decoded.magic_capacity_tenths == 240);
    assert(decoded.owned.size() == 1 && decoded.owned.front() == &equipment[0]);
    put(progress, 44, 17);
    assert(!read_progress(progress).health_quarters);
    put(progress, 44, 11);
    put(progress, 68, 0);
    assert(!read_progress(progress).magic_tenths);
    put(progress, 96, fields.front().first);
    assert(!read_progress(progress).health_quarters && read_progress(progress).owned.empty());
    progress.resize(8);
    assert(!read_progress(progress).capacity_quarters);

    // Full inventory, late-game vitals and real counts must all survive decoding.
    progress = save(0x175d8, 0x175d8);
    size_t offset = 40;
    const auto field = [&](uint32_t id, uint32_t value) {
        put(progress, offset, id); put(progress, offset + 4, value); offset += 8;
    };
    field(progress_vitals[0], 79); field(progress_vitals[1], 80);
    field(progress_vitals[2], 480); field(progress_vitals[3], 480);
    field(0xa77921d7, 999); field(0xe0010a50, 1000);
    for (const auto& item : equipment) {
        field(item.stock, item.max_quantity ? item.max_quantity : 1);
        field(item.acquired, 1);
    }
    auto full = read_progress(progress);
    assert(full.owned.size() == std::size(equipment));
    assert(full.owned.size() == full.quantities.size());
    assert(full.health_quarters == 79 && full.capacity_quarters == 80);
    assert(full.magic_capacity_tenths == 480 && full.rupees == 999 && full.double_defense);
    // Both eras' starting clothes belong to equipment, never the consumable item grid.
    for (const auto& item : equipment)
        if (item.stock == 0x84ba0d24 || item.stock == 0x6f631388)
            assert(item.category == ItemCategory::equipment);
    for (size_t i = 0; i < full.owned.size(); ++i) {
        if (full.owned[i]->max_quantity) assert(full.quantities[i] == full.owned[i]->max_quantity);
        else assert(!full.quantities[i]);
    }
    // A consumed supply stays unlocked while its current quantity is zero.
    const auto bomb = find_field(progress, 0x175d8, 0xdb1bcd4f);
    assert(bomb); put(progress, *bomb, 0);
    full = read_progress(progress);
    bool found = false;
    for (size_t i = 0; i < full.owned.size(); ++i)
        if (full.owned[i]->stock == 0xdb1bcd4f) { assert(full.quantities[i] == 0); found = true; }
    assert(found);
    put(progress, *bomb, UINT32_MAX);
    assert(read_progress(progress).owned.size() == std::size(equipment) - 1);

    // Bottles show their actual contents and aggregate only matching contents.
    for (size_t i = 0; i < bottle_slots.size(); ++i) {
        field(bottle_slots[i][0], 1); field(bottle_slots[i][1], 1);
        field(bottle_slots[i][2], bottle_contents[i % 2].stock);
    }
    full = read_progress(progress);
    assert(full.owned[full.owned.size() - 2] == &bottle_contents[0]);
    assert(full.quantities[full.quantities.size() - 2] == 3 && full.quantities.back() == 2);

    const std::string header = "HOOKSHOT_SAVE_METADATA\t1\n";
    auto metadata = read_metadata(header + "song\tItem_Melody_Test\tAuthored melody\tItem_Ocarina_Time\t101\t102\n"
                                  "age\t103\t1\t0\n");
    assert(metadata.songs.size() == 1 && metadata.age == 103);
    field(101, 1); field(103, 0);
    full = read_progress(progress, metadata.items(), metadata.age);
    assert(full.childhood == false && full.owned.back() == &metadata.songs[0]->item);
    put(progress, *find_field(progress, 0x175d8, 101), 2);
    assert(read_progress(progress, metadata.items()).owned.size() == full.owned.size() - 1);
    field(103, 1);
    assert(!read_progress(progress, metadata.items(), metadata.age).childhood);
    assert(read_metadata(header + "song\tx\tTest\t../outside\t101\t102\n").songs.empty());
    assert(read_metadata(header + "song\tx\tTest\ticon\t101\t102\n"
                                "song\ty\tTest\ticon\t101\t102\n").songs.empty());
    assert(read_metadata("unknown\n").songs.empty());
}
"""

WINDOWS_FIXTURE = r"""
#define NOMINMAX
#include <cassert>
#include "file_select_store.h"

using namespace file_select;

void save(const fs::path& root, const char* text) {
    const auto slot = root / "slot_00";
    fs::create_directories(slot);
    std::ofstream(slot / "progress.sav", std::ios::binary) << text;
}

std::string read(const fs::path& root) {
    const auto bytes = read_bytes(root / "slot_00" / "progress.sav", 1024);
    return {bytes.begin(), bytes.end()};
}

int main(int argc, char** argv) {
    const Store store(fs::current_path());
    if (argc == 1) {
        save(store.mount, "written by game");
        return 0;
    }
    assert(argc == 2);
    const fs::path self = argv[1];
    Store fixture(fs::current_path());
    fs::create_directories(fixture.game.parent_path());
    fs::copy_file(self, fixture.game);
    save(fixture.mount, "original");
    const auto legacy = fixture.root / "profiles";
    save(legacy / "file1", "original");
    fs::create_directory(legacy / "file5");
    write_number(legacy / "active.txt", 1);
    fixture.recover();
    assert(fixture.files() == (std::vector<unsigned>{1, 5}));
    assert(fixture.next_file() == 6);
    assert(read(fixture.profile(1)) == "original");

    // Simulate termination after the incoming mount and active marker moved,
    // before the rollback marker could be removed.
    const auto stage = fixture.mount.parent_path() / (L"." + fixture.mount.filename().wstring() + L".swap");
    fs::create_directory(stage);
    fs::rename(fixture.mount, stage / "previous");
    std::ofstream(stage / "pending") << "1 1\n";
    save(fixture.mount, "interrupted");
    write_number(fixture.profiles / "active.txt", 5);
    fixture.recover();
    assert(fixture.active() == 1 && read(fixture.mount) == "original");
    assert(!fs::exists(stage));
    assert(fixture.play(5, false) == 0);
    assert(fixture.active() == 5 && read(fixture.profile(5)) == "written by game");
    assert(read(fixture.profile(1)) == "original" && read(legacy / "file1") == "original");
}
"""


def verify_build_marker(directory):
    source = ROOT / "nx2/make_game.py"
    tree = ast.parse(source.read_text(encoding="utf-8"), filename=str(source))
    functions = {"setup_has_file_select", "write_installed_version"}
    module = ast.Module(body=[node for node in tree.body
                              if isinstance(node, ast.FunctionDef) and node.name in functions], type_ignores=[])
    fixture = {"os": os, "tempfile": tempfile}
    exec(compile(module, str(source), "exec"), fixture)
    detect, write = fixture["setup_has_file_select"], fixture["write_installed_version"]
    setup = directory / "setup.exe"
    version = directory / "hookshot_version.txt"
    setup.write_bytes(b"MZ legacy setup")
    assert not detect(setup)
    write(directory, "v2.1.0", detect(setup), "portable")
    assert version.read_text() == "version=v2.1.0\nbuilder_local=1\n"
    setup.write_bytes(b"MZ" + "Hookshot File Select v1".encode("utf-16le"))
    assert detect(setup)
    write(directory, "", detect(setup), "portable")
    assert version.read_text() == "version=development\nfile_select=1\nbuilder_local=1\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="hookshot-file-select-test-") as temp:
        directory = Path(temp)
        source = directory / "preview.cpp"
        binary = directory / "preview.exe"
        source.write_text(FIXTURE, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "builder/setup"), str(source), "-o", str(binary)],
                       check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=20)
        verify_build_marker(directory)
        if sys.platform == "win32":
            source.write_text(WINDOWS_FIXTURE, encoding="utf-8")
            subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "builder/setup"), str(source), "-o", str(binary)],
                           check=True, timeout=60)
            subprocess.run([str(binary), str(binary)], cwd=directory, check=True, timeout=20)
    print("File Select preview and build markers passed, plus save recovery on Windows.")


if __name__ == "__main__":
    main()
