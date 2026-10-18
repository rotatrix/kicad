# Linux Flatpak builds

This adapter reuses the KiCad **10.0.6** Flathub recipe at
[`279821a1e7a0c0e5831f3b8f7cb410e9734f8087`](https://github.com/flathub/org.kicad.KiCad/tree/279821a1e7a0c0e5831f3b8f7cb410e9734f8087),
including its pinned dependencies, wxPython, ngspice, STEP support, documentation,
library extensions and sandbox permissions. The fork source and its pinned
OpenAxis SDK replace only the application source inputs. Translations remain
inside the app rather than requiring a separate Locale download. The recipe
disables upstream QA execution for 10.0.6; this is not a claim that QA passed.

Install `flatpak`, `flatpak-builder`, `ostree`, `xvfb`, `xauth` and Git through your distribution, configure
Flathub, and install `org.gnome.Sdk//50`. Run from the KiCad checkout, supplying the
full source SHA that is available from the source URL:

```sh
sha=$(git rev-parse HEAD)
python3 tools/flatpak/build.py prepare --source-sha "$sha"
python3 tools/flatpak/build.py deps --source-sha "$sha"
xvfb-run -a python3 tools/flatpak/build.py build --source-sha "$sha"
python3 tools/flatpak/build.py smoke --source-sha "$sha"
python3 tools/flatpak/build.py bundle --source-sha "$sha"
python3 tools/flatpak/build.py package-smoke --source-sha "$sha"
```

`deps` stops before the OpenAxis regression module or compiling KiCad, allowing dependency preparation while source
work continues. All stages reuse `build-flatpak/`; cache `build-flatpak/state/`
in CI. `--jobs` defaults to 2. `--cache` selects an independent build tree.
The adapter explicitly sets the dependency library directory to `/app/lib`.
Older Flatpak-builder versions otherwise let Meson select `/app/lib64` in this
SDK, conflicting with upstream's GLU link path and `/lib64` cleanup. Changing
this option invalidates affected dependency cache entries; do not reuse the old
library layout by copying libraries or adding symlinks.
`--source-url` can point at a local Git checkout for committed local testing.
Uncommitted application edits are never silently included. The SDK is downloaded
by Flatpak before the network-isolated compile stage. Do not run stages
concurrently against one cache. `--force-clean` recreates only the sandbox app
directory; the dependency and compiler caches persist.

On hosts where AppArmor or another host policy prevents `fusermount3` from
accessing the build directory, pass `--disable-rofiles-fuse` to `deps` and `build`.
This selects Flatpak's supported non-FUSE cache checkout without changing host
permissions or disabling security policy. Dependency and compiler caches remain
enabled.

The output is `build-flatpak/artifacts/KiCad-linux-x64-<full-sha>.flatpak`.
Upload this file directly (without an enclosing ZIP). Keep build logs separate.
The GNOME 50 SDK is also the runtime, following the pinned upstream recipe.

## Installation and testing

The app keeps the upstream ID `org.kicad.KiCad` and stable branch. Installing it
can replace upstream KiCad in the same Flatpak installation and uses the same
Flatpak preferences. Native KiCad configuration is not automatically shared.
Do not replace a running test instance. Install runtime and component libraries
from Flathub in the same installation as the app:

```sh
flatpak install --user flathub org.gnome.Sdk//50 \
  org.kicad.KiCad.Library.Symbols//stable \
  org.kicad.KiCad.Library.Footprints//stable \
  org.kicad.KiCad.Library.Templates//stable \
  org.kicad.KiCad.Library.Packages3D//stable
flatpak install --user ./KiCad-linux-x64-<full-sha>.flatpak
flatpak run org.kicad.KiCad
```

Library extensions are separate upstream downloads, not included in the app
bundle. Flathub must be configured for `--user` when using these commands.
For automated checking before installation, the `smoke` stage runs the CLI and
imports wxPython/pcbnew directly in the built sandbox. Also test the exported
bundle in a separate Flatpak installation before accepting a distribution build.
Manual checks cover schematic/PCB editing, 3D models and STEP import/export,
simulation, Python plugins, library availability, and OpenAxis navigation and
diagnostics. Existing network permission permits OpenAxis's localhost connection.

The build runs OpenAxis camera/scheduler tests in a separate sandbox module before
compiling the application. Start it under `xvfb-run -a` on headless CI. The checks
configure the full project with the same upstream options and pinned SDK. The
`package-smoke` stage imports the exported `.flatpak` into a temporary OSTree
repository, extracts it, checks core executables and runs CLI/Python checks using
the declared runtime. It does not replace an installed KiCad. This validates the
exported bytes; GUI, component-library and physical-device acceptance remain
separate checks.
