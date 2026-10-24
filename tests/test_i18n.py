from paths import ROOT
import ast
import json
import os
import re
import string
import sys
import unittest
sys.path.insert(0, str(ROOT / 'launcher'))
import bbport_i18n


class MenuTests(unittest.TestCase):
    """The menu bar holds the language, so that it can be changed from any page."""

    def _window(self, language=None):
        os.environ['QT_QPA_PLATFORM'] = 'offscreen'
        try:
            from PySide6.QtWidgets import QApplication
        except ImportError:
            self.skipTest('PySide6 is not installed')
        app = QApplication.instance() or QApplication([])
        sys.path.insert(0, str(ROOT / 'launcher'))
        sys.path.insert(0, str(ROOT / 'scripts'))
        import win_launcher
        if language is not None:
            win_launcher.CONFIG_FILE.parent.mkdir(parents=True, exist_ok=True)
            win_launcher.CONFIG_FILE.write_text(
                json.dumps({'ui_language': language}), encoding='utf-8')
        return win_launcher.Window()

    def test_the_menu_bar_is_in_the_chosen_language(self):
        for language, title in (('en', 'Language'), ('zh_CN', '语言'), ('zh_TW', '語言')):
            with self.subTest(language=language):
                window = self._window(language)
                titles = [action.text() for action in window.menuBar().actions()]
                self.assertEqual(titles, [title])
                for text in titles:
                    self.assertIsNone(CYRILLIC.search(text))

    def test_the_menu_holds_every_language_and_marks_the_chosen_one(self):
        sys.path.insert(0, str(ROOT / 'launcher'))
        import win_launcher
        window = self._window('zh_TW')
        self.assertEqual(len(window.language_menu.actions()), len(win_launcher.UI_LANGUAGES))
        chosen = [action.text() for action in window.language_menu.actions() if action.isChecked()]
        self.assertEqual(chosen, ['繁體中文'])

    def test_changing_the_language_builds_the_window_again(self):
        window = self._window('en')
        window.choose_language('zh_CN')
        tabs = [window.tabs.tabText(index) for index in range(window.tabs.count())]
        self.assertEqual(tabs, ['游戏', '金手指', '设置', '操作设置', '日志'])
        self.assertEqual([action.text() for action in window.menuBar().actions()], ['语言'])


class GameLanguageTests(unittest.TestCase):
    def test_desktop_locale_picks_the_ps4_system_language(self):
        cases = {'es_ES.UTF-8': 3, 'fr_CA.UTF-8': 2, 'ja_JP.UTF-8': 0, 'ru_RU.UTF-8': 8,
                 'de_DE.UTF-8': 4, 'it_IT.UTF-8': 5, 'en_GB.UTF-8': 1, 'nl_NL.UTF-8': 1, '': 1}
        for locale, code in cases.items():
            with self.subTest(locale=locale):
                self.assertEqual(bbport_i18n.system_game_language({'LANG': locale}, prefer_env=True), code)

    def test_lc_all_overrides_lang(self):
        self.assertEqual(bbport_i18n.system_game_language({'LC_ALL': 'de_DE', 'LANG': 'es_ES'}, prefer_env=True), 4)


# The languages this fork keeps complete.
MAINTAINED = ('EN', 'ES', 'IT', 'FR', 'DE', 'PT_BR', 'ZH_CN', 'ZH_TW')
CYRILLIC = re.compile('[А-Яа-яЁё]')


def launcher_texts():
    """The Russian texts the launcher translates: tr()'s literal arguments, and the choice
    lists' labels (tuples of module-level constants, passed to tr() later)."""
    texts = set()
    for path in sorted((ROOT / 'launcher').glob('*.py')):
        if path.name == 'bbport_i18n.py':
            continue
        tree = ast.parse(path.read_text(encoding='utf-8'))
        for node in ast.walk(tree):
            if (isinstance(node, ast.Call) and getattr(node.func, 'id', None) == 'tr' and node.args
                    and isinstance(node.args[0], ast.Constant) and isinstance(node.args[0].value, str)):
                texts.add(node.args[0].value)
        for node in tree.body:
            if isinstance(node, ast.Assign) and all(getattr(t, 'id', '').isupper() for t in node.targets):
                for item in ast.walk(node.value):
                    if (isinstance(item, ast.Tuple) and item.elts and isinstance(item.elts[0], ast.Constant)
                            and isinstance(item.elts[0].value, str)):
                        texts.add(item.elts[0].value)
    return {text for text in texts if CYRILLIC.search(text)}


def fields(text):
    return sorted(str(name or index) for index, (_lit, name, _spec, _conv)
                  in enumerate(string.Formatter().parse(text)) if name is not None)


class TranslationTests(unittest.TestCase):
    def test_every_launcher_text_is_translated(self):
        texts = launcher_texts()
        self.assertGreater(len(texts), 200)
        for language in MAINTAINED:
            table = getattr(bbport_i18n, language)
            with self.subTest(language=language):
                self.assertEqual(sorted(t for t in texts if t not in table), [])

    def test_translations_keep_the_placeholders(self):
        for language in MAINTAINED:
            for key, text in getattr(bbport_i18n, language).items():
                with self.subTest(language=language, key=key[:40]):
                    self.assertEqual(len(fields(text)), len(fields(key)))


class MenuTests(unittest.TestCase):
    """The menu bar holds the language, so that it can be changed from any page."""

    def _window(self, language=None):
        os.environ['QT_QPA_PLATFORM'] = 'offscreen'
        try:
            from PySide6.QtWidgets import QApplication
        except ImportError:
            self.skipTest('PySide6 is not installed')
        app = QApplication.instance() or QApplication([])
        sys.path.insert(0, str(ROOT / 'launcher'))
        sys.path.insert(0, str(ROOT / 'scripts'))
        import win_launcher
        if language is not None:
            win_launcher.CONFIG_FILE.parent.mkdir(parents=True, exist_ok=True)
            win_launcher.CONFIG_FILE.write_text(
                json.dumps({'ui_language': language}), encoding='utf-8')
        return win_launcher.Window()

    def test_the_menu_bar_is_in_the_chosen_language(self):
        for language, title in (('en', 'Language'), ('zh_CN', '语言'), ('zh_TW', '語言')):
            with self.subTest(language=language):
                window = self._window(language)
                titles = [action.text() for action in window.menuBar().actions()]
                self.assertEqual(titles, [title])
                for text in titles:
                    self.assertIsNone(CYRILLIC.search(text))

    def test_the_menu_holds_every_language_and_marks_the_chosen_one(self):
        sys.path.insert(0, str(ROOT / 'launcher'))
        import win_launcher
        window = self._window('zh_TW')
        self.assertEqual(len(window.language_menu.actions()), len(win_launcher.UI_LANGUAGES))
        chosen = [action.text() for action in window.language_menu.actions() if action.isChecked()]
        self.assertEqual(chosen, ['繁體中文'])

    def test_changing_the_language_builds_the_window_again(self):
        window = self._window('en')
        window.choose_language('zh_CN')
        tabs = [window.tabs.tabText(index) for index in range(window.tabs.count())]
        self.assertEqual(tabs, ['游戏', '金手指', '设置', '操作设置', '日志'])
        self.assertEqual([action.text() for action in window.menuBar().actions()], ['语言'])


class GameLanguageTests(unittest.TestCase):
    """The game's own language: the release decides which languages it has."""

    def test_the_release_decides_the_languages(self):
        # The North American release is in English; the Hong Kong one has the traditional
        # Chinese and the Korean.
        self.assertEqual(bbport_i18n.RELEASE_LANGUAGES['CUSA18758'], (1,))
        self.assertEqual(bbport_i18n.RELEASE_LANGUAGES['CUSA15006'], (10, 11))

    def test_the_system_language_is_used_when_the_game_has_it(self):
        for title, wanted, expected in (('CUSA18758', 1, 1), ('CUSA15006', 11, 11),
                                        ('CUSA15006', 10, 10)):
            with self.subTest(title=title, wanted=wanted):
                self.assertEqual(bbport_i18n.game_language('', title, wanted), expected)

    def test_a_language_the_game_does_not_have_is_not_used(self):
        # The North American release is in English, whatever the system speaks.
        for wanted in (0, 10, 11, 2):
            with self.subTest(wanted=wanted):
                self.assertEqual(bbport_i18n.game_language('', 'CUSA18758', wanted), 1)
        # The Hong Kong one: a Chinese system gets the traditional Chinese, an English one
        # the traditional Chinese as well (the game has no English).
        self.assertEqual(bbport_i18n.game_language('', 'CUSA15006', 1), 10)
        self.assertEqual(bbport_i18n.game_language('', 'CUSA15006', 9), 10)

    def test_the_chosen_language_is_used_as_it_is(self):
        self.assertEqual(bbport_i18n.game_language('11', 'CUSA18758', 1), 11)
        self.assertEqual(bbport_i18n.game_language('1', 'CUSA15006', 10), 1)

    def test_an_unknown_release_keeps_the_system_language(self):
        self.assertEqual(bbport_i18n.game_language('', '', 10), 10)
        self.assertEqual(bbport_i18n.game_language('', 'CUSA99999', 11), 11)


class FallbackTests(unittest.TestCase):
    """A text a language does not have must not show up as the Russian key: the window
    would look like a mixture of two languages."""

    def test_a_missing_text_falls_back_to_english(self):
        original = bbport_i18n._language
        try:
            for language in bbport_i18n.LANGUAGES:
                if language == 'ru':
                    continue  # the Russian key is the Russian text
                bbport_i18n.set_language(language)
                for key in launcher_texts():
                    shown = bbport_i18n.tr(key)
                    with self.subTest(language=language, key=key[:40]):
                        # The Russian key must not show up: the window would look like a
                        # mixture of two languages.
                        self.assertNotEqual(shown, key)
                        self.assertIsNone(CYRILLIC.search(shown))
        finally:
            bbport_i18n._language = original

    def test_the_traditional_chinese_table_differs_from_the_simplified_one(self):
        self.assertGreater(len(bbport_i18n.ZH_TW), 500)
        different = [key for key, text in bbport_i18n.ZH_TW.items()
                     if bbport_i18n.ZH_CN.get(key) != text]
        self.assertGreater(len(different), 100)


class WindowTests(unittest.TestCase):
    """The window itself must not show the Russian text in another language."""

    def test_the_window_shows_no_russian_in_the_other_languages(self):
        os.environ['QT_QPA_PLATFORM'] = 'offscreen'
        try:
            from PySide6.QtWidgets import QApplication, QComboBox, QListWidget
        except ImportError:
            self.skipTest('PySide6 is not installed')
        app = QApplication.instance() or QApplication([])
        sys.path.insert(0, str(ROOT / 'launcher'))
        sys.path.insert(0, str(ROOT / 'scripts'))
        import win_launcher

        def texts(widget, found):
            for child in widget.children():
                for getter in ('text', 'title', 'toolTip', 'statusTip', 'placeholderText'):
                    if hasattr(child, getter):
                        try:
                            value = getattr(child, getter)()
                        except Exception:
                            value = None
                        if isinstance(value, str) and value.strip():
                            found.append(value)
                if isinstance(child, QComboBox):
                    for index in range(child.count()):
                        found.append(child.itemText(index))
                if isinstance(child, QListWidget):
                    for index in range(child.count()):
                        found.append(child.item(index).text())
                texts(child, found)
            return found

        original = bbport_i18n._language
        try:
            for language in ('en', 'zh_CN', 'zh_TW'):
                bbport_i18n.set_language(language)
                window = win_launcher.Window()
                shown = texts(window, [])
                russian = [text for text in shown if CYRILLIC.search(text)]
                with self.subTest(language=language):
                    self.assertEqual(russian, [])
        finally:
            bbport_i18n._language = original


if __name__ == '__main__':
    unittest.main()
