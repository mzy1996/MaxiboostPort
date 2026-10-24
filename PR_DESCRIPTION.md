The launcher for Windows: a window that needs nothing installed, in every language

The port also runs on Windows, with MinGW-w64 (the guest speaks the SysV ABI and
uses pthreads, which MSVC does not have).

**The launcher.** `launcher/MAXIBOOST ON.exe` is a single file: the interpreter, the
window's library and the scripts that prepare the game travel with it, so the player
double clicks it and nothing else is needed. The window asks for the game folder, shows
the cheats, the settings and the log.

**The translations are complete.** A text that a language does not have falls back to
the English one and never to the Russian key, which used to make the window a mixture of
two languages. The traditional Chinese is made from the simplified one (OpenCC), and the
system language is read from Windows itself (`GetUserDefaultLocaleName`), which is the
only way there, as Windows has no `LANG`. The menu bar holds the language, so that it
can be changed from any page.

**The controls page.** The connected input devices are shown with the architecture they
are read through (XInput or DirectInput); the bindings can be changed one by one, and
the automatic configuration fills them in for the device that is there. The bindings are
written into the configuration file (`key.cross=...`, `pad.cross=...`), the same lines
the runtime reads.

**The picture is 16:9 and is not stretched.** The window takes the 16:9 part of the
display (the rest is black), and a window of another shape keeps its shape, the picture
being drawn in its 16:9 part. The output resolution is chosen in the settings.

**The game's own language follows the release.** The North American one is in English,
the Hong Kong one in the traditional Chinese and the Korean. The list in the launcher
holds the languages of the game in the folder that is chosen, and the system's language
is used only when the game has it.

**The package ships every DLL the program needs.** The list is worked out from the
program's own import table (`tools/collect_dlls.py`) and checked before the package is
made (`tools/check_package.py`).

The README is not changed: the notes about the Hong Kong release and about the Windows
build are in `BUILD.win.md`, which describes the build anyway.
