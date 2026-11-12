"""Local metadata fails closed on wrong types, unsaved fields and bad BYML."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("metadata", ROOT / "nx2/file_select_metadata.py")
metadata = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(metadata)


class MetadataTests(unittest.TestCase):
    def document(self, kind="Bool", save_index=0):
        return {"MetaData": {"SaveDirectory": ["slot_%02d/progress.sav"]},
                "Data": {kind: [{"Hash": metadata.field_hash(prefix + "Item_Melody_TestSong"),
                                  "SaveFileIndex": save_index, "DefaultValue": False}
                                 for prefix in ("IsGet.", "HavePlayedGetEvent.")]}}

    def test_hash_independent_known_vectors(self):
        self.assertEqual(metadata.field_hash(""), 0)
        self.assertEqual(metadata.field_hash("foo"), 0xf6a5c420)
        self.assertEqual(metadata.field_hash("hello"), 0x248bfa47)

    def test_saved_song(self):
        rows = metadata.metadata_rows(self.document(), ["Item_Melody_TestSong"])
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0][:4], ("song", "Item_Melody_TestSong", "Song of Test", "Item_Ocarina_Time"))
        self.assertNotEqual(rows[0][4], rows[0][5])

    def test_wrong_type_unsaved_missing_related(self):
        for doc in (self.document("Int"), self.document(save_index=2)):
            with self.assertRaises(ValueError):
                metadata.metadata_rows(doc, ["Item_Melody_TestSong"])
        doc = self.document()
        doc["Data"]["Bool"].pop()
        with self.assertRaises(ValueError):
            metadata.metadata_rows(doc, ["Item_Melody_TestSong"])

    def test_collision_and_unsafe_actor(self):
        doc = self.document()
        doc["Data"]["Bool"].append(doc["Data"]["Bool"][0].copy())
        with self.assertRaises(ValueError):
            metadata.saved_bool_fields(doc)
        with self.assertRaises(ValueError):
            metadata.metadata_rows(self.document(), ["Item_Melody_Test\tSong"])

    def test_both_byte_orders_and_truncated_nodes(self):
        # A synthetic BYML root array with a single Bool, no string tables.
        for order, magic in (("little", b"YB"), ("big", b"BY")):
            def number(value, size=4):
                return value.to_bytes(size, order)
            data = magic + number(7, 2) + bytes(8) + number(16)
            data += b"\xc0" + number(1, 3) + b"\xd0\0\0\0" + number(1)
            self.assertEqual(metadata.Byml(data).node(), [True])
            for cut in (0, 15, 19, len(data) - 1):
                with self.assertRaises(ValueError):
                    metadata.Byml(data[:cut]).node()
            bad_bool = data[:-4] + number(2)
            with self.assertRaises(ValueError):
                metadata.Byml(bad_bool).node()
            cycle = data[:20] + b"\xc0\0\0\0" + number(16)
            with self.assertRaises(ValueError):
                metadata.Byml(cycle).node()


if __name__ == "__main__":
    unittest.main()
