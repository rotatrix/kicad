// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// KiCad's process-wide OpenAxis service. The SDK and all of its state live in
// kicommon; canvases in the editor modules implement OPENAXIS_CANVAS using only
// the SDK's header-only geometry types, so no SDK JSON crosses module boundaries.

#include <kicommon.h>
#include <openaxis/geometry.hpp>
#include <array>
#include <optional>
#include <string>
#include <vector>

class wxWindow;

struct OPENAXIS_BOX
{
    openaxis::Vec3 min, max;
};

// A tested pick. An absent point is a miss; marker is the sampled viewport pixel.
struct OPENAXIS_PICK
{
    std::optional<openaxis::Vec3> point;
    std::optional<OPENAXIS_BOX>   bounds;
    std::array<double, 2>         marker{};
};

// Detached diagnostic evidence for one canvas, with SDK tones resolved to RGB.
struct OPENAXIS_DIAGNOSTICS
{
    struct LINE    { std::string text; std::array<int, 3> color; };
    struct SEGMENT { openaxis::Vec3 start, end; std::array<int, 3> color; double width, opacity; };
    struct MARKER  { std::string label; std::array<double, 2> point; std::array<int, 3> color; };

    std::vector<LINE>    lines;
    std::vector<SEGMENT> segments;
    std::vector<MARKER>  markers;
};

// Implemented by each navigable canvas. All calls run on the wx UI thread.
class OPENAXIS_CANVAS
{
public:
    virtual ~OPENAXIS_CANVAS() = default;

    virtual wxWindow* Window() = 0;

    /// Identity of the displayed target, including anything whose change must
    /// cancel a gesture (document, content revision, projection, mirroring).
    /// Empty when navigation is temporarily unavailable, e.g. during a reload.
    virtual std::string Target() = 0;

    /// Opaque identity that is stable while the document stays open.
    virtual std::string DocumentId() = 0;

    virtual std::vector<std::string> Tags() = 0;

    virtual std::optional<openaxis::Pose> ReadCamera() = 0;
    virtual bool WriteCamera( const openaxis::Pose& aPose ) = 0;

    /// World up and forward (semantic Front view direction); right-handed.
    virtual std::array<openaxis::Vec3, 2> WorldOrientation() = 0;

    /// Relative/absolute/angular tolerance for comparing camera readback.
    /// 0 uses the SDK defaults; single-precision cameras need more.
    virtual double ComparisonTolerance() { return 0; }

    virtual std::optional<OPENAXIS_BOX> ModelBounds() { return {}; }
    virtual std::optional<OPENAXIS_BOX> SelectionBounds() { return {}; }
    virtual std::optional<openaxis::Vec3> ViewTarget() { return {}; }

    /// Pick at the cursor or viewport center. Empty means the test did not run.
    virtual std::optional<OPENAXIS_PICK> Pick( bool aViewportCenter ) { return {}; }

    virtual void ShowPivot( std::optional<openaxis::Vec3> aPoint ) {}

    /// Diagnostic evidence changed or expired; repaint when visible.
    virtual void DiagnosticsChanged() {}

    /// Draw transient evidence during the native canvas render pass.
    virtual void DrawDiagnostics() {}
};

namespace KIOPENAXIS
{
/// Register a canvas once it can be painted. The first registration starts the
/// shared connection; the last unregistration stops it.
KICOMMON_API void Register( OPENAXIS_CANVAS* aCanvas );

/// Call at the start of the canvas destructor, while its native resources exist.
KICOMMON_API void Unregister( OPENAXIS_CANVAS* aCanvas );

/// Cancel any gesture bound to this canvas, e.g. when its document reloads.
KICOMMON_API void Invalidate( OPENAXIS_CANVAS* aCanvas );

KICOMMON_API bool DiagnosticsVisible();
KICOMMON_API void SetDiagnosticsVisible( bool aVisible );

/// Evidence to draw on this canvas, or empty when hidden or bound elsewhere.
KICOMMON_API std::optional<OPENAXIS_DIAGNOSTICS> Diagnostics( OPENAXIS_CANVAS* aCanvas );
}
