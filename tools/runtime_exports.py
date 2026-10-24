# SPDX-License-Identifier: GPL-2.0-or-later
"""Writes the module definition file (.def) of the Windows runtime.

The GPU library (bbgpu.dll) calls the runtime (bb-probe.exe) for the guest's memory, the
fault recovery and the like. On Linux the two share the symbols the usual way (the runtime
is linked with -rdynamic, the library leaves them undefined). Windows has no such thing:
a DLL's imports are resolved when it is loaded, from another module's exports.

So the runtime exports the symbols the library needs (the .def file lists them) and the
library links against the import library made from the same file. The file is written from
the list the build knows: tools/build_windows_runtime.sh passes it on.

    python tools/runtime_exports.py OUTPUT.def SYMBOL...
"""
import sys


def main(argv):
    if len(argv) < 3:
        print('Usage: runtime_exports.py OUTPUT.def SYMBOL...', file=sys.stderr)
        return 2
    output = argv[1]
    symbols = sorted(set(argv[2:]))
    lines = ['; The runtime\'s exports (bb-probe.exe): the symbols the GPU library calls.',
             '; Written by tools/runtime_exports.py; do not edit by hand.',
             'EXPORTS']
    for symbol in symbols:
        lines.append('    %s' % symbol)
    with open(output, 'w', encoding='utf-8', newline='\r\n') as handle:
        handle.write('\n'.join(lines) + '\n')
    print('%s: %d exports' % (output, len(symbols)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
