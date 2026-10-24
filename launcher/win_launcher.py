# SPDX-License-Identifier: GPL-2.0-or-later
"""The Windows launcher for mbport (MAXIBOOST ON).

The Linux launcher (launcher/bbport_launcher.py) is GTK4's libadwaita; that stack is not
available on Windows, so this is a separate program with the same behaviour: the same
settings file (settings.json), the same environment for the runtime (game_environment) and
the same translations (launcher/bbport_i18n.py, the Russian text is the key).

    python launcher/win_launcher.py

The settings, the cheat table and the patch table live where the Linux launcher puts them:
%LOCALAPPDATA%/mbport-launcher (settings.json) and the data directory (cheats.json,
patches.json).
"""
import json
import os
import subprocess
import sys
import threading
from pathlib import Path

try:
    from PySide6.QtCore import Qt, QProcess, QTimer, Signal
    from PySide6.QtGui import QAction, QKeySequence
    from PySide6.QtWidgets import (
        QApplication,
        QCheckBox,
        QComboBox,
        QFileDialog,
        QGridLayout,
        QGroupBox,
        QHBoxLayout,
        QLabel,
        QLineEdit,
        QListWidget,
        QListWidgetItem,
        QMainWindow,
        QMessageBox,
        QPushButton,
        QSpinBox,
        QSplitter,
        QTabWidget,
        QTextEdit,
        QVBoxLayout,
        QWidget,
    )
except ImportError as error:
    # The launcher needs PySide6; without it there is no window, only this message.
    print("The launcher needs PySide6, which is not installed.\n"
          "\n"
          "Install it with:\n"
          "    python -m pip install PySide6\n"
          "\n"
          f"(the error was: {error})", file=sys.stderr)
    sys.exit(1)

# The program's own directory. In the packaged program (a single executable) it is the
# folder the executable was put in, not the folder it was unpacked into: the runtime and
# the game's data are beside the player, not inside the executable.
if getattr(sys, "frozen", False):
    PORT_DIR = Path(sys.executable).resolve().parent.parent
    # The modules and the data are inside the executable's own folder.
    unpacked = getattr(sys, "_MEIPASS", None)
    if unpacked:
        for folder in ("launcher", "scripts"):
            path = str(Path(unpacked) / folder)
            if path not in sys.path:
                sys.path.insert(0, path)
else:
    PORT_DIR = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PORT_DIR / "launcher"))
sys.path.insert(0, str(PORT_DIR / "scripts"))

from bbport_i18n import language, set_language, system_game_language, tr  # noqa: E402

import game_check  # noqa: E402
import cheats  # noqa: E402
import port_prepare  # noqa: E402
from patches import APP_VERSION as PATCH_APP_VERSION, BUILTIN_XML, FPS_PRESETS  # noqa: E402
from patches import EFFECTS as PATCH_EFFECTS, external_patches  # noqa: E402

# The settings live in the user's own directory (the trophy key is in there).
CONFIG_DIR = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData/Local")) / "mbport-launcher"
CONFIG_FILE = CONFIG_DIR / "settings.json"
# The generated files (the cheat table, the patch table) go with the program.
DATA_DIR = Path(os.environ.get("BB_DATA_DIR", PORT_DIR))
CHEATS_CONFIG = DATA_DIR / "cheats.json"
PATCHES_CONFIG = DATA_DIR / "patches.json"

# The language of the launcher: the first entry is the system's own, the others are named
# in the language they are (a person who does not read the rest looks for their own).
# The languages the launcher has. Each is named in its own language, so that the person
# who reads it finds their own (the "system" entry is translated, because it is the
# launcher's own wording).
def choice_label(label, value):
    """The name of a choice in a list.

    The "system" entry is the launcher's own wording, so it is translated; the languages
    keep their own names, so that the person who reads it finds their own."""
    if value == "":
        return tr("Как в системе")
    if value == "ru" and language() != "ru":
        return "Russian"
    return label


UI_LANGUAGES = [("Как в системе", ""), ("Русский", "ru"), ("English", "en"), ("Português (Brasil)", "pt_BR"),
                ("Español", "es"), ("Italiano", "it"), ("Français", "fr"), ("Deutsch", "de"),
                ("简体中文", "zh_CN"), ("繁體中文", "zh_TW")]
# The game's own language (the two the game has).
GAME_LANGUAGES = [("Как в системе", ""), ("English", "1"), ("日本語", "2")]
FPS_MODES = [("Как в системе", ""), ("30", "30"), ("60", "60"), ("120", "120")]

DEFAULTS = {
    "ui_language": "",
    "game_dir": "",
    "user_dir": "",
    "dlc_dir": "",
    "patches_dir": "",
    "cheats_file": "",
    "language": "",
    "fullscreen": False,
    "hdr": False,
    "present_mode": "Mailbox",
    "gamepad": "",
    "display": "",
    "fps_mode": "60",
    "fps_limit": 0,
    "draw_pipe": "",
    "readbacks": "",
    "preupload": "",
    "mangohud": False,
    "frame_stats": False,
    "save_log": False,
    "crash_diag": False,
    "memory_model": "auto",
    "gpu_profile": False,
    "vk_validation": False,
    "player_name": "Player1",
    "lan": "off",
    "trophy_key": "",
}


def load_settings():
    settings = dict(DEFAULTS)
    try:
        settings.update(json.loads(CONFIG_FILE.read_text(encoding="utf-8")))
    except (OSError, ValueError):
        pass
    return settings


def save_settings(settings):
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    CONFIG_FILE.write_text(json.dumps(settings, indent=2, ensure_ascii=False), encoding="utf-8")


def default_game_dir():
    """The dump beside the program (the README's CUSA18758-full) or where the run script looks."""
    for folder in (PORT_DIR / "CUSA18758-full", PORT_DIR.parent / "CUSA18758-full",
                   PORT_DIR.parent / "CUSA15006-full", PORT_DIR.parent / "CUSA15006"):
        if (folder / "eboot.bin").is_file():
            return folder
    return PORT_DIR / "CUSA18758-full"


def patches_dir(settings):
    return Path(settings.get("patches_dir") or DATA_DIR / "patches").expanduser()


def builtin_patches():
    """The patch names the built-in patch file has for the supported version."""
    try:
        import xml.etree.ElementTree as ET
        root = ET.parse(BUILTIN_XML).getroot()
    except (OSError, Exception):
        return set()
    return {meta.get("Name") for meta in root.iter("Metadata") if meta.get("AppVer") == PATCH_APP_VERSION}


def patch_available(key, have=None):
    have = builtin_patches() if have is None else have
    return all(name in have for name in PATCH_EFFECTS.get(key, ()) if name)


def fps_modes(have=None):
    have = builtin_patches() if have is None else have
    return [mode for mode in FPS_MODES if all(name in have for name in FPS_PRESETS.get(mode[1], ()))]


def game_environment(s):
    """The environment for the runtime: the same variables the Linux launcher sets."""
    env = dict(os.environ)
    env["BB_GAME_DIR"] = str(Path(s["game_dir"]).expanduser())
    if s["user_dir"]:
        env["BB_USER_DIR"] = s["user_dir"]
    if s.get("dlc_dir"):
        env["BB_DLC_DIR"] = str(Path(s["dlc_dir"]).expanduser())
    env["BB_MODS_ENABLED"] = "0"
    env["BB_PATCHES_DIR"] = str(patches_dir(s))
    env["BB_PATCHES_CONFIG"] = str(PATCHES_CONFIG)
    if s.get("cheats_file"):
        env["BB_CHEATS"] = str(Path(s["cheats_file"]).expanduser())
        env["BB_CHEATS_CONFIG"] = str(CHEATS_CONFIG)
    env["BB_LANGUAGE"] = s["language"] or str(system_game_language())
    env["BB_MENU_LANGUAGE"] = language()
    env["BB_FULLSCREEN"] = "1" if s["fullscreen"] else "0"
    env["BB_PRESENT_MODE"] = s["present_mode"]
    if s.get("gamepad"):
        env["BB_GAMEPAD"] = s["gamepad"]
    if s.get("display"):
        env["BB_DISPLAY"] = s["display"]
    if s["hdr"]:
        env["BB_HDR"] = "1"
    env["BB_FPS"] = s["fps_mode"] if s["fps_mode"] in [value for _, value in fps_modes()] else "60"
    if s["fps_limit"] > 0:
        env["BB_FPS_LIMIT"] = str(s["fps_limit"])
    if s["draw_pipe"]:
        env["BB_DRAW_PIPE"] = s["draw_pipe"]
    if s["readbacks"]:
        env["BB_READBACKS"] = s["readbacks"]
    if s.get("preupload"):
        env["BB_PREUPLOAD"] = s["preupload"]
    if s["mangohud"]:
        env["MANGOHUD"] = "1"
    if s["frame_stats"]:
        env["BB_FRAME_STATS"] = "1"
    if s.get("save_log"):
        env["BB_SAVE_LOG"] = "1"
    model = s.get("memory_model", "auto")
    if model in ("new", "old"):
        env["BB_PC_MODEL"] = "1" if model == "new" else "0"
    if s.get("crash_diag"):
        env["BB_FREE_CHECK"] = "1"
        env["BB_WRITE_LOG"] = "1"
        env["BB_PRODUCER_CHECK"] = "1"
    if s["gpu_profile"]:
        env["BB_GPU_PROFILE"] = "1"
    if s["vk_validation"]:
        env["BB_VK_VALIDATION"] = "1"
    if s.get("player_name"):
        env["BB_PLAYER_NAME"] = s["player_name"]
    return env


class Row(QWidget):
    """A labelled row: the label on the left, the control on the right."""

    def __init__(self, title, control, subtitle=None, parent=None):
        super().__init__(parent)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 2, 0, 2)
        text = QVBoxLayout()
        label = QLabel(title)
        label.setWordWrap(True)
        text.addWidget(label)
        if subtitle:
            note = QLabel(subtitle)
            note.setWordWrap(True)
            note.setStyleSheet("color: palette(mid);")
            text.addWidget(note)
        layout.addLayout(text, 1)
        layout.addWidget(control, 0)
        self.control = control


def folder_button(callback):
    button = QPushButton("…")
    button.setFixedWidth(34)
    button.clicked.connect(callback)
    return button


class PrepareWorker(threading.Thread):
    """Prepares the game in the background, so that the window stays alive.

    The eboot is decrypted and the modules are linked: that takes a while, and the window
    would freeze if it were done in the thread of the button."""
    def __init__(self, window):
        super().__init__(daemon=True)
        self.window = window
        self.result = Signal(list)
        self.failed = Signal(str)

    def run(self):
        try:
            prepared = port_prepare.prepare_game(DATA_DIR, self.window.config_path())
        except port_prepare.PrepareError as error:
            self.failed.emit(str(error))
            return
        except Exception as error:  # something else went wrong
            self.failed.emit(f"{error}")
            return
        self.result.emit(port_prepare.runtime_arguments(prepared))


class Window(QMainWindow):
    def __init__(self):
        super().__init__()
        self.settings = load_settings()
        if not self.settings.get("game_dir"):
            self.settings["game_dir"] = str(default_game_dir())
        self.process = None
        self.setWindowTitle("MAXIBOOST ON (mbport)")
        self.resize(980, 720)

        self.tabs = QTabWidget()
        self.setCentralWidget(self.tabs)
        self.build_game_tab()
        self.build_cheats_tab()
        self.build_settings_tab()
        self.build_log_tab()

        self.play = QPushButton(tr("Запустить"))
        self.play.setDefault(True)
        self.play.clicked.connect(self.on_play)
        self.stop = QPushButton(tr("Остановить"))
        self.stop.setEnabled(False)
        self.stop.clicked.connect(self.on_stop)
        bar = QHBoxLayout()
        bar.addStretch(1)
        bar.addWidget(self.stop)
        bar.addWidget(self.play)
        holder = QWidget()
        holder.setLayout(bar)
        self.statusBar().addPermanentWidget(holder)

        quit_action = QAction(tr("Выход"), self)
        quit_action.setShortcut(QKeySequence.Quit)
        quit_action.triggered.connect(self.close)
        self.addAction(quit_action)

        self.apply_language()
        self.refresh_cheats()
        self.refresh_patches()
        self.update_game_status()

    # --- the game page -------------------------------------------------------------------
    def build_game_tab(self):
        page = QWidget()
        layout = QVBoxLayout(page)

        group = QGroupBox(tr("Игра"))
        grid = QGridLayout(group)
        self.game_dir = QLineEdit(self.settings["game_dir"])
        self.game_dir.setReadOnly(True)
        pick = QPushButton(tr("Выбрать папку игры"))
        pick.clicked.connect(self.on_choose_game)
        row = QWidget()
        row_layout = QHBoxLayout(row)
        row_layout.setContentsMargins(0, 0, 0, 0)
        row_layout.addWidget(self.game_dir, 1)
        row_layout.addWidget(pick)
        grid.addWidget(QLabel(tr("Папка игры")), 0, 0)
        grid.addWidget(row, 0, 1)

        self.dlc_dir = QLineEdit(self.settings.get("dlc_dir", ""))
        self.dlc_dir.setReadOnly(True)
        pick_dlc = QPushButton(tr("Выбрать папку DLC"))
        pick_dlc.clicked.connect(self.on_choose_dlc)
        row_dlc = QWidget()
        row_dlc_layout = QHBoxLayout(row_dlc)
        row_dlc_layout.setContentsMargins(0, 0, 0, 0)
        row_dlc_layout.addWidget(self.dlc_dir, 1)
        row_dlc_layout.addWidget(pick_dlc)
        grid.addWidget(QLabel(tr("Папка DLC")), 1, 0)
        grid.addWidget(row_dlc, 1, 1)

        self.user_dir = QLineEdit(self.settings.get("user_dir", ""))
        self.user_dir.setReadOnly(True)
        pick_user = QPushButton(tr("Выбрать папку сохранений"))
        pick_user.clicked.connect(self.on_choose_user)
        row_user = QWidget()
        row_user_layout = QHBoxLayout(row_user)
        row_user_layout.setContentsMargins(0, 0, 0, 0)
        row_user_layout.addWidget(self.user_dir, 1)
        row_user_layout.addWidget(pick_user)
        grid.addWidget(QLabel(tr("Папка сохранений")), 2, 0)
        grid.addWidget(row_user, 2, 1)
        grid.setColumnStretch(1, 1)
        layout.addWidget(group)

        self.game_status = QLabel()
        self.game_status.setWordWrap(True)
        layout.addWidget(self.game_status)
        layout.addStretch(1)
        self.tabs.addTab(page, tr("Игра"))

    # --- the cheats page -----------------------------------------------------------------
    def build_cheats_tab(self):
        page = QWidget()
        layout = QVBoxLayout(page)

        group = QGroupBox(tr("Читы"))
        group_layout = QGridLayout(group)
        self.cheats_file = QLineEdit(self.settings.get("cheats_file", ""))
        self.cheats_file.setReadOnly(True)
        pick = QPushButton(tr("Выбрать файл читов"))
        pick.clicked.connect(self.on_choose_cheats)
        row = QWidget()
        row_layout = QHBoxLayout(row)
        row_layout.setContentsMargins(0, 0, 0, 0)
        row_layout.addWidget(self.cheats_file, 1)
        row_layout.addWidget(pick)
        group_layout.addWidget(QLabel(tr("Файл читов")), 0, 0)
        group_layout.addWidget(row, 0, 1)
        group_layout.setColumnStretch(1, 1)
        layout.addWidget(group)

        self.cheat_list = QListWidget()
        self.cheat_list.itemChanged.connect(self.on_cheat_toggled)
        layout.addWidget(QLabel(tr("Включённые читы")))
        layout.addWidget(self.cheat_list, 1)
        # The two shortcuts: everything on, everything off.
        shortcuts = QHBoxLayout()
        for label, state in ((tr("Включить все"), Qt.Checked), (tr("Выключить все"), Qt.Unchecked)):
            button = QPushButton(label)
            button.clicked.connect(lambda _b, s=state: self.set_all_cheats(s))
            shortcuts.addWidget(button)
        shortcuts.addStretch(1)
        layout.addLayout(shortcuts)
        self.tabs.addTab(page, tr("Читы"))

    def set_all_cheats(self, state):
        """Switches every cheat on or off."""
        self.cheat_list.blockSignals(True)
        for index in range(self.cheat_list.count()):
            item = self.cheat_list.item(index)
            if item.flags() & Qt.ItemIsUserCheckable:
                item.setCheckState(state)
        self.cheat_list.blockSignals(False)
        self.write_cheats()


    def build_settings_tab(self):
        page = QWidget()
        layout = QVBoxLayout(page)
        form = QGridLayout()
        row = 0

        self.ui_language = QComboBox()
        for label, value in UI_LANGUAGES:
            # The languages are named in their own language; the "system" entry is the
            # launcher's own wording, so it follows the chosen language.
            self.ui_language.addItem(choice_label(label, value), value)
        self.select(self.ui_language, self.settings.get("ui_language", ""))
        self.ui_language.currentIndexChanged.connect(self.on_ui_language)
        form.addWidget(Row(tr("Язык интерфейса"), self.ui_language), row, 0)
        row += 1

        self.game_language = QComboBox()
        for label, value in GAME_LANGUAGES:
            self.game_language.addItem(choice_label(label, value), value)
        self.select(self.game_language, self.settings.get("language", ""))
        self.game_language.currentIndexChanged.connect(self.on_game_language)
        form.addWidget(Row(tr("Язык игры"), self.game_language), row, 0)
        row += 1

        self.fps = QComboBox()
        for label, value in fps_modes():
            self.fps.addItem(choice_label(label, value), value)
        self.select(self.fps, self.settings.get("fps_mode", "60"))
        self.fps.currentIndexChanged.connect(self.on_fps)
        form.addWidget(Row(tr("Частота кадров"), self.fps), row, 0)
        row += 1

        self.fps_limit = QSpinBox()
        self.fps_limit.setRange(0, 240)
        self.fps_limit.setValue(self.settings.get("fps_limit", 0))
        self.fps_limit.valueChanged.connect(self.on_fps_limit)
        form.addWidget(Row(tr("Ограничение кадров"), self.fps_limit), row, 0)
        row += 1

        self.present_mode = QComboBox()
        for mode in ("Mailbox", "FIFO", "Immediate", "VR"):
            self.present_mode.addItem(mode, mode)
        self.select(self.present_mode, self.settings.get("present_mode", "Mailbox"))
        self.present_mode.currentIndexChanged.connect(self.on_present_mode)
        form.addWidget(Row(tr("Режим презентации"), self.present_mode), row, 0)
        row += 1

        self.memory_model = QComboBox()
        for label, value in ((tr("Авто"), "auto"), (tr("Новая"), "new"), (tr("Старая"), "old")):
            self.memory_model.addItem(label, value)
        self.select(self.memory_model, self.settings.get("memory_model", "auto"))
        self.memory_model.currentIndexChanged.connect(self.on_memory_model)
        form.addWidget(Row(tr("Модель памяти"), self.memory_model), row, 0)
        row += 1

        self.player_name = QLineEdit(self.settings.get("player_name", "Player1"))
        self.player_name.editingFinished.connect(self.on_player_name)
        form.addWidget(Row(tr("Имя игрока"), self.player_name), row, 0)
        row += 1

        self.fullscreen = QCheckBox(tr("Полноэкранный режим"))
        self.fullscreen.setChecked(bool(self.settings.get("fullscreen")))
        self.fullscreen.stateChanged.connect(self.on_fullscreen)
        form.addWidget(Row("", self.fullscreen), row, 0)
        row += 1

        self.hdr = QCheckBox(tr("HDR"))
        self.hdr.setChecked(bool(self.settings.get("hdr")))
        self.hdr.stateChanged.connect(self.on_hdr)
        form.addWidget(Row("", self.hdr), row, 0)
        row += 1

        self.frame_stats = QCheckBox(tr("Статистика кадров"))
        self.frame_stats.setChecked(bool(self.settings.get("frame_stats")))
        self.frame_stats.stateChanged.connect(self.on_frame_stats)
        form.addWidget(Row("", self.frame_stats), row, 0)
        row += 1

        self.crash_diag = QCheckBox(tr("Диагностика падений"))
        self.crash_diag.setChecked(bool(self.settings.get("crash_diag")))
        self.crash_diag.stateChanged.connect(self.on_crash_diag)
        form.addWidget(Row("", self.crash_diag), row, 0)
        row += 1

        self.vk_validation = QCheckBox(tr("Проверка Vulkan"))
        self.vk_validation.setChecked(bool(self.settings.get("vk_validation")))
        self.vk_validation.stateChanged.connect(self.on_vk_validation)
        form.addWidget(Row("", self.vk_validation), row, 0)
        row += 1

        layout.addLayout(form)
        layout.addStretch(1)
        self.tabs.addTab(page, tr("Настройки"))

    # --- the log page --------------------------------------------------------------------
    def build_log_tab(self):
        page = QWidget()
        layout = QVBoxLayout(page)
        self.log = QTextEdit()
        self.log.setReadOnly(True)
        self.log.setLineWrapMode(QTextEdit.NoWrap)
        layout.addWidget(self.log, 1)
        buttons = QHBoxLayout()
        clear = QPushButton(tr("Очистить журнал"))
        clear.clicked.connect(self.log.clear)
        buttons.addWidget(clear)
        buttons.addStretch(1)
        layout.addLayout(buttons)
        self.tabs.addTab(page, tr("Журнал"))

    # --- helpers -------------------------------------------------------------------------
    def select(self, box, value):
        index = box.findData(value)
        box.setCurrentIndex(index if index >= 0 else 0)

    def apply_language(self):
        set_language(self.settings.get("ui_language", ""))
        self.setWindowTitle("MAXIBOOST ON (mbport)")
        self.tabs.setTabText(0, tr("Игра"))
        self.tabs.setTabText(1, tr("Читы"))
        self.tabs.setTabText(2, tr("Настройки"))
        self.tabs.setTabText(3, tr("Журнал"))
        self.play.setText(tr("Запустить"))
        self.stop.setText(tr("Остановить"))

    def update_game_status(self):
        folder = self.settings.get("game_dir", "")
        if not folder or not (Path(folder) / "eboot.bin").is_file():
            self.game_status.setText(tr("Игра не найдена: выберите папку с eboot.bin"))
            return
        found = game_check.problem(folder)
        if found:
            kind, title, version = found
            self.game_status.setText(game_check.explain(kind, title, version))
        else:
            self.game_status.setText(tr("MAXIBOOST ON 1.15: поддерживается"))

    # --- the game page's actions ---------------------------------------------------------
    def on_choose_game(self):
        folder = QFileDialog.getExistingDirectory(self, tr("Папка игры"), self.settings.get("game_dir", ""))
        if folder:
            self.settings["game_dir"] = folder
            self.game_dir.setText(folder)
            self.store()
            self.update_game_status()

    def on_choose_dlc(self):
        folder = QFileDialog.getExistingDirectory(self, tr("Папка DLC"), self.settings.get("dlc_dir", ""))
        if folder:
            self.settings["dlc_dir"] = folder
            self.dlc_dir.setText(folder)
            self.store()

    def on_choose_user(self):
        folder = QFileDialog.getExistingDirectory(self, tr("Папка сохранений"), self.settings.get("user_dir", ""))
        if folder:
            self.settings["user_dir"] = folder
            self.user_dir.setText(folder)
            self.store()

    # --- the cheats ----------------------------------------------------------------------
    def on_choose_cheats(self):
        path, _ = QFileDialog.getOpenFileName(self, tr("Файл читов"), "",
                                              tr("Файлы читов (*.json);;Все файлы (*)"))
        if not path:
            return
        self.settings["cheats_file"] = path
        self.cheats_file.setText(path)
        self.store()
        self.refresh_cheats()

    def refresh_cheats(self):
        """The cheats of the chosen file, one switch each."""
        self.cheat_list.blockSignals(True)
        self.cheat_list.clear()
        path = self.settings.get("cheats_file") or ""
        if not path:
            self.cheat_list.addItem(tr("Файл читов не выбран"))
            self.cheat_list.item(0).setFlags(Qt.ItemIsEnabled)
            self.cheat_list.blockSignals(False)
            return
        try:
            data = cheats.load(path)
        except cheats.CheatError as error:
            item = QListWidgetItem(f"{path} — {error}")
            item.setFlags(Qt.ItemIsEnabled)
            self.cheat_list.addItem(item)
            self.cheat_list.blockSignals(False)
            return
        names = [name for name, _ in cheats.entries(data)]
        enabled = self.cheat_enabled()
        for name in names:
            item = QListWidgetItem(name)
            item.setFlags(item.flags() | Qt.ItemIsUserCheckable)
            item.setCheckState(Qt.Checked if name in enabled else Qt.Unchecked)
            self.cheat_list.addItem(item)
        self.cheat_list.blockSignals(False)

    def cheat_enabled(self):
        """cheats.json: the cheats that are switched on."""
        try:
            data = json.loads(CHEATS_CONFIG.read_text(encoding="utf-8"))
            return set(data.get("enabled", []))
        except (OSError, ValueError):
            return set()

    def on_cheat_toggled(self, item):
        if not (item.flags() & Qt.ItemIsUserCheckable):
            return
        self.write_cheats()

    def write_cheats(self):
        """The cheats that are switched on (cheats.json); the file's order is kept."""
        enabled = set()
        for index in range(self.cheat_list.count()):
            entry = self.cheat_list.item(index)
            if not (entry.flags() & Qt.ItemIsUserCheckable):
                continue
            if entry.checkState() == Qt.Checked:
                enabled.add(entry.text())
        # The order of the file is kept: the names are written the way the table has them.
        try:
            data = cheats.load(self.settings["cheats_file"])
            names = [name for name, _ in cheats.entries(data)]
        except (cheats.CheatError, KeyError):
            names = sorted(enabled)
        ordered = [name for name in names if name in enabled]
        ordered += sorted(name for name in enabled if name not in names)
        CHEATS_CONFIG.parent.mkdir(parents=True, exist_ok=True)
        CHEATS_CONFIG.write_text(json.dumps({"enabled": ordered}, indent=2, ensure_ascii=False),
                                 encoding="utf-8")

    def refresh_patches(self):
        """The third-party patches in the patch folder (kept for the settings file's sake)."""
        folder = patches_dir(self.settings)
        if not folder.is_dir():
            return
        try:
            found = external_patches(folder, PATCH_APP_VERSION, None)
        except Exception:
            return
        for _ in found:
            break

    # --- the settings page's actions ------------------------------------------------------
    def on_ui_language(self):
        self.settings["ui_language"] = self.ui_language.currentData()
        self.store()
        self.apply_language()

    def on_game_language(self):
        self.settings["language"] = self.game_language.currentData()
        self.store()

    def on_fps(self):
        self.settings["fps_mode"] = self.fps.currentData()
        self.store()

    def on_fps_limit(self, value):
        self.settings["fps_limit"] = value
        self.store()

    def on_present_mode(self):
        self.settings["present_mode"] = self.present_mode.currentData()
        self.store()

    def on_memory_model(self):
        self.settings["memory_model"] = self.memory_model.currentData()
        self.store()

    def on_player_name(self):
        self.settings["player_name"] = self.player_name.text()
        self.store()

    def on_fullscreen(self):
        self.settings["fullscreen"] = self.fullscreen.isChecked()
        self.store()

    def on_hdr(self):
        self.settings["hdr"] = self.hdr.isChecked()
        self.store()

    def on_frame_stats(self):
        self.settings["frame_stats"] = self.frame_stats.isChecked()
        self.store()

    def on_crash_diag(self):
        self.settings["crash_diag"] = self.crash_diag.isChecked()
        self.store()

    def on_vk_validation(self):
        self.settings["vk_validation"] = self.vk_validation.isChecked()
        self.store()

    # --- the run -------------------------------------------------------------------------
    def store(self):
        save_settings(self.settings)

    def runtime_path(self):
        """The runtime's program: in the package (bin), or in the build tree."""
        for candidate in (PORT_DIR / "bin" / "bb-probe.exe",
                          PORT_DIR / "bb-probe.exe",
                          PORT_DIR.parent / "bin" / "bb-probe.exe",
                          PORT_DIR / "out" / "win" / "bin" / "bb-probe.exe",
                          PORT_DIR / "out" / "win" / "bb-probe.exe"):
            if candidate.is_file():
                return candidate
        return None

    def runtime_arguments(self):
        """The arguments the runtime is started with.

        The game is prepared first: the eboot is decrypted, the game's modules are linked
        into it, the profile and the patches are written. The steps take a while, so they
        run in a thread and the window stays alive."""
        self.set_busy(True)
        self.log.append(tr("Готовим игру..."))
        worker = PrepareWorker(self)
        worker.result.connect(self.on_prepared)
        worker.failed.connect(self.on_prepare_failed)
        worker.start()
        return None

    def config_path(self):
        """The file the settings are read from (the runtime's own configuration)."""
        return DATA_DIR / "bbport.ini"

    def set_busy(self, busy):
        """The window's buttons while the game is being prepared."""
        self.play.setEnabled(not busy)
        self.stop.setEnabled(not busy)

    def on_prepared(self, arguments):
        """The game is prepared: the runtime can start."""
        self.set_busy(False)
        if not arguments:
            return
        runtime = self.runtime_path()
        if runtime is None:
            QMessageBox.warning(self, tr("Запустить"),
                                tr("Не найден исполняемый файл bb-probe.exe. Соберите проект."))
            return
        env = game_environment(self.settings)
        self.process = QProcess(self)
        self.process.setProcessEnvironment(_qt_environment(env))
        self.process.setWorkingDirectory(str(runtime.parent))
        self.process.readyReadStandardOutput.connect(self.on_output)
        self.process.readyReadStandardError.connect(self.on_output)
        self.process.finished.connect(self.on_finished)
        self.process.start(str(runtime), arguments)
        self.play.setEnabled(False)
        self.stop.setEnabled(True)

    def on_prepare_failed(self, message):
        """The game could not be prepared."""
        self.set_busy(False)
        self.log.append(message)
        QMessageBox.critical(self, tr("Запустить"), message)

    def on_play(self):
        if self.process is not None:
            return
        runtime = self.runtime_path()
        if runtime is None:
            QMessageBox.warning(self, tr("Запустить"),
                                tr("Не найден исполняемый файл bb-probe.exe. Соберите проект."))
            return
        found = game_check.problem(self.settings["game_dir"])
        if found:
            kind, title, version = found
            QMessageBox.warning(self, tr("Запустить"), game_check.explain(kind, title, version))
            return
        self.store()
        self.tabs.setCurrentIndex(3)
        self.log.clear()
        self.runtime_arguments()

    def on_output(self):
        data = bytes(self.process.readAllStandardOutput())
        if not data:
            data = bytes(self.process.readAllStandardError())
        if data:
            self.log.append(data.decode("utf-8", "replace").rstrip("\n"))

    def on_finished(self, code, status):
        self.log.append(tr("Игра завершилась (код {}).").format(code))
        self.process = None
        self.play.setEnabled(True)
        self.stop.setEnabled(False)

    def on_stop(self):
        if self.process is not None:
            self.process.kill()

    def closeEvent(self, event):
        if self.process is not None:
            self.process.kill()
        self.store()
        super().closeEvent(event)


def _qt_environment(env):
    """The environment as QProcess wants it (a list of KEY=VALUE)."""
    from PySide6.QtCore import QProcessEnvironment
    qt_env = QProcessEnvironment()
    for key, value in env.items():
        qt_env.insert(str(key), str(value))
    return qt_env


def main():
    app = QApplication(sys.argv)
    window = Window()
    window.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
