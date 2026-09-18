# SpeedCrunch — macOS Apple Silicon (arm64) build notes

**Date:** 2026-09-18 (updated 2026-09-19: added Intel build + switched to official Qt — see §1b)
**Built on:** MacBook Air (Apple M4), macOS (Darwin 25.6), Xcode 16.4 command line tools (AppleClang 17)
**Result:** two native, self-contained, ad-hoc-signed, unnotarised dmgs —
`dist/SpeedCrunch-1.0-604-g3aa5f060-macOS-arm64.dmg` (15 MB) and `...-macOS-x86_64.dmg` (16 MB).
Both include the local changes (Classic Appearance toggle + classic 0.12 terminal icon; new "S" icon
assets kept) on branch `macos-arm64-classic-appearance` over upstream master `b598d91d`.

This file records exactly what was built, every problem hit, and every change made,
so the work can be reviewed and reproduced, and shared with the upstream maintainer.
The original single-arch Homebrew-Qt narrative is kept below as history; §1b records the
current method (both arches, official Qt) that produced the shipped dmgs.

---

## 1. What was built

| Item | Value |
|------|-------|
| Upstream repo | https://bitbucket.org/heldercorreia/speedcrunch |
| Branch | `master` |
| **Base upstream commit** | `b598d91db29333eb3536ff0fe86c2abcfe5c6ad8` (2026-08-28, "Match precision label menu color") |
| **Built commit** | `3aa5f060` (branch `macos-arm64-classic-appearance`, = base + 13 local commits) |
| Version | 1.0 (build `1.0-604-g3aa5f060`) — see §1a |
| App icon | Classic 0.12 terminal icon (Retina), new "S" icon kept in tree/history |
| Classic Appearance | New default-OFF Settings ▸ Appearance toggle (0.12 compact look) |
| Build system | CMake (`src/CMakeLists.txt`) + CPack — this is the current upstream method |
| Qt | Qt **6.11.2** (official, via aqtinstall; universal, thinned per-arch — see §1b) |
| Architecture | **native arm64 and native x86_64** (one dmg each) |
| Deployment target | macOS **13.0** (matches the official Qt 6.11 floor) |
| Minimum OS to run | macOS 13 (Ventura) |
| Code signing | **Ad-hoc only** (no Apple Developer ID); not notarised |
| HiDPI/Retina fix | Present — `NSHighResolutionCapable` in `Contents/Info.plist` (upstream issue #1106) |

**Source tree was NOT modified.** Every fix below is a build-invocation flag or a
post-build packaging step, so the corresponding GPL source is upstream commit
`b598d91d…` verbatim — no source patch is required.

---

## 1a. Versioning — why "1.0" and what the build id means

- Upstream's declared version is **1.0**: set in `src/CMakeLists.txt`
  (`speedcrunch_VERSION "1.0"`), `src/speedcrunch.pro`, and `pkg/Info.plist`
  (`CFBundleShortVersionString`). The README states "Current stable version: 1.0."
  The live update server `speedcrunch.org/version` also returns `1.0`.
- Version is bumped via the upstream script `adm/set-project-version.sh <ver>`
  (single source of truth; writes CMake, qmake, Linux packaging, and Sphinx docs).
  There is also a numeric config-migration scheme in `settings.cpp`:
  `major*10000 + minor*100 + patch` (`1200` = 0.12.0; still `1200` because no config
  break since 0.12).
- Version 1.0 was designated by the maintainer in commit `2a997f04`
  ("Set app version to 1.0", 2026-03-08).
- **There is no git *release tag* for 1.0** (last tag is `release-0.12.0`, 2016).
  The build commit `b598d91d` is **591 commits after** the 1.0 designation, so this is
  **1.0 plus unreleased commits**. Captured as the `git describe`-style identifier
  **`1.0-591-gb598d91d`**, used in the dmg filename and read-me. The app's internal
  version is left unmodified at upstream's `1.0`.

---

## 1b. Current build method (2026-09-19) — both arches, official Qt

The first build (§1, §2 below) used Homebrew's arm64 Qt and shipped arm64 only. It was then
superseded to (a) also serve Intel Macs and (b) shrink the bundle, using a single unified script
`scripts/build_speedcrunch_macos.sh <arm64|x86_64>`.

- **Official Qt via `aqtinstall`, not Homebrew.** Homebrew's Qt bundles a ~32 MB `libicudata`
  plus ICU i18n/uc and OpenSSL `libcrypto` (~40 MB total) that the official Qt build does not need.
  The Homebrew arm64 bundle was ~80 MB (31 MB dmg); the official-Qt bundles are ~37 MB (**15 MB
  arm64 / 16 MB x86_64 dmg**). Setup: `pip install aqtinstall` in a venv (Homebrew's Python is
  externally-managed), then `aqt install-qt mac desktop 6.11.2 clang_64` → a **universal**
  (x86_64+arm64) Qt under `intel-build/Qt/6.11.2/macos`. (`intel-build/` is git-ignored.)
- **Per-arch build + thin.** Configure with `-DCMAKE_OSX_ARCHITECTURES=<arch>` and
  `-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0`, run macdeployqt, then `lipo -thin <arch>` every Mach-O in
  the bundle so only the target slice ships.
- **SQL drivers.** Official Qt ships all four SQL drivers; `libqsqlpsql/odbc/mimer` link to DB
  client libs not present on the build machine, so macdeployqt leaves them with dangling external
  deps. Keep only `libqsqlite.dylib` (QtHelp needs it) and delete the other three. (Homebrew's Qt
  only ever had the sqlite driver, so the original arm64 build never hit this.)
- **x86_64 is test-launched under Rosetta 2** on the M4 build machine; arm64 runs natively. Both
  pass the "no dependency outside the bundle" check and an ad-hoc `codesign --verify --deep --strict`.
- **Minimum macOS is now 13.0** (official Qt 6.11 floor), down from 14.0 (Homebrew floor).

The §2 bug narrative below documents the original Homebrew path (still a valid alternative). The
Qt5-vs-Qt6 and deployment-target findings still apply; the "minos 14.0" checks there reflect the
original build (current builds are minos 13.0).

---

## 2. Bugs / problems encountered and how they were fixed

### Bug A — Master requires Qt6, not Qt5 (starting assumption was wrong)
- **Symptom:** Building against Homebrew `qt@5` (Qt 5.15.19) failed to compile with
  `use of undeclared identifier 'qHashMulti'` (`src/math/rational.h:61`),
  `QColorSpace::NamedColorSpace` mismatch (`src/main.cpp:232`), and
  `QList<HistoryEntry>::remove(int,int)` not found (`src/core/session.cpp:104`).
- **Cause:** Current `master` uses Qt6-only APIs. The wiki page "BuildingOSXPackage"
  is stale (2018, Qt5/0.12). `find_package(Qt6 …)` in `src/CMakeLists.txt` confirms Qt6.
- **Fix:** Installed Homebrew `qt` (Qt 6.11.2) and built against it. (`qt@5` had to be
  `brew unlink`ed first because it conflicts with `qt`.)

### Bug B — qmake path sets deployment target 10.8, too low for std::filesystem
- **Symptom (qmake attempt only):** `'path' is unavailable: introduced in macOS 10.15`
  etc. in `src/core/settings.cpp`.
- **Cause:** `src/speedcrunch.pro` hard-codes `QMAKE_MACOSX_DEPLOYMENT_TARGET = 10.8`,
  but the code uses `std::filesystem` (needs 10.15+). Irrelevant once we moved to the
  CMake path, but worth reporting upstream.
- **Note:** We used the CMake build, not qmake, so this did not affect the final artifact.

### Bug C — Accidental macOS 15.5 minimum
- **Symptom:** First CMake build produced a binary with `minos 15.5` (it inherited the
  build SDK version), needlessly excluding macOS 14 users.
- **Fix:** Reconfigured with `-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0` (Qt6's own floor).
  Final binary is `minos 14.0`.

### Bug D — **macdeployqt never ran (upstream CMake defect); dmg was not self-contained**
- **Symptom:** `make install` / `make package` produced a tiny 3.2 MB dmg whose app had
  **no `Contents/Frameworks`** and still linked to `/opt/homebrew/...` Qt — it would fail
  to launch on any Mac without Homebrew Qt6.
- **Cause:** `src/cmake/MacdeployQt.cmake` builds the command from
  `${_qt6Core_install_prefix}`, a private CMake variable that is **empty under Qt6**, so
  the command degraded to `/bin/macdeployqt` (nonexistent) and failed silently (its
  `EXECUTE_PROCESS` has no error checking; output went to an ignored log file).
- **Fix:** Ran macdeployqt manually against the built bundle:
  `"$(brew --prefix qt)/bin/macdeployqt" SpeedCrunch.app -verbose=1 -always-overwrite`.
- **→ Worth reporting upstream** (the Qt6 macOS deploy is broken for anyone building on a Mac).

### Bug E — macdeployqt pulled in unneeded plugins with missing framework deps + bloat
- **Symptom:** macdeployqt copied plugins whose framework dependencies it did not follow,
  leaving `QtSvg`, `QtPdf`, `QtVirtualKeyboard`, `QtVirtualKeyboardQml` unresolved, and it
  dragged the entire QtQml/QtQuick stack into the bundle (~90 MB) via the virtual-keyboard
  plugin. It also aborted its final ad-hoc signing step with a codesign error, leaving the
  bundle in a partially-signed state.
- **Analysis:** A desktop calculator needs none of these. The single `.svg` in the repo
  (`gfx/speedcrunch.svg`) is the icon *source*, not referenced by any `.qrc`, so SVG
  support is unused at runtime.
- **Fix (packaging step, no source change):** removed the unneeded plugins and the
  now-orphaned frameworks:
  - Plugins: `platforminputcontexts/libqtvirtualkeyboardplugin.dylib`,
    `iconengines/libqsvgicon.dylib`, `imageformats/libqpdf.dylib`, `imageformats/libqsvg.dylib`
  - Frameworks: `QtQml`, `QtQmlMeta`, `QtQmlModels`, `QtQmlWorkerScript`, `QtQuick`
  - Bundle shrank 90 MB → 79 MB; no plugin has an unresolved dependency afterwards.

### Bug F — Stray Homebrew rpath left on the main binary
- **Symptom:** `otool -l` showed an `LC_RPATH` of `/opt/homebrew/opt/qt/lib` on the main
  executable.
- **Fix:** `install_name_tool -delete_rpath /opt/homebrew/opt/qt/lib …`. (All Qt deps on
  the main binary use `@executable_path/../Frameworks`, so no rpath is needed.)

### Fix G — Re-signed the bundle ad-hoc
- Because macdeployqt and `install_name_tool` invalidated signatures, the whole bundle was
  re-signed ad-hoc: `codesign --force --deep --sign - --timestamp=none SpeedCrunch.app`.
  Verified with `codesign --verify --deep --strict` → *valid on disk, satisfies its
  Designated Requirement*.

### Fix H — GPL: license file was missing on macOS
- **Finding:** On macOS the CMake install does **not** bundle a license file
  (`COPYING.rtf` is installed only on WIN32). The app bundle contained no LICENSE/COPYING.
- **Fix:** Embedded the upstream GPLv2 `LICENSE` file **inside the app bundle** at
  `Contents/Resources/LICENSE`, then re-signed. This keeps the license with the binary
  (GPL conveyance) even after the app is copied out, while allowing the dmg to keep the
  minimal official-0.12 layout (see below).

### Fix I — Low-resolution / non-Retina app icon (upstream bug)
- **Finding:** The committed `src/resources/speedcrunch.icns` contains only legacy types up
  to **128×128** (`is32`/`il32`/`ih32`/`it32`), with no Retina/@2x or high-res (256/512/1024)
  representations, so the icon renders blurry at large sizes and on Retina — ironic for a
  Retina-focused build. Root cause is upstream `gfx/generate-icons.sh`, which builds the
  icns at only sizes 16/32/48/128 via `png2icns`. The icon *design* is correct: it is
  upstream's current official icon (commits `f6c55846`, `999dd781`); speedcrunch.org still
  shows the old 0.12 icon.
- **Fix:** Regenerated the icns from the **same** master SVG (`gfx/speedcrunch.svg`) using
  `rsvg-convert` + macOS `iconutil`, rendering the full ladder 16→1024 incl. all @2x
  variants (`ic04/ic05/ic07..ic14`). Same design, sharp on Retina. The regenerated icns
  replaces `src/resources/speedcrunch.icns` in the source tree (committed on the
  `macos-arm64-classic-appearance` branch of the clone), so builds embed it directly.
- **→ Worth reporting upstream** (extend `generate-icons.sh` to emit high-res/@2x icns types).

### Design — dmg layout matches the official 0.12 release
- The dmg contains only `Applications` (symlink) + `SpeedCrunch.app`, matching the simple
  official 0.12 macOS bundle. The earlier top-level `LICENSE` and read-me were removed;
  the license now lives inside the app bundle (Fix H). End-user Gatekeeper instructions
  live in `dist/How to open SpeedCrunch (read me).txt` and `HANDOFF.md` for the maintainer,
  not inside the dmg.

---

## 3. Verification of the final artifact

Checked on the app **inside the delivered dmg** (mounted, then copied out and launched):
- `lipo -archs` → **arm64** ✓
- `minos` → **14.0** ✓
- `Info.plist` contains `NSHighResolutionCapable` ✓
- `CFBundleShortVersionString` → **1.0** ✓
- `codesign --verify --deep --strict` → valid (ad-hoc) ✓
- External-dependency audit (every Mach-O, dependencies excluding own id): **no** refs to
  `/opt`, `/usr/local`, `/Users`, `/Cellar` — only `@executable_path`/`@loader_path`/`@rpath`
  and system `/System`, `/usr/lib` ✓ → fully self-contained
- **Launch test:** app starts, event loop runs, QtNetwork + TLS work (completed an update
  check), no dyld or plugin-load errors ✓

---

## 4. Gatekeeper (unsigned/unnotarised) — how end users open it

Because there is no Apple Developer ID signature or notarisation, Gatekeeper blocks the
first launch. End users open it once via **right-click → Open → Open**, or by clearing the
quarantine attribute: `xattr -dr com.apple.quarantine /Applications/SpeedCrunch.app`.
Full instructions ship in the dmg as *"How to open SpeedCrunch (read me).txt"*.

Signing + notarisation can be added later without changing anything above: obtain a
Developer ID, then `codesign --force --options runtime --sign "Developer ID Application: …"`
the bundle, `codesign`-verify, `xcrun notarytool submit` the dmg, and `xcrun stapler staple`.

---

## 5. Files in this project

- `speedcrunch/` — upstream clone at commit `b598d91d…` (unmodified source).
- `speedcrunch/build-cmake/` — out-of-source build tree (contains the finished `SpeedCrunch.app`).
- `dist/SpeedCrunch-1.0-591-gb598d91d-macOS-arm64.dmg` — the deliverable.
- `dist/How to open SpeedCrunch (read me).txt` — end-user Gatekeeper instructions.
- The full reproducible method is documented step-by-step in §1b (official Qt via aqtinstall,
  per-arch configure + `lipo` thinning, plugin/SQL-driver trim, ad-hoc sign, dependency check).
  It is automated by a single script `build_speedcrunch_macos.sh <arm64|x86_64>` in the author's
  build project.
- `research/2026-09-18_speedcrunch_macos_build.md` — upstream build-doc research.
- `HANDOFF.md` — concise note for the upstream maintainer.
