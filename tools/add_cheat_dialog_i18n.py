#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Adds the cheat page's texts to every language of the launcher's translations.

The Russian text is the key (launcher/bbport_i18n.py). Each language's block gets the new
keys with their translation; the ones that are already there are left alone.

    python3 tools/add_cheat_dialog_i18n.py
"""
import re
import sys
from pathlib import Path

I18N = Path(__file__).resolve().parent.parent / "launcher" / "bbport_i18n.py"

# The Russian key and its translation in each language.
TEXTS = {
    "Включить все": {
        "EN": "Enable all",
        "PT_BR": "Ativar todas",
        "ES": "Activar todos",
        "IT": "Attiva tutti",
        "FR": "Tout activer",
        "DE": "Alle aktivieren",
        "ZH_CN": "全部启用",
    },
    "Выключить все": {
        "EN": "Disable all",
        "PT_BR": "Desativar todas",
        "ES": "Desactivar todos",
        "IT": "Disattiva tutti",
        "FR": "Tout désactiver",
        "DE": "Alle deaktivieren",
        "ZH_CN": "全部禁用",
    },
    "Включённые читы": {
        "EN": "Enabled cheats",
        "PT_BR": "Trapaças ativas",
        "ES": "Trucos activados",
        "IT": "Trucchi attivati",
        "FR": "Triches activées",
        "DE": "Aktive Cheats",
        "ZH_CN": "已启用的金手指",
    },
}

LANGUAGES = ("EN", "PT_BR", "ES", "IT", "FR", "DE", "ZH_CN")


def escape(text):
    return text.replace("\\", "\\\\").replace('"', '\\"')


def main():
    text = I18N.read_text(encoding="utf-8")
    added = 0
    for language in LANGUAGES:
        start = re.search(r"^%s = \{$" % language, text, re.M)
        if not start:
            print(f"no block for {language}")
            continue
        # The block ends at its closing brace (the last entry and the brace share a line; a
        # comment about the next language may follow).
        following = re.search(r"\},?$", text[start.end():], re.M)
        if not following:
            print(f"no block after {language}")
            continue
        end = start.end() + following.end()
        block = text[start.start():end]
        lines = []
        for key, translations in TEXTS.items():
            if f'"{escape(key)}"' in block:
                continue
            value = translations[language]
            lines.append(f'    "{escape(key)}": "{escape(value)}",')
        if not lines:
            print(f"{language}: nothing to add")
            continue
        # The new keys go at the end of the block (before its closing brace).
        closing = block.rstrip()
        assert closing.endswith("}"), closing[-40:]
        closing = closing[:-1].rstrip()
        # The last entry of the block may or may not have a comma of its own.
        if not closing.endswith(","):
            closing += ","
        new_block = closing + "\n" + "\n".join(lines) + "}\n"
        text = text[:start.start()] + new_block + text[end:]
        added += len(lines)
        print(f"{language}: {len(lines)} texts")
    I18N.write_text(text, encoding="utf-8")
    print(f"added {added} texts")
    return 0


if __name__ == "__main__":
    sys.exit(main())
