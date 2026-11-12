"""Build local-only selector metadata from the user's supported RomFS.

No field-ID table is distributed: candidate names must resolve to uniquely
typed, saved fields in this dump. The output is display metadata, not permission
to edit story state. In particular, Scarecrow's Song also needs recorded notes,
and changing age involves scene, equipment and transition state.
"""
import argparse
from pathlib import Path
import re
import struct
import sys


def field_hash(name):
    """MurmurHash3 x86 32-bit, seed zero (UTF-8 field names)."""
    data = name.encode("utf-8")
    mask = 0xffffffff

    def rotate(value, shift):
        return ((value << shift) | (value >> (32 - shift))) & mask

    def mix(value):
        return rotate(value * 0xcc9e2d51 & mask, 15) * 0x1b873593 & mask

    result = 0
    full = len(data) & ~3
    for offset in range(0, full, 4):
        result ^= mix(int.from_bytes(data[offset:offset + 4], "little"))
        result = (rotate(result, 13) * 5 + 0xe6546b64) & mask
    result ^= mix(int.from_bytes(data[full:], "little"))
    result ^= len(data)
    result ^= result >> 16
    result = result * 0x85ebca6b & mask
    result ^= result >> 13
    result = result * 0xc2b2ae35 & mask
    return result ^ (result >> 16)


class Byml:
    """Small bounds-checked BYML reader; supports either byte order."""
    def __init__(self, data):
        self.data = data
        if len(data) < 16 or data[:2] not in (b"BY", b"YB"):
            raise ValueError("invalid BYML header")
        self.order = "big" if data[:2] == b"BY" else "little"
        if self.number(2, 2) not in range(2, 8):
            raise ValueError("unsupported BYML version")
        self.keys = self.strings(self.number(4))
        self.values = self.strings(self.number(8))
        self.active = set()

    def span(self, offset, size):
        if offset < 0 or size < 0 or offset + size > len(self.data):
            raise ValueError("BYML offset out of bounds")
        return self.data[offset:offset + size]

    def number(self, offset, size=4, signed=False):
        return int.from_bytes(self.span(offset, size), self.order, signed=signed)

    def strings(self, offset):
        if not offset:
            return []
        if self.span(offset, 1) != b"\xc2":
            raise ValueError("invalid BYML string table")
        count = self.number(offset + 1, 3)
        self.span(offset + 4, (count + 1) * 4)
        output = []
        for index in range(count):
            start = offset + self.number(offset + 4 + index * 4)
            stop = offset + self.number(offset + 8 + index * 4)
            raw = self.span(start, stop - start)
            if not raw or raw[-1:] != b"\0":
                raise ValueError("unterminated BYML string")
            output.append(raw[:-1].decode("utf-8"))
        return output

    def value(self, kind, position, depth):
        value = self.number(position)
        if kind == 0xa0:
            if value >= len(self.values):
                raise ValueError("invalid BYML string index")
            return self.values[value]
        if kind in (0xc0, 0xc1):
            return self.node(value, depth + 1)
        if kind == 0xd0:
            if value not in (0, 1):
                raise ValueError("invalid BYML boolean")
            return bool(value)
        if kind == 0xd1:
            return self.number(position, signed=True)
        if kind == 0xd2:
            return struct.unpack(">f" if self.order == "big" else "<f", self.span(position, 4))[0]
        if kind == 0xd3:
            return value
        if kind in (0xd4, 0xd5):
            return self.number(value, 8, signed=kind == 0xd4)
        if kind == 0xd6:
            return struct.unpack(">d" if self.order == "big" else "<d", self.span(value, 8))[0]
        if kind == 0xff:
            return None
        raise ValueError("unsupported BYML value type")

    def node(self, offset=None, depth=0):
        if offset is None:
            offset = self.number(12)
        if depth > 64 or offset in self.active:
            raise ValueError("recursive BYML container")
        kind = self.number(offset, 1)
        count = self.number(offset + 1, 3)
        self.active.add(offset)
        try:
            if kind == 0xc0:
                self.span(offset + 4, count)
                start = (offset + 4 + count + 3) & ~3
                self.span(start, count * 4)
                return [self.value(self.number(offset + 4 + i, 1), start + i * 4, depth)
                        for i in range(count)]
            if kind == 0xc1:
                self.span(offset + 4, count * 8)
                output = {}
                for i in range(count):
                    pos = offset + 4 + i * 8
                    key = self.number(pos, 3)
                    if key >= len(self.keys) or self.keys[key] in output:
                        raise ValueError("invalid or duplicate BYML key")
                    output[self.keys[key]] = self.value(self.number(pos + 3, 1), pos + 4, depth)
                return output
            raise ValueError("invalid BYML container type")
        finally:
            self.active.remove(offset)


def saved_bool_fields(document):
    """Exclude duplicate hashes, non-booleans and fields outside progress.sav."""
    data = document.get("Data")
    meta = document.get("MetaData")
    if not isinstance(data, dict) or not isinstance(meta, dict):
        raise ValueError("missing save schema")
    directories = meta.get("SaveDirectory", [])
    if not directories or directories[0] != "slot_%02d/progress.sav":
        raise ValueError("unsupported progress save index")
    seen, valid = set(), set()
    for kind, rows in data.items():
        if not isinstance(rows, list):
            raise ValueError("invalid save schema records")
        for row in rows:
            key = row.get("Hash")
            if type(key) is not int or not 0 <= key <= 0xffffffffffffffff:
                raise ValueError("invalid save field hash")
            if key > 0xffffffff:
                continue  # 64-bit-key data does not belong to the scalar table.
            if key in seen:
                raise ValueError("duplicate save field hash")
            seen.add(key)
            if kind == "Bool" and row.get("SaveFileIndex") == 0:
                if type(row.get("DefaultValue")) is not bool:
                    raise ValueError("invalid boolean default")
                valid.add(key)
    return valid


def song_label(actor):
    word = actor.removeprefix("Item_Melody_")
    parts = re.findall(r"[A-Z][a-z]*|[0-9]+", word)
    if len(parts) == 2 and parts[1] == "Song":
        subject = parts[0]
        return (subject + "'s Song" if subject in ("Epona", "Saria", "Scarecrow")
                else "Sun's Song" if subject == "Sun"
                else "Song of " + ("Storms" if subject == "Storm" else subject))
    if len(parts) == 2 and parts[1] == "Lullaby":
        return parts[0] + "'s Lullaby"
    if len(parts) == 2:
        return parts[1] + " of " + parts[0]
    return " ".join(parts)


def metadata_rows(document, actor_names):
    """Every emitted ownership field is a validated progress Bool (0 or 1).

    TSV song columns: kind, actor, label, texture, ownership, get-event.
    The get-event field is supplied as evidence of a separate state, not a
    prescription to set it. Age: kind, field, child-value, adult-value.
    """
    fields = saved_bool_fields(document)
    rows = []
    for actor in sorted(set(actor_names)):
        if not re.fullmatch(r"Item_Melody_[A-Za-z0-9_]+", actor):
            raise ValueError("unsafe melody actor name")
        owned = field_hash("IsGet." + actor)
        event = field_hash("HavePlayedGetEvent." + actor)
        if owned not in fields or event not in fields:
            raise ValueError("melody has no validated saved ownership/event pair")
        rows.append(("song", actor, song_label(actor), "Item_Ocarina_Time", str(owned), str(event)))
    age = field_hash("Sequence_IsChildhood")
    if age in fields:
        rows.append(("age", str(age), "1", "0"))
    defense = field_hash("IsGet.Item_FairyBless_DefenseDouble")
    if defense in fields:
        rows.append(("defense", str(defense)))
    return rows


def generate(romfs, output):
    # The existing prompt pipeline supplies zstd handling; imports remain lazy
    # so all parser/validation tests run without game files or dependencies.
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from prompt_assets import read_zs, zs_dictionary
    root = Path(romfs)
    candidates = list((root / "GameData").glob("GameDataList.Product.*.byml.zs"))
    if len(candidates) != 1:
        raise ValueError("expected exactly one GameData schema")
    data = read_zs(root, str(candidates[0].relative_to(root)), zs_dictionary(root))
    actors = [path.name.removesuffix(".pack.zs")
              for path in (root / "Pack" / "Actor").glob("Item_Melody_*.pack.zs")]
    if not actors:
        raise ValueError("no melody actors found")
    rows = metadata_rows(Byml(data).node(), actors)
    target = Path(output)
    target.parent.mkdir(parents=True, exist_ok=True)
    text = "HOOKSHOT_SAVE_METADATA\t1\n" + "".join("\t".join(row) + "\n" for row in rows)
    temporary = target.with_name(target.name + ".tmp")
    temporary.write_text(text, encoding="utf-8", newline="\n")
    temporary.replace(target)
    return len(actors)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--romfs", required=True)
    parser.add_argument("--out", required=True, help="local generated .tsv path; never distribute")
    args = parser.parse_args()
    print(f"Generated local metadata for {generate(args.romfs, args.out)} songs")
