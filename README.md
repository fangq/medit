# medit — Lightweight Programmer's Text Editor

- Copyright: (c) Yevgen Muntyan (2004-2010), Qianqian Fang <q.fang at neu.edu> (2025-2026)
- License: GNU Lesser General Public License (LGPL) version 2.1 or later.
- Version: 1.8.0
- Github: <https://github.com/fangq/medit>

**medit** is a lightweight, feature-rich text editor designed for programming and everyday text editing. Built on GTK, it provides a clean, fast editing experience with powerful features including split views, an integrated terminal, plugin support, and extensive customization options.


## What's New in 1.8.x (GTK3 Branch)

The 1.8.x series represents a major modernization of medit:

1. **GTK2 → GTK3 migration** — modern widget toolkit with better HiDPI support
2. **Python 2 → Python 3** — updated plugin system for current Python
3. **C++ libvte terminal** — replaces the old Python-based VTE terminal
4. **Split terminal panels** — multiple terminal sessions in the terminal pane
5. **Fast long-line loading** — visual truncation with progressive reveal for files with very long lines
6. **Column/box selection** — `Ctrl+drag` rectangular selection with full copy/paste support
7. **Toggle matching bracket** — `Ctrl+]` to jump between a bracket and its match
8. **Ctrl+scroll font zoom** — increase/decrease the editor notebook font size
9. **Code folding** — collapse/expand blocks for curly-bracket languages (C/C++/Java/JS), `end`-keyword languages (Ruby/Pascal/Matlab/Octave), and indentation-based languages (Python)
10. **Indentation shortcuts** — `Ctrl+9` to indent / `Ctrl+0` to unindent selected text by one space
11. **Breadcrumb folder navigation** — clickable path bar in the file selector for fast directory traversal
12. **CMake build support** — CMake is now the supported build system
13. **Customizable color themes** — extended theme and terminal color scheme support
14. **Spell checking** — inline misspelling underlines with suggestions (gspell)
15. **Markdown and wiki preview** — live rendered preview pane
16. **Integrated GDB debugger** — breakpoints, stepping, call stack, locals,
    watches, hover-to-inspect, and `launch.json` support
17. **Numerous bug fixes** and performance improvements


## Features

### Core Editing
- **Syntax highlighting** for 100+ programming and markup languages (via GtkSourceView)
- **Multiple documents** with tabbed interface
- **Split views** — view the same or different files side by side
- **Split notebooks** — independent tab groups for flexible layouts
- **Line numbers**, current line highlighting, and right margin indicator
- **Bracket matching** with highlight; `Ctrl+]` toggles between a bracket and its match
- **Bookmarks** — set, navigate, and manage bookmarks across documents
- **Code folding** — collapse/expand code blocks with margin glyphs; supports curly-bracket languages (C/C++/Java/JavaScript/etc.), `end`-keyword languages (Ruby/Pascal/Matlab/Octave), and indentation-based languages (Python)
- **Indentation adjustment** — `Ctrl+9` / `Ctrl+0` to indent or unindent selected lines by one space
- **Configurable word wrap** — none, character, or word boundaries
- **Undo/Redo** with unlimited history

### Column (Box) Selection
- **Ctrl+click+drag** to select a rectangular block of text
- Per-character highlight using the theme selection color
- **Copy** column-selected text (`Ctrl+C`)
- **Paste** column text at cursor position — inserts per-line at the cursor column
- **Paste into box selection** — replaces the selected rectangle with pasted block
- Full Unicode support for column operations

### Code Folding

medit supports folding code blocks directly in the editor margin:

- **Curly-bracket languages** (C, C++, Java, JavaScript, etc.) — folds `{...}` blocks
- **`end`-keyword languages** (Ruby, Pascal, Matlab, Octave) — folds `begin...end`, `if...end`, `for...end`, and similar constructs
- **Indentation-based languages** (Python) — folds blocks defined by consistent indentation
- Click the **fold glyph** (`▾`/`▸`) in the left margin to collapse or expand a block
- Folded regions are visually indicated; inner folds are preserved when a parent is collapsed
- Syntax highlighting and long-line handling remain active for folded content

### Long Line Handling
- **Fast loading** of files with very long lines (thousands of characters)
- Lines beyond 4096 characters are visually truncated with a `...` indicator
- **Click the `...` marker** to progressively reveal 4096 more characters
- Full line content preserved in buffer — copy/paste and save work on complete text
- Long-line truncation applies automatically to pasted text in new documents

### Font Zoom
- **Ctrl+Scroll wheel** to increase/decrease the editor notebook font size
- Temporary zoom — does not affect global preferences
- Supports both discrete scroll wheel and smooth trackpad scrolling
- Font size range: 4 pt – 72 pt

### Integrated Terminal
- **Built-in terminal** panel (C++ libvte-based)
- **Split terminal panels** — run multiple independent terminal sessions
- Terminal tracks the current file's directory automatically
- **Multiple color schemes** — Default, Black on White, Green on Black, VIM Colors, and more

### Symbol Browser (Ctags)
- **Symbol browser panel** — lists functions, classes, and variables in the active document
- Powered by **ctags** for accurate symbol extraction across many languages
- Click any symbol to jump to its definition line
- Tree view auto-refreshes when switching documents

### Find in Files
- **Multi-file search** — grep-like search across a directory tree
- Supports **regular expressions** and file pattern filters
- Automatically skips VCS directories (`.git/`, `.svn/`, `.hg/`, `CVS/`)
- Results listed in a panel with one-click jump to matching line

### File Management
- **File selector panel** — browse and open files from a sidebar tree with **breadcrumb navigation bar** for quickly jumping up the folder hierarchy
- **File list panel** — quick access to open documents
- **File filters** — configurable patterns to show/hide files in the selector
- **Recent files** list with history
- **Session support** — save and restore open documents and window layout
- **File group support** — organize files into logical groups

### Extensibility
- **Python 3 plugin support**
- **Lua scripting** support
- **Configurable command-line tools** — run external commands from the editor
- **Custom actions** with keyboard shortcuts

### Customization
- **Configurable color themes** with multiple built-in schemes
- **Keyboard shortcut editor** — remap any command in Preferences
- **Per-language settings** — tab width, indentation style, word characters
- **Configurable encoding** detection and conversion

## Building from Source

### Dependencies

Required: CMake >= 3.16, pkg-config, a C11/C++14 compiler, GTK+ >= 3.22,
GLib >= 2.56, libxml2 >= 2.9, intltool and python3 (build-time only).

Optional, auto-detected: VTE >= 0.52 (terminal), gspell >= 1.2 plus hunspell
and a dictionary (spell check), md4c/md4c-html >= 0.4 (Markdown preview),
python3-dev with pygobject >= 3.30 (Python plugins), and gdb at run time for
the debugger plugin.

GtkSourceView, Lua, xdgmime and EggSMClient are vendored in the source tree,
so no system packages of those are needed.

On Debian/Ubuntu:

```bash
sudo apt install build-essential cmake pkg-config intltool \
    libgtk-3-dev libxml2-dev libglib2.0-dev libvte-2.91-dev \
    libgspell-1-dev libhunspell-dev hunspell-en-us \
    libmd4c-dev libmd4c-html0-dev python3-dev gdb
```

On Fedora/RHEL:

```bash
sudo dnf install gcc gcc-c++ cmake pkgconfig intltool \
    gtk3-devel libxml2-devel glib2-devel vte291-devel \
    gspell-devel hunspell-devel hunspell-en-US \
    md4c-devel python3-devel gdb
```

### Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"
sudo cmake --install build
```

Run the test suite with `ctest --test-dir build --output-on-failure`
(prefix with `xvfb-run -a` on a headless machine).

Common options:

| Option | Description |
|--------|-------------|
| `-DCMAKE_INSTALL_PREFIX=/usr` | Install prefix (default `/usr/local`) |
| `-DCMAKE_BUILD_TYPE=...` | `Debug`, `Release`, `RelWithDebInfo` (default), `MinSizeRel` |
| `-DMOO_WITH_PYTHON=yes\|no` | Python plugin support (default: auto-detect) |
| `-DMOO_STRICT_MODE=ON` | All warnings plus `-Werror` |
| `-DMOO_SANITIZE=address,undefined` | Build with sanitizers |
| `-DMOO_ENABLE_COVERAGE=ON` | Code-coverage instrumentation |
| `-DMOO_BUILD_MODULE=ON` | Also build the `_moo` Python module |

### Legacy autotools build

An autotools build still exists but is **unsupported and incomplete**: it has
no detection for gspell, md4c, the wiki preview or gdb, so it cannot build
spell checking, the page preview or the debugger. Use CMake.

## Keyboard Shortcuts

### General
| Shortcut | Action |
|----------|--------|
| `Ctrl+N` | New document |
| `Ctrl+O` | Open file |
| `Ctrl+S` | Save |
| `Ctrl+Shift+S` | Save As |
| `Ctrl+W` | Close tab |
| `Ctrl+Z` | Undo |
| `Ctrl+Shift+Z` | Redo |
| `Ctrl+F` | Find |
| `Ctrl+R` | Replace |
| `Ctrl+G` | Go to line |

### Navigation
| Shortcut | Action |
|----------|--------|
| `Ctrl+]` | Toggle between matching brackets |
| `Ctrl+B` | Toggle bookmark |
| `Alt+Down` | Next bookmark |
| `Alt+Up` | Previous bookmark |
| `Alt+Left/Right` | Previous/next tab |

### Column Selection
| Shortcut | Action |
|----------|--------|
| `Ctrl+Click+Drag` | Box/column select |
| `Ctrl+C` | Copy box selection |
| `Ctrl+V` | Paste box text (column mode) |
| `Escape` | Clear box selection |

### Indentation
| Shortcut | Action |
|----------|--------|
| `Ctrl+9` | Indent selected lines by one space |
| `Ctrl+0` | Unindent selected lines by one space |

### View
| Shortcut | Action |
|----------|--------|
| `Ctrl+Scroll` | Zoom in/out (font size) |
| `F5` | Reload file |

## Project Structure

```
medit/
├── CMakeLists.txt          # CMake build system
├── moo/
│   ├── medit-app/          # Application entry point
│   ├── mooedit/            # Core editor widgets and file operations
│   │   ├── mootextview.c       # Text view widget (rendering, selection, zoom)
│   │   ├── mootextview-input.c # Input handling (mouse, keyboard, box selection)
│   │   ├── mooedit-fileops.cpp  # File loading/saving, long-line truncation
│   │   ├── mooeditwindow.cpp    # Editor window, actions, menu
│   │   └── langs/              # Syntax highlighting definitions
│   ├── mooutils/           # Utility widgets and helpers
│   ├── moocpp/             # C++ utility wrappers
│   ├── plugins/            # Built-in plugins (terminal, file selector, etc.)
│   └── gtksourceview/      # Embedded GtkSourceView (syntax engine)
├── api/                    # API documentation
├── doc/                    # User documentation
└── po/                     # Translations
```

## License

medit is free software; you can redistribute it and/or modify it under the terms of the **GNU Lesser General Public License** as published by the Free Software Foundation; either version 2.1 of the License, or (at your option) any later version.

See [COPYING](COPYING) for the full license text.

## Credits

- **Yevgen Muntyan** — original author (2004–2010)
- **Qianqian Fang** — contributed to migrating the medit codebase from Python 2 to Python 3, as well as from GTK2 to GTK3, with assistance from the AI coding assistant [Claude](https://claude.ai) (Anthropic). Also developed many of the new features in the 1.8.x branch including code folding (curly-bracket, end-keyword, and indentation-based languages), column selection, long-line handling, bracket toggle navigation, indentation shortcuts, font zoom, breadcrumb folder navigation, and CMake build support.
- **Contributors** — see git log for the full list of contributors

## Links

- Source code: [GitHub](https://github.com/fangq/medit)
- Bug reports: GitHub Issues
- Original project: http://mooedit.sourceforge.net/
