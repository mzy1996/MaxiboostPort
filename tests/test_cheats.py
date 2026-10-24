from paths import ROOT
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(ROOT / 'scripts'))
import cheats  # noqa: E402


TABLE = {
    "name": "MAXIBOOST ON (Premium Edition)",
    "id": "CUSA15006",
    "version": "01.15",
    "process": "eboot.bin",
    "mods": [
        {
            "name": "Player Health 999",
            "type": "checkbox",
            "memory": [{"offset": "F06CEE", "on": "00", "off": "0A"}],
        },
        {
            "name": "Enemy 1 Hit Kill",
            "type": "checkbox",
            "memory": [{"offset": "F06D17", "on": "750641BC9F860100", "off": "EB0641BCE7030000"}],
        },
    ],
    "credits": ["Fullcodes"],
}


class CheatTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.path = self.root / 'CUSA15006_01.15.json'
        self.path.write_text(json.dumps(TABLE), encoding='utf-8')

    def tearDown(self):
        self.tmp.cleanup()

    def image(self):
        """A stand-in image holding the bytes the table expects."""
        image = bytearray(0x1000000)
        image[0xf06cee:0xf06cef] = bytes.fromhex('0a')
        image[0xf06d17:0xf06d20] = bytes.fromhex('eb0641bce7030000')
        return image

    def test_the_table_is_read(self):
        data = cheats.load(self.path)
        self.assertIsNotNone(data)
        self.assertEqual([name for name, _ in cheats.entries(data)],
                         ['Player Health 999', 'Enemy 1 Hit Kill'])

    def test_a_file_that_is_not_a_table_is_refused(self):
        self.assertIsNone(cheats.load(self.root / 'missing.json'))
        other = self.root / 'other.json'
        other.write_text('{"hello": 1}', encoding='utf-8')
        self.assertIsNone(cheats.load(other))

    def test_the_offsets_are_the_executable_s_own(self):
        """The offsets are into the executable's loaded image, not PS4 virtual addresses."""
        data = cheats.load(self.path)
        entries = cheats.entries(data)
        self.assertEqual(entries[0][1][0][0], 0xf06cee)

    def test_the_writes_of_the_enabled_cheats(self):
        data = cheats.load(self.path)
        writes = cheats.compile_writes(data, ['Player Health 999'], self.image(),
                                       'CUSA15006', '01.15')
        self.assertEqual(writes, [(0xf06cee, bytes.fromhex('00'))])

    def test_only_the_enabled_cheats_are_written(self):
        data = cheats.load(self.path)
        writes = cheats.compile_writes(data, [], self.image(), 'CUSA15006', '01.15')
        self.assertEqual(writes, [])

    def test_a_table_for_another_game_is_refused(self):
        data = cheats.load(self.path)
        with self.assertRaises(cheats.CheatError):
            cheats.compile_writes(data, ['Player Health 999'], self.image(), 'CUSA18758', '01.15')

    def test_a_table_for_another_version_is_refused(self):
        data = cheats.load(self.path)
        with self.assertRaises(cheats.CheatError):
            cheats.compile_writes(data, ['Player Health 999'], self.image(), 'CUSA15006', '01.20')

    def test_a_table_of_another_build_is_refused(self):
        """The bytes the executable holds are checked, so a table of another build cannot
        write over the game's code."""
        data = cheats.load(self.path)
        image = self.image()
        image[0xf06cee] = 0xff  # not the byte the table expects
        with self.assertRaises(cheats.CheatError):
            cheats.compile_writes(data, ['Player Health 999'], image, 'CUSA15006', '01.15')

    def test_the_configuration_chooses_the_cheats(self):
        config = self.root / 'cheats.json'
        config.write_text(json.dumps({'enabled': ['Enemy 1 Hit Kill']}), encoding='utf-8')
        names = [name for name, _ in cheats.entries(cheats.load(self.path))]
        self.assertEqual(cheats.enabled_from_config(config, names), ['Enemy 1 Hit Kill'])

    def test_without_a_configuration_no_cheat_is_applied(self):
        names = [name for name, _ in cheats.entries(cheats.load(self.path))]
        self.assertEqual(cheats.enabled_from_config(None, names), [])
        self.assertEqual(cheats.enabled_from_config(self.root / 'missing.json', names), [])

    def test_the_versions_are_compared_by_number(self):
        self.assertTrue(cheats.same_version('1.15', '01.15'))
        self.assertTrue(cheats.same_version('01.15', '01.15'))
        self.assertFalse(cheats.same_version('1.16', '01.15'))

    def test_a_malformed_entry_is_reported(self):
        broken = dict(TABLE)
        broken['mods'] = [{'name': 'Broken', 'type': 'checkbox',
                           'memory': [{'offset': 'ZZ', 'on': '00', 'off': '0A'}]}]
        path = self.root / 'broken.json'
        path.write_text(json.dumps(broken), encoding='utf-8')
        with self.assertRaises(cheats.CheatError):
            cheats.entries(cheats.load(path))

    def test_the_cheats_that_are_not_checkboxes_have_no_writes(self):
        other = dict(TABLE)
        other['mods'] = [{'name': 'A value', 'type': 'text', 'value': '5'}]
        path = self.root / 'other.json'
        path.write_text(json.dumps(other), encoding='utf-8')
        self.assertEqual(cheats.entries(cheats.load(path)), [])


if __name__ == '__main__':
    unittest.main()
