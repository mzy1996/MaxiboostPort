# SPDX-License-Identifier: GPL-2.0-or-later
"""Adds the launcher texts the Windows launcher introduced to every language table.

The Russian text is the key; an existing key is never overwritten.
"""
import ast
import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
I18N = os.path.normpath(os.path.join(HERE, '..', 'launcher', 'bbport_i18n.py'))

LANGUAGES = ('EN', 'PT_BR', 'ES', 'IT', 'FR', 'DE', 'ZH_CN')

NEW = {
    'MAXIBOOST ON 1.15: поддерживается': {
        'EN': 'MAXIBOOST ON 1.15: supported',
        'PT_BR': 'MAXIBOOST ON 1.15: suportado',
        'ES': 'MAXIBOOST ON 1.15: compatible',
        'IT': 'MAXIBOOST ON 1.15: supportato',
        'FR': 'MAXIBOOST ON 1.15 : pris en charge',
        'DE': 'MAXIBOOST ON 1.15: unterstützt',
        'ZH_CN': 'MAXIBOOST ON 1.15：受支持',
    },
    'Выбрать папку игры': {
        'EN': 'Choose the game folder',
        'PT_BR': 'Escolher a pasta do jogo',
        'ES': 'Elegir la carpeta del juego',
        'IT': 'Scegli la cartella del gioco',
        'FR': 'Choisir le dossier du jeu',
        'DE': 'Spielordner wählen',
        'ZH_CN': '选择游戏文件夹',
    },
    'Выход': {
        'EN': 'Quit',
        'PT_BR': 'Sair',
        'ES': 'Salir',
        'IT': 'Esci',
        'FR': 'Quitter',
        'DE': 'Beenden',
        'ZH_CN': '退出',
    },
    'Диагностика падений': {
        'EN': 'Crash diagnostics',
        'PT_BR': 'Diagnóstico de travamentos',
        'ES': 'Diagnóstico de fallos',
        'IT': 'Diagnostica degli arresti anomali',
        'FR': 'Diagnostic des plantages',
        'DE': 'Absturzdiagnose',
        'ZH_CN': '崩溃诊断',
    },
    'Игра завершилась (код {}).': {
        'EN': 'The game exited (code {}).',
        'PT_BR': 'O jogo terminou (código {}).',
        'ES': 'El juego terminó (código {}).',
        'IT': 'Il gioco è terminato (codice {}).',
        'FR': "Le jeu s'est terminé (code {}).",
        'DE': 'Das Spiel wurde beendet (Code {}).',
        'ZH_CN': '游戏已退出（代码 {}）。',
    },
    'Игра не найдена: выберите папку с eboot.bin': {
        'EN': 'Game not found: choose the folder with eboot.bin',
        'PT_BR': 'Jogo não encontrado: escolha a pasta com eboot.bin',
        'ES': 'Juego no encontrado: elige la carpeta con eboot.bin',
        'IT': 'Gioco non trovato: scegli la cartella con eboot.bin',
        'FR': 'Jeu introuvable : choisissez le dossier avec eboot.bin',
        'DE': 'Spiel nicht gefunden: Ordner mit eboot.bin wählen',
        'ZH_CN': '未找到游戏：请选择包含 eboot.bin 的文件夹',
    },
    'Модель памяти': {
        'EN': 'Memory model',
        'PT_BR': 'Modelo de memória',
        'ES': 'Modelo de memoria',
        'IT': 'Modello di memoria',
        'FR': 'Modèle de mémoire',
        'DE': 'Speichermodell',
        'ZH_CN': '内存模型',
    },
    'Не найден исполняемый файл bb-probe.exe. Соберите проект.': {
        'EN': 'bb-probe.exe not found. Build the project.',
        'PT_BR': 'Arquivo executável bb-probe.exe não encontrado. Compile o projeto.',
        'ES': 'No se encontró el ejecutable bb-probe.exe. Compila el proyecto.',
        'IT': 'File eseguibile bb-probe.exe non trovato. Compila il progetto.',
        'FR': 'Exécutable bb-probe.exe introuvable. Compilez le projet.',
        'DE': 'bb-probe.exe nicht gefunden. Bitte das Projekt bauen.',
        'ZH_CN': '未找到可执行文件 bb-probe.exe。请先编译项目。',
    },
    'Ограничение кадров': {
        'EN': 'Frame rate limit',
        'PT_BR': 'Limite de quadros',
        'ES': 'Límite de fotogramas',
        'IT': 'Limite di frame',
        'FR': "Limite de fréquence d'images",
        'DE': 'Bildratenbegrenzung',
        'ZH_CN': '帧率限制',
    },
    'Очистить журнал': {
        'EN': 'Clear the log',
        'PT_BR': 'Limpar o registro',
        'ES': 'Vaciar el registro',
        'IT': 'Svuota il registro',
        'FR': 'Effacer le journal',
        'DE': 'Protokoll leeren',
        'ZH_CN': '清空日志',
    },
    'Проверка Vulkan': {
        'EN': 'Vulkan validation',
        'PT_BR': 'Validação Vulkan',
        'ES': 'Validación de Vulkan',
        'IT': 'Validazione Vulkan',
        'FR': 'Validation Vulkan',
        'DE': 'Vulkan-Validierung',
        'ZH_CN': 'Vulkan 验证',
    },
    'Режим презентации': {
        'EN': 'Presentation mode',
        'PT_BR': 'Modo de apresentação',
        'ES': 'Modo de presentación',
        'IT': 'Modalità presentazione',
        'FR': 'Mode présentation',
        'DE': 'Präsentationsmodus',
        'ZH_CN': '演示模式',
    },
    'Статистика кадров': {
        'EN': 'Frame statistics',
        'PT_BR': 'Estatísticas de quadros',
        'ES': 'Estadísticas de fotogramas',
        'IT': 'Statistiche dei frame',
        'FR': "Statistiques d'images",
        'DE': 'Bildstatistik',
        'ZH_CN': '帧统计',
    },
    'Файл читов не выбран': {
        'EN': 'No cheat file chosen',
        'PT_BR': 'Nenhum arquivo de cheats escolhido',
        'ES': 'No se eligió un archivo de cheats',
        'IT': 'Nessun file di cheat scelto',
        'FR': 'Aucun fichier de triche choisi',
        'DE': 'Keine Cheat-Datei gewählt',
        'ZH_CN': '未选择金手指文件',
    },
    'Язык игры': {
        'EN': 'Game language',
        'PT_BR': 'Idioma do jogo',
        'ES': 'Idioma del juego',
        'IT': 'Lingua del gioco',
        'FR': 'Langue du jeu',
        'DE': 'Spielsprache',
        'ZH_CN': '游戏语言',
    },
    'Язык интерфейса': {
        'EN': 'Interface language',
        'PT_BR': 'Idioma da interface',
        'ES': 'Idioma de la interfaz',
        'IT': "Lingua dell'interfaccia",
        'FR': "Langue de l'interface",
        'DE': 'Oberflächensprache',
        'ZH_CN': '界面语言',
    },
}


def dump(value):
    """A Python string literal, quoted the way the file quotes them."""
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'


def main():
    with io.open(I18N, 'r', encoding='utf-8') as handle:
        source = handle.read()
    tree = ast.parse(source)

    # The tables are edited line by line: the AST column offsets are byte offsets and do
    # not match the character offsets of the text.
    lines = source.splitlines(keepends=True)

    inserts = []  # (line index, text)
    for node in tree.body:
        if not isinstance(node, ast.Assign):
            continue
        target = getattr(node.targets[0], 'id', '')
        if target not in LANGUAGES:
            continue
        table = ast.literal_eval(node.value)
        missing = [key for key in NEW if key not in table]
        if not missing:
            continue
        index = node.value.end_lineno - 1
        line = lines[index]
        brace = line.rfind('}')
        if brace < 0:
            raise SystemExit('%s: no closing brace on line %d' % (target, index + 1))
        head = line[:brace]
        comma = '' if head.rstrip().endswith(',') else ','
        block = ['\n', '    # The Windows launcher\n']
        for key in missing:
            block.append('    %s: %s,\n' % (dump(key), dump(NEW[key][target])))
        lines[index] = head + comma + ''.join(block) + line[brace:]

    result = ''.join(lines)
    ast.parse(result)
    with io.open(I18N, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write(result)
    print('added %d language tables' % len(inserts))


if __name__ == '__main__':
    sys.exit(main())
