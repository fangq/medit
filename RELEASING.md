# Cutting a release

## 1. Version

`CMakeLists.txt` holds the version in one place:

```cmake
set(MOO_MAJOR_VERSION  1)
set(MOO_MINOR_VERSION  8)
set(MOO_MICRO_VERSION  0)
set(MOO_VERSION_SUFFIX "devel")   # <- clear this for a release
```

**`MOO_VERSION_SUFFIX` is deliberately left as `"devel"` in the repository.**
Clear it (`set(MOO_VERSION_SUFFIX "")`) in the release commit only, then set it
back afterwards. Everything downstream — the About dialog, `--version`, the
`.deb`/`.dmg` names and the Windows installer — derives from it, so a tree that
says `1.8.0` while development continues is worse than one that says
`1.8.0-devel`.

Keep `configure.ac` in step if you still care about the legacy autotools build.

## 2. Update the release notes

- Add the entry to `NEWS` and replace `Released <unreleased>` with the date.
- Add a matching `<release version="..." date="..."/>` to
  `data/medit.metainfo.xml.in`. The date currently defaults to the *build*
  date, which is fine for development builds but should be pinned for a
  release.
- Check `README.md`'s feature list still matches reality.

## 3. Pre-flight checks

```bash
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-rel -j"$(nproc)"
xvfb-run -a ctest --test-dir build-rel --output-on-failure

# warnings must stay at zero
cmake -S . -B build-strict -DMOO_STRICT_MODE=ON -DMOO_WITH_PYTHON=yes
cmake --build build-strict -j"$(nproc)"

# no memory errors
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
      -DMOO_SANITIZE=address,undefined
cmake --build build-asan -j"$(nproc)"
xvfb-run -a ctest --test-dir build-asan --output-on-failure

# packaging metadata
desktop-file-validate build-rel/moo/medit-app/medit.desktop
appstreamcli validate build-rel/moo/data/io.github.fangq.medit.metainfo.xml
```

Then a manual pass from a **fresh clone** (no stray `config.h` from an old
autotools run, which can mask feature-flag problems): confirm the Terminal,
Page Preview and Debugger panes all appear, and check the editor against both
a light and a dark theme.

## 4. Tag

CI builds Linux, macOS (arm64) and Windows on every push, and the `release`
job attaches artifacts to a GitHub Release for tags matching `v*.*.*`:

```bash
git tag -a v1.8.0 -m "medit 1.8.0"
git push origin v1.8.0
```

## 5. After the release

Restore `MOO_VERSION_SUFFIX "devel"` and open a new `NEWS` section.

## Known follow-up work

Not release blockers, but tracked:

- **Deprecation warnings.** A strict build (`-DMOO_STRICT_MODE=ON`) is
  error-free but prints **297** `-Wdeprecated-declarations` warnings, spread
  across `mooglade.c` (28), `moofontsel.c` (24), `mooeditwindow.cpp` (16),
  `mootextview.c` (14), `moopane.c` (12) and the action/UI-XML files. They are
  deliberately non-fatal (`-Wno-error=deprecated-declarations`) so the
  `-Werror` gate is usable today; retiring them is the `GtkAction`/`GtkStock`
  work listed below.
  Separately, ~56 GLib **macro** deprecations (`G_INLINE_FUNC`,
  `G_UNICODE_COMBINING_MARK`, `g_type_class_add_private`) are reported via
  `#pragma` and so are not covered by that flag; they are suppressed with
  `GLIB_DISABLE_DEPRECATION_WARNINGS`, which stays defined even in strict mode
  until they are fixed. Most are in the vendored `eggsmclient` and
  `gtksourceview` trees. See `cmake/MooCompilerFlags.cmake`.

  The **default** build is warning-free, and that is the baseline worth
  keeping.
- `GtkAction`/`GtkUIManager` are used throughout and are deprecated since GTK
  3.10. They work fine on 3.x; porting to `GAction`/`GMenu` is a separate
  project.
- `GtkStock`/`GtkIconFactory` (`moo/mooutils/moostock.c`) should become
  `gtk_image_new_from_icon_name`; the stock names are already freedesktop
  names, so the factory buys nothing.
- The Python defs plumbing is incomplete: `cmake/MooPython.cmake` sets the
  defs directories to `""`, so `moo/CMakeLists.txt` passes absolute paths at
  `/`. The untracked `moo/*-types.defs` stubs and `fix_pygobject_types*.py`
  scripts in the working tree are a manual workaround for this.
- `moo/moopython/plugins/terminal.py` is still installed even though the C++
  libvte terminal replaced it; with Python enabled a user gets two competing
  terminal plugins.
- `intltool` is deprecated and unavailable on some current distros;
  `cmake/MooNLS.cmake` makes it a hard `FATAL_ERROR`. It is only really used to
  merge `medit.desktop.in`, which `msgfmt --desktop` can do instead.
- Translations date from 2014 and the POT needs regenerating.
- `doc/help/` ships HTML generated in 2010 and `doc/built/medit.1` is dated
  September 2010.
