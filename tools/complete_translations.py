# SPDX-License-Identifier: GPL-2.0-or-later
"""Completes the launcher's translation tables.

Every language must have every text the launcher shows. A text that is missing from a
table shows up in that language as the Russian key (the key is the Russian text), so the
window looks like a mixture of two languages.

The script finds the gaps and fills them:
- the English table from the wording of the other languages (or the English one in
  ENGLISH below);
- the other tables from the English one, through a conversion when the language is
  Chinese.

The traditional Chinese table is made from the simplified one (OpenCC's s2twp, which
follows the wording Taiwan uses), so the two stay in step.

The tables are edited in place: the entries that are already there keep their comments and
their place; only the missing ones are added, at the end of their table.

    python tools/complete_translations.py [--check]

With --check it only reports the gaps (what the test does).
"""
import argparse
import ast
import io
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MODULE = ROOT / "launcher" / "bbport_i18n.py"

# The wording of the texts the English table was missing.
ENGLISH = {
    "English": "English",
    "Язык": "Language",
    "HDR": "HDR",
    "Чит": "Cheat",
    "Управление": "Controls",
    "Устройство ввода": "Input device",
    "Обновить список": "Refresh the list",
    "Устройство не выбрано": "No device chosen",
    "Действие": "Action",
    "Клавиша": "Key",
    "Кнопка геймпада": "Gamepad button",
    "Настроить автоматически": "Set automatically",
    "Сбросить": "Reset",
    "XInput": "XInput",
    "DirectInput": "DirectInput",
    "Геймпад через {}: {}": "Gamepad through {}: {}",
    "Клавиатура": "Keyboard",
    "Cross": "Cross",
    "Circle": "Circle",
    "Square": "Square",
    "Triangle": "Triangle",
    "Options": "Options",
    "Touchpad (left half)": "Touchpad (left half)",
    "Touchpad (right half)": "Touchpad (right half)",
    "D-pad up": "D-pad up",
    "D-pad down": "D-pad down",
    "D-pad left": "D-pad left",
    "D-pad right": "D-pad right",
    "Move forward": "Move forward",
    "Move back": "Move back",
    "Move left": "Move left",
    "Move right": "Move right",
    "Camera up": "Camera up",
    "Camera down": "Camera down",
    "Camera left": "Camera left",
    "Camera right": "Camera right",
    "Разрешение вывода": "Output resolution",
    "Память игры": "Game memory",
    "Язык игры": "Game language",
    "Китайский (традиционный)": "Chinese (traditional)",
    "Корейский": "Korean",
    "Авто": "Automatic",
    "Новая": "New",
    "Старая": "Old",
    "Файлы читов (*.json);;Все файлы (*)": "Cheat files (*.json);;All files (*)",
    "Камера не поворачивается сама при движении; ручное управление и захват":
        "The camera does not turn by itself when you move; manual control and capture",
    "Нужны видеокарта NVIDIA RTX и проприетарный драйвер; иначе используется встроенная":
        "Needs an NVIDIA RTX graphics card and the proprietary driver; otherwise the built-in one is used",
    "Установите DbgFont14h.ccm и DbgFont14h.tpf в dvdroot_ps4/font из мода Nexus #253":
        "Install DbgFont14h.ccm and DbgFont14h.tpf into dvdroot_ps4/font from the Nexus mod #253",
    "0 — по частоте экрана (не выше 120 Гц); укажите число, чтобы ограничить иначе":
        "0 — by the screen's refresh rate (not above 120 Hz); give a number to limit it otherwise",
    "Balanced (x1.7)": "Balanced (x1.7)",
    "Native AA": "Native AA",
    "Performance (x2)": "Performance (x2)",
    "Quality (x1.5)": "Quality (x1.5)",
    "Relaxed (по умолчанию)": "Relaxed (default)",
    "Ultra Performance (x3)": "Ultra Performance (x3)",
    "Авто (8+ потоков)": "Automatic (8+ threads)",
    "Выключен (стабильнее)": "Off (more stable)",
    "Только для видеокарт AMD, а на этом компьютере её нет. Используется старая модель памяти, как в 0.3, со всеми исправлениями":
        "For AMD graphics cards only, and this computer has none. The old memory model is used, as in 0.3, with all the fixes",
    "Эксперимент, только видеокарты AMD. Видеокарта работает с памятью игры напрямую, как в игре для ПК, а команды графики переводятся, а не эмулируются; возможны ошибки. Выключено — старая модель памяти, как в 0.3, со всеми исправлениями":
        "Experimental, AMD graphics cards only. The card works with the game's memory directly, as in a PC game, and the graphics commands are translated rather than emulated; errors are possible. Off — the old memory model, as in 0.3, with all the fixes",
}

# The wording of the texts the other tables were missing. The key is the language's own
# code; the value is a table of the Russian key to the text in that language.
ADDITIONS = {
    "zh_CN": {
        "Язык": "语言",
        "Управление": "操作设置",
        "Устройство ввода": "输入设备",
        "Обновить список": "刷新列表",
        "Устройство не выбрано": "未选择设备",
        "Действие": "动作",
        "Клавиша": "键位",
        "Кнопка геймпада": "手柄按键",
        "Настроить автоматически": "自动配置",
        "Разрешение вывода": "输出分辨率",
        "Память игры": "游戏内存",
        "Язык игры": "游戏语言",
        "Китайский (традиционный)": "繁體中文",
        "Корейский": "韩语",
        "Читы": "金手指",
        "Чит": "金手指",
    },
    "pt_BR": {
        "Чит": "Trapaça",
        "R3: управлять целью, L3: вернуться. Не вместе со свободной камерой":
            "R3: controlar o alvo, L3: voltar. Não junto com a câmera livre",
        "Rally не угасает": "Rally nunca desaparece",
        "shadnet-p2p: на вашем ПК или VPS": "shadnet-p2p: no seu PC ou VPS",
        "Атаки не возвращают здоровье": "Ataques não recuperam vida",
        "Бег примерно с 70% наклона стика вместо 90%": "Correr com cerca de 70% de inclinação do analógico, em vez de 90%",
        "Бег с меньшим наклоном стика": "Corrida mais fácil",
        "Без Rally": "Sem Rally",
        "Без автоповорота камеры": "Sem rotação automática da câmera",
        "Бессмертие": "Imortalidade",
        "Бесшумность": "Passos silenciosos",
        "Бронзовый": "Bronze",
        "Враги не видят вас и не реагируют, пока вы не атакуете": "Os inimigos não veem nem reagem a você até você atacar",
        "Враги не слышат вас, но видят": "Os inimigos não ouvem você, mas veem",
        "Здоровье для Rally не исчезает со временем": "A vida recuperável com Rally não desaparece com o tempo",
        "Золотой": "Ouro",
        "Камера дальше": "Câmera mais distante",
        "Камера дальше от персонажа": "A câmera fica mais distante do personagem",
        "Камера не поворачивается сама при движении; ручное управление и захват":
            "A câmera não gira sozinha ao se mover; controle manual e captura",
        "Невидимость": "Furtividade",
        "Нет файлов DLSS: tools/build_dlss.sh": "Sem arquivos DLSS: tools/build_dlss.sh",
        "Нужны видеокарта NVIDIA RTX и проприетарный драйвер; иначе используется встроенная":
            "Precisa de uma placa NVIDIA RTX e do driver proprietário; senão usa a integrada",
        "Платиновый": "Platina",
        "Серебряный": "Prata",
        "Тела и предметы разлетаются от ударов": "Corpos e objetos voam quando são atingidos",
        "Управление врагом": "Controle de inimigo",
        "Урон проходит, но здоровье не опускается ниже 1": "Você toma dano, mas nunca fica abaixo de 1 PV",
        "Физика тел как в Dark Souls": "Física de corpos como em Dark Souls",
    },
    "es": {
        "Чит": "Truco",
        "R3: управлять целью, L3: вернуться. Не вместе со свободной камерой":
            "R3: controlar el objetivo, L3: volver. No junto con la cámara libre",
        "Rally не угасает": "Rally nunca se desvanece",
        "shadnet-p2p: на вашем ПК или VPS": "shadnet-p2p: en tu PC o un VPS",
        "Атаки не возвращают здоровье": "Los ataques no recuperan salud",
        "Бег примерно с 70% наклона стика вместо 90%": "Correr con un 70 % de inclinación del stick en vez del 90 %",
        "Бег с меньшим наклоном стика": "Carrera más fácil",
        "Без Rally": "Sin Rally",
        "Без автоповорота камеры": "Sin giro automático de la cámara",
        "Бессмертие": "Inmortalidad",
        "Бесшумность": "Pasos silenciosos",
        "Бронзовый": "Bronce",
        "Враги не видят вас и не реагируют, пока вы не атакуете": "Los enemigos no te ven ni reaccionan hasta que atacas",
        "Враги не слышат вас, но видят": "Los enemigos no te oyen, pero te ven",
        "Здоровье для Rally не исчезает со временем": "La salud que Rally recupera no desaparece con el tiempo",
        "Золотой": "Oro",
        "Камера дальше": "Cámara más lejos",
        "Камера дальше от персонажа": "La cámara se mantiene más lejos del personaje",
        "Камера не поворачивается сама при движении; ручное управление и захват":
            "La cámara no gira sola al moverte; control manual y captura",
        "Невидимость": "Sigilo",
        "Нет файлов DLSS: tools/build_dlss.sh": "No hay archivos DLSS: tools/build_dlss.sh",
        "Нужны видеокарта NVIDIA RTX и проприетарный драйвер; иначе используется встроенная":
            "Necesita una GPU NVIDIA RTX y el driver propietario; si no, usa la integrada",
        "Платиновый": "Platino",
        "Серебряный": "Plata",
        "Тела и предметы разлетаются от ударов": "Los cuerpos y los objetos salen despedidos al golpearlos",
        "Управление врагом": "Control de enemigos",
        "Урон проходит, но здоровье не опускается ниже 1": "Recibes daño, pero nunca bajas de 1 PS",
        "Физика тел как в Dark Souls": "Física de cuerpos como en Dark Souls",
    },
    "it": {
        "Чит": "Truffa",
        "R3: управлять целью, L3: вернуться. Не вместе со свободной камерой":
            "R3: controlla il bersaglio, L3: indietro. Non insieme alla camera libera",
        "Rally не угасает": "Rally non svanisce mai",
        "shadnet-p2p: на вашем ПК или VPS": "shadnet-p2p: sul tuo PC o su un VPS",
        "Атаки не возвращают здоровье": "Gli attacchi non recuperano salute",
        "Бег примерно с 70% наклона стика вместо 90%": "Corri con il 70% di inclinazione dello stick invece del 90%",
        "Бег с меньшим наклоном стика": "Corsa più facile",
        "Без Rally": "Senza Rally",
        "Без автоповорота камеры": "Senza rotazione automatica della camera",
        "Бессмертие": "Immortalità",
        "Бесшумность": "Passi silenziosi",
        "Бронзовый": "Bronzo",
        "Враги не видят вас и не реагируют, пока вы не атакуете": "I nemici non ti vedono né reagiscono finché non attacchi",
        "Враги не слышат вас, но видят": "I nemici non ti sentono, ma ti vedono",
        "Здоровье для Rally не исчезает со временем": "La salute recuperabile con Rally non svanisce nel tempo",
        "Золотой": "Oro",
        "Камера дальше": "Camera più lontana",
        "Камера дальше от персонажа": "La camera resta più lontana dal personaggio",
        "Камера не поворачивается сама при движении; ручное управление и захват":
            "La camera non gira da sola quando ti muovi; controllo manuale e cattura",
        "Невидимость": "Furtività",
        "Нет файлов DLSS: tools/build_dlss.sh": "Nessun file DLSS: tools/build_dlss.sh",
        "Нужны видеокарта NVIDIA RTX и проприетарный драйвер; иначе используется встроенная":
            "Serve una GPU NVIDIA RTX con il driver proprietario; altrimenti usa quella integrata",
        "Платиновый": "Platino",
        "Серебряный": "Argento",
        "Тела и предметы разлетаются от ударов": "Corpi e oggetti volano via quando colpiti",
        "Управление врагом": "Controllo dei nemici",
        "Урон проходит, но здоровье не опускается ниже 1": "Subisci danni ma non scendi mai sotto 1 PV",
        "Физика тел как в Dark Souls": "Fisica dei corpi come in Dark Souls",
    },
    "fr": {
        "Чит": "Triche",
        "R3: управлять целью, L3: вернуться. Не вместе со свободной камерой":
            "R3 : cibler l'ennemi, L3 : retour. Pas avec la caméra libre",
        "Rally не угасает": "Rally ne s'estompe jamais",
        "shadnet-p2p: на вашем ПК или VPS": "shadnet-p2p : sur votre PC ou un VPS",
        "Атаки не возвращают здоровье": "Les attaques ne rendent pas de vie",
        "Бег примерно с 70% наклона стика вместо 90%": "Courir avec environ 70 % d'inclinaison du stick au lieu de 90 %",
        "Бег с меньшим наклоном стика": "Course plus facile",
        "Без Rally": "Sans Rally",
        "Без автоповорота камеры": "Sans rotation automatique de la caméra",
        "Бессмертие": "Immortalité",
        "Бесшумность": "Pas silencieux",
        "Бронзовый": "Bronze",
        "Враги не видят вас и не реагируют, пока вы не атакуете": "Les ennemis ne vous voient ni ne réagissent avant votre attaque",
        "Враги не слышат вас, но видят": "Les ennemis ne vous entendent pas, mais ils vous voient",
        "Здоровье для Rally не исчезает со временем": "La vie récupérable avec Rally ne s'estompe pas avec le temps",
        "Золотой": "Or",
        "Камера дальше": "Caméra plus loin",
        "Камера дальше от персонажа": "La caméra reste plus loin du personnage",
        "Камера не поворачивается сама при движении; ручное управление и захват":
            "La caméra ne tourne pas toute seule quand vous bougez ; contrôle manuel et capture",
        "Невидимость": "Furtivité",
        "Нет файлов DLSS: tools/build_dlss.sh": "Pas de fichiers DLSS : tools/build_dlss.sh",
        "Нужны видеокарта NVIDIA RTX и проприетарный драйвер; иначе используется встроенная":
            "Il faut une carte NVIDIA RTX et le pilote propriétaire ; sinon la carte intégrée est utilisée",
        "Платиновый": "Platine",
        "Серебряный": "Argent",
        "Тела и предметы разлетаются от ударов": "Les corps et les objets s'envolent quand ils sont frappés",
        "Управление врагом": "Contrôle des ennemis",
        "Урон проходит, но здоровье не опускается ниже 1": "Vous subissez des dégâts mais ne descendez jamais sous 1 PV",
        "Физика тел как в Dark Souls": "Physique des corps comme dans Dark Souls",
    },
    "de": {
        "Чит": "Cheat",
        "R3: управлять целью, L3: вернуться. Не вместе со свободной камерой":
            "R3: Ziel anvisieren, L3: zurück. Nicht zusammen mit der freien Kamera",
        "Rally не угасает": "Rally verblasst nie",
        "shadnet-p2p: на вашем ПК или VPS": "shadnet-p2p: auf deinem PC oder einem VPS",
        "Атаки не возвращают здоровье": "Angriffe erstatten keine Gesundheit",
        "Бег примерно с 70% наклона стика вместо 90%": "Laufen mit etwa 70 % Stick-Neigung statt 90 %",
        "Бег с меньшим наклоном стика": "Leichteres Laufen",
        "Без Rally": "Ohne Rally",
        "Без автоповорота камеры": "Ohne automatische Kamera-Drehung",
        "Бессмертие": "Unsterblich",
        "Бесшумность": "Leise Schritte",
        "Бронзовый": "Bronze",
        "Враги не видят вас и не реагируют, пока вы не атакуете": "Feinde sehen Sie nicht und reagieren nicht, bis Sie angreifen",
        "Враги не слышат вас, но видят": "Feinde hören Sie nicht, sehen Sie aber",
        "Здоровье для Rally не исчезает со временем": "Die mit Rally zurückgewonnene Gesundheit verblasst nicht mit der Zeit",
        "Золотой": "Gold",
        "Камера дальше": "Kamera weiter weg",
        "Камера дальше от персонажа": "Die Kamera bleibt weiter von der Figur entfernt",
        "Камера не поворачивается сама при движении; ручное управление и захват":
            "Die Kamera dreht sich beim Bewegen nicht von selbst; manuelle Steuerung und Aufnahme",
        "Невидимость": "Schleichmodus",
        "Нет файлов DLSS: tools/build_dlss.sh": "Keine DLSS-Dateien: tools/build_dlss.sh",
        "Нужны видеокарта NVIDIA RTX и проприетарный драйвер; иначе используется встроенная":
            "Braucht eine NVIDIA-RTX-Grafikkarte und den proprietären Treiber; sonst wird die eingebaute verwendet",
        "Платиновый": "Platin",
        "Серебряный": "Silber",
        "Тела и предметы разлетаются от ударов": "Leichen und Gegenstände fliegen bei Treffern umher",
        "Управление врагом": "Feindsteuerung",
        "Урон проходит, но здоровье не опускается ниже 1": "Sie erleiden Schaden, fallen aber nie unter 1 LP",
        "Физика тел как в Dark Souls": "Leichenphysik wie in Dark Souls",
    },
}

# The wording that replaces what a table already says (the Chinese word for the cheats is
# "金手指", not the literal "cheating").
OVERRIDES = {
    "zh_CN": {
        "Язык": "语言",
        "Управление": "操作设置",
        "Устройство ввода": "输入设备",
        "Обновить список": "刷新列表",
        "Устройство не выбрано": "未选择设备",
        "Действие": "动作",
        "Клавиша": "键位",
        "Кнопка геймпада": "手柄按键",
        "Настроить автоматически": "自动配置",
        "Разрешение вывода": "输出分辨率",
        "Память игры": "游戏内存",
        "Язык игры": "游戏语言",
        "Китайский (традиционный)": "繁體中文",
        "Корейский": "韩语",
        "Читы": "金手指",
        "Файл читов": "金手指文件",
        "Файлы читов (*.json);;Все файлы (*)": "金手指文件 (*.json);;所有文件 (*)",
        "Авто": "自动",
        "Новая": "新",
        "Старая": "旧",
    },
}

# The tables that are completed, and the language the entries come from.
# The tables, in the order they are in the source. The code is the one the settings
# use; the name is the variable in the module.
LANGUAGES = (("pt_BR", "PT_BR"), ("es", "ES"), ("it", "IT"), ("fr", "FR"),
            ("de", "DE"), ("zh_CN", "ZH_CN"))


def load_tables(path=MODULE, source=None):
    """The tables of the module and its source.

    The module is run in a namespace of its own, so that the tables are the ones it
    really has (the source is read for the editing). A source that is given is used
    instead of the file's, so that the tables of the editing in progress are read."""
    if source is None:
        source = io.open(path, encoding="utf-8").read()
    namespace = {"__name__": "bbport_i18n_tables", "__file__": str(path)}
    exec(compile(source, str(path), "exec"), namespace)
    tables = {}
    for name, value in namespace.items():
        if name.isupper() and isinstance(value, dict):
            tables[name] = dict(value)
    return tables, source


def chinese_simplified(text):
    """The simplified form of a Chinese text."""
    from zhconv import convert
    return convert(text, "zh-cn")


def chinese_traditional(text):
    """The traditional form of a Chinese text, with the wording Taiwan uses."""
    try:
        from opencc import OpenCC
        return OpenCC("s2twp").convert(text)
    except Exception:
        from zhconv import convert
        return convert(text, "zh-tw")


def _literal(text):
    """The text as Python's source."""
    escaped = (text.replace("\\", "\\\\")
                    .replace('"', '\\"')
                    .replace("\n", "\\n")
                    .replace("\r", "\\r")
                    .replace("\t", "\\t"))
    return '"%s"' % escaped


def insert_into(source, table_name, entries):
    """Puts the entries at the end of the table in the source.

    The entries that are already there are left alone, with their comments."""
    if not entries:
        return source
    lines = source.split("\n")
    out = []
    inside = False
    for line in lines:
        if re.match(r"^%s\s*=\s*\{" % re.escape(table_name), line):
            inside = True
            out.append(line)
            continue
        if inside and re.match(r"^\}", line):
            indent = "    "
            for key, value in entries.items():
                out.append("%s%s: %s," % (indent, _literal(key), _literal(value)))
            out.append(line)
            inside = False
            continue
        out.append(line)
    if inside:
        raise SystemExit("the table %s has no end" % table_name)
    return "\n".join(out)


def _replace_entries(source, table_name, entries):
    """Replaces the wording of the entries that are already in the table.

    Only the entries of that table are touched: the same text is in every table, and each
    of them has its own wording."""
    lines = source.split("\n")
    start = None
    for index, line in enumerate(lines):
        if re.match(r"^%s\s*=\s*\{" % re.escape(table_name), line):
            start = index
            break
    if start is None:
        return source
    end = start
    for index in range(start + 1, len(lines)):
        if re.match(r"^\}", lines[index]):
            end = index
            break
    for index in range(start, end):
        for key, value in entries.items():
            pattern = re.compile(r'^(\s*)"%s":\s*".*?",\s*$' % re.escape(key))
            match = pattern.match(lines[index])
            if match:
                lines[index] = '%s"%s": %s,' % (match.group(1), key, _literal(value))
                break
    return "\n".join(lines)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="only report the gaps")
    args = parser.parse_args(argv[1:])

    tables, source = load_tables()
    english = dict(tables["EN"])
    english.update(ENGLISH)

    # Every key of every table is a text the launcher may show.
    keys = set(english)
    for table in tables.values():
        keys |= set(table)

    if args.check:
        gaps = 0
        for name in ("EN",) + tuple(table for _, table in LANGUAGES):
            table = dict(tables[name])
            table.update(ENGLISH if name == "EN" else {})
            missing = keys - set(table)
            if missing:
                print("%s is missing %d texts" % (name, len(missing)))
                gaps += len(missing)
        if not gaps:
            print("every table is complete")
        return 1 if gaps else 0

    # The English table first: the others are completed from it.
    source = insert_into(source, "EN", {k: v for k, v in ENGLISH.items() if k not in tables["EN"]})
    for code, name in LANGUAGES:
        table = tables[name]
        missing = {key: english[key] for key in keys - set(table) if key in english}
        missing.update({k: v for k, v in ADDITIONS.get(code, {}).items() if k not in table})
        if code == "zh_CN":
            missing = {key: chinese_simplified(value) for key, value in missing.items()}
        if missing:
            source = insert_into(source, name, missing)
            print("%s: %d texts added" % (name, len(missing)))
        # The wording that replaces what the table already says.
        overrides = OVERRIDES.get(code, {})
        changed = {key: value for key, value in overrides.items() if table.get(key) != value}
        if changed:
            source = _replace_entries(source, name, changed)
            print("%s: %d texts replaced" % (name, len(changed)))

    # The traditional Chinese table: the simplified one, converted. The simplified table is
    # read back from the source, so that the wording the overrides put there is in the
    # traditional one too.
    source = insert_into(source, "ZH_CN", {})
    simplified = load_tables(source=source)[0]["ZH_CN"]
    traditional = {key: chinese_traditional(value) for key, value in simplified.items()}
    # The traditional table is made from the simplified one, so it is written as a whole:
    # a text that the simplified one says differently is different here too.
    block = ["ZH_TW = {",
             "    # The traditional Chinese (made from the simplified one by",
             "    # tools/complete_translations.py; the wording Taiwan uses)."]
    block += ["    %s: %s," % (_literal(k), _literal(v)) for k, v in traditional.items()]
    block.append("}")
    if "ZH_TW" in tables:
        # The replacement is a function: re.sub would read the backslashes of the text
        # (the "\n" of a line break) as its own escapes.
        source = re.sub(r"^ZH_TW\s*=\s*\{.*?^\}", lambda _: "\n".join(block), source,
                        count=1, flags=re.S | re.M)
    else:
        marker = "\n# Translations by language code"
        source = source.replace(marker, "\n" + "\n".join(block) + "\n" + marker, 1)
    print("ZH_TW: %d texts" % len(traditional))

    # The table has to be in the list of the translations, or it is never used.
    if '"zh_TW"' not in source:
        source = re.sub(r'("zh_CN": ZH_CN)\}', r'\1, "zh_TW": ZH_TW}', source, count=1)

    io.open(MODULE, "w", encoding="utf-8", newline="\n").write(source)
    print("written:", MODULE)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
