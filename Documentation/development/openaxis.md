# OpenAxis navigation

This unofficial KiCad build adds optional OpenAxis navigation. It preserves KiCad's
application identity, preferences, mouse controls and 3Dconnexion support.
Enable it at build time with `-DKICAD_OPENAXIS=ON`.

## Navigation and diagnostics

Start Rotatrix and open KiCad. OpenAxis connects automatically and follows the
foreground editor. One service in `kicommon` owns the connection for every canvas
in the process; separate KiCad processes have separate connections.

- The board 3D viewer supports orbit, pan and zoom in both projections, with
  surface picks, scene and selection bounds. A green disc with a black rim shows
  the pivot chosen by Rotatrix.
- Schematic, PCB, symbol, footprint, Gerber and drawing-sheet canvases support
  fixed-orientation 2D pan and zoom, including symbol and footprint viewers.
- **View > Show OpenAxis Diagnostics** draws connection status, query facts and
  camera evidence directly in the viewport. It is shared by windows in the
  process, starts off, and supports KiCad's usual hotkey configuration. Normal
  pivots remain visible independently of diagnostics.

Configure bindings and navigation policy in Rotatrix. SDK session logs remain
independent of overlay visibility. Files begin with `kicad-` in:

| Platform | Directory |
| --- | --- |
| Windows | `%LOCALAPPDATA%/Rotatrix/logs` |
| macOS | `~/Library/Application Support/Rotatrix/logs` |
| Linux | `$XDG_DATA_HOME/Rotatrix/logs`, default `~/.local/share/Rotatrix/logs` |

`ROTATRIX_LOG_DIR` overrides the directory. Flatpak uses its application data directory.

### Scope

Free camera and object manipulation are excluded. Model-preview dialogs use a
separate camera and are not navigated. Selection-filtered picks are deferred;
ordinary surface picks and selection bounds are available. The 2D pivot is not
drawn. Raytracing supplies no scene depth buffer, so its pivot is opaque; OpenGL
shows occluded fragments faintly. Overlays do not enter selection, extents, undo
or saved documents.

Focus, modal dialogs, document replacement, resize and DPI changes invalidate
obsolete navigation. Native mouse changes are reconciled on UI idle and before
scheduled SDK work. The last canvas stops the shared service before destroying
its resources. Automated tests do not establish physical-device or visual acceptance.

## Build and test

The source baseline is upstream `10.0.6`. OpenAxis is pinned to public release
`cpp/v1.0.0-rc.1`, commit `acc4da095cde6747556245b4b6c110c16b968b6b`.
Set `OPENAXIS_SOURCE_DIR` to a local SDK checkout for development.

On Windows, install Visual Studio 2022 and run Windows PowerShell 5.1:

```powershell
./tools/build-openaxis-windows.ps1 -Stage All
```

Stages `Configure`, `Check`, `Build` and `Package` are separately invocable.
`-BuilderPath` reuses a builder checkout, `-SdkPath` overrides the SDK, and `-Jobs`
controls concurrency. The official Windows builder is pinned to
`81d736abb12384f85d4453553246d43002d0ff45`; its vcpkg environment, Python/runtime
bundling and NSIS scripts are reused without resetting this checkout. The unsigned
x64 Lite installer is under the builder's `.out`. Install content libraries using
KiCad's library facilities.

The platform scripts in `tools/` document macOS and Linux prerequisites and stages.
They reuse pinned upstream packaging recipes. macOS produces `KiCad.app` in one
`.tar.gz`; Linux preserves the upstream Flatpak application ID and runtime.
Installing that Flatpak may replace an upstream installation of the same ID;
native preferences use a separate location.

`openaxis_native_camera` and `openaxis_wx_scheduler` cover camera round trips,
world orientation, mirrored 2D views, zoom constraints, native-input continuation,
deferred dispatch and shutdown. Standalone checks build from `qa/openaxis` with
`OPENAXIS_SOURCE_DIR` and a wxWidgets development installation. Full builds also
run package startup checks. Exercise device movement, pivots, diagnostics, native
mouse continuation, editor switching and cleanup interactively on each target OS.

## CI and release preparation

Develop and contribute on `rotatrix/work/10.0.6`. Pushes, PR updates and tags do
not start builds automatically. Manually dispatch **Build KiCad**, supplying the
full source SHA at the selected work-branch revision; the workflow verifies it.
The full matrix is Windows x64, Linux x64, macOS arm64 and macOS x86_64.
SHA-named package artifacts expire after 14 days; logs remain separate.

After automated package checks pass, create an immutable
`10.0.6-rotatrix.N[-beta.M]` tag at that commit. **Prepare release draft** accepts
that tag and the successful full-platform build run ID. It checks the selection,
requests central macOS signing/notarization and assembles the existing Windows
and Linux packages, signed Mac packages and `SHA256SUMS` into a draft. It does not
rebuild or publish. Test those exact final packages before separately publishing.
Windows signing is not currently available from the central service.

macOS runtime exceptions are declared by bundle ID in
`signing/macos-entitlements.plist`. Only the five GUI applications that load the
external 3Dconnexion framework request disabled library validation. Utilities and
bundled libraries receive no such exception. The signer must support source-owned
bundle maps before preparing a draft; this does not require a KiCad-specific
central entitlement profile.

## Licensing and source

OpenAxis's GPL-3.0-only option applies to combined binaries; existing KiCad source
retains its notices. SDK and IXWebSocket licenses are installed with this document.
Application source is retained at immutable release tags. Dependency versions,
source URLs and patches remain in Git and the pinned platform recipes.
