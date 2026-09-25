# OpenAxis navigation in the Rotatrix KiCad fork

`rotatrix/stable` follows upstream KiCad's `10.0` branch (latest released version
at integration start: 10.0.6). OpenAxis is optional, enabled with
`-DKICAD_OPENAXIS=ON`. Builds fetch SDK release `cpp/v1.0.0-rc.1`, pinned to
`acc4da095cde6747556245b4b6c110c16b968b6b`. For SDK development, set
`-DOPENAXIS_SOURCE_DIR=C:/path/to/openaxis`.

The integration follows the local PrusaSlicer `backport-2.9.6` adapter and wx
scheduler pattern. The SDK handles transport, reconnects, gesture policy and
native-input reconciliation. No device mappings live in KiCad.

## Navigation

Start Rotatrix, then open a board's 3D viewer, schematic editor or PCB editor.
Connections advertise `app.kicad`, the relevant workspace and `navigation`.
The 3D viewer advertises `viewspace.3d`; schematic and PCB canvases advertise
`viewspace.2d`, allowing the server to select fixed-orientation pan/zoom.
Default server bindings can be customized in Rotatrix.

The viewer supplies perspective or orthographic camera poses, board bounds,
cursor position and world orientation. It uses KiCad's existing 3D mouse pivot
marker. Surface picking and selection bounds are currently unavailable; server
pivot fallback applies. The 2D adapter maps KiCad's downward Y axis to a
right-handed Y-up camera, including mirrored PCB views. Both adapters return
the realized native pose after zoom and position constraints.

UI-thread idle events reconcile native mouse changes and publish focus. SDK
callbacks use wx deferred dispatch and one-shot deadline timers. No repeating
integration timer is used. Before every dispatch, focus and context are checked;
modal dialogs, inactive windows, document replacement, resize and DPI changes
cancel obsolete gestures. Closing a canvas stops its connection before its
rendering resources are destroyed. SDK session logs use the `kicad` identity.

## Windows x64 build and packaging

Install Visual Studio 2022 with Desktop development with C++ and CMake tools.
Run from Windows PowerShell 5.1:

```powershell
./tools/build-openaxis-windows.ps1 -Stage All
```

Stages `Configure`, `Build` and `Package` can be run separately. `-BuilderPath`
reuses a dedicated builder checkout, `-SdkPath` overrides the SDK, and `-Jobs`
controls build concurrency. The wrapper pins KiCad's official Windows builder
to `81d736abb12384f85d4453553246d43002d0ff45` and uses its pinned vcpkg toolchain,
environment setup, runtime dependency staging, Python bundling and NSIS scripts.
It builds this checkout directly without the builder's source-reset operation.

The output is an unsigned x64 Lite installer under the builder's `.out` folder.
Lite omits symbol, footprint and 3D model libraries; use KiCad's online library
installation facilities. The installed application tree is also under `.out`.
The initial dependency build may take several hours if KiCad's public binary
cache has no matching packages. Subsequent builds reuse the local vcpkg cache.

GitHub Actions runs the same wrapper on `windows-2022`, caches dependencies,
uploads the installer and SHA-256 hashes, and creates a **draft** release only
after packaging succeeds. Failed builds upload their logs. Hardware acceptance
is required before publishing a release.

The separate Linux and macOS workflow runs concurrently with Windows. Linux
uses KiCad's Fedora 41 CI image and uploads a staged x64 installation archive
that requires the distribution's shared dependencies. macOS builds for Apple
Silicon with the official macOS builder pinned to
`370400dc2e5cbfcd20b762c02f45730755516e87`, then uploads an ad-hoc signed app
bundle. These are test artifacts, without bundled content libraries; the macOS
bundle is not notarized. Both jobs run the OpenAxis regressions and a CLI smoke
test before uploading binaries.

## Licensing and provenance

OpenAxis's GPL-3.0-only license option applies to these combined binaries.
Existing KiCad source retains its notices. The SDK's complete license is
installed alongside this document. The exact source commit accompanies CI
artifacts; the SDK and builder commits are pinned in this repository.

## Native acceptance

The normal Windows build runs `openaxis_native_camera` and
`openaxis_wx_scheduler` before installation.
These regression checks use KiCad's actual CAMERA and TRACK_BALL classes to
verify camera round trips, native mouse continuation, orthographic zoom limits,
reversal at a limit and all four 2D mirroring combinations. They can also be
built separately with `cmake -S qa/openaxis -B build/openaxis-checks`, supplying
`OPENAXIS_SOURCE_DIR` and a wxWidgets installation.
The scheduler checks cover cross-thread dispatch, deadline ordering and shutdown
with queued callbacks. A packaged `kicad-cli version` smoke test runs after NSIS.

Verify orbit, pan and zoom in both viewer projections; switch back to the mouse
after SDK motion and confirm continuity. Check minimum/maximum zoom, resize and
DPI changes, document reload, view animations, foreground-window changes, modal
dialogs, reconnect, and closing the application during a gesture. In schematic
and PCB editors, check cursor-anchored zoom, pan, sheet switching, mirrored PCB
views and switching between multiple editor windows. Confirm that 2D views never
tilt or roll and that only the foreground window accepts navigation.
