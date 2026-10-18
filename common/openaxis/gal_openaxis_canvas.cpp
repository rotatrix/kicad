// SPDX-License-Identifier: GPL-3.0-or-later
// 2D pan/zoom for GAL editor canvases (viewspace.2d).

#include <openaxis/kicad_openaxis.h>
#include <openaxis/camera.h>
#include <class_draw_panel_gal.h>
#include <eda_draw_frame.h>
#include <base_screen.h>
#include <tool/selection.h>
#include <view/view.h>
#include <gal/graphics_abstraction_layer.h>
#include <wx/tokenzr.h>
#include <font/font.h>
#include <font/glyph.h>
#include <wx/toplevel.h>
#include <cmath>
#include <memory>

namespace
{
const char* Workspace( FRAME_T aType )
{
    switch( aType )
    {
    case FRAME_SCH:               return "workspace.schematic";
    case FRAME_SCH_SYMBOL_EDITOR:
    case FRAME_SCH_VIEWER:        return "workspace.symbol";
    case FRAME_PCB_EDITOR:        return "workspace.pcb";
    case FRAME_FOOTPRINT_EDITOR:
    case FRAME_FOOTPRINT_VIEWER:  return "workspace.footprint";
    case FRAME_GERBER:            return "workspace.gerber";
    case FRAME_PL_EDITOR:         return "workspace.drawing_sheet";
    default:                      return nullptr;
    }
}


std::string Address( const void* aPointer )
{
    return std::to_string( reinterpret_cast<std::uintptr_t>( aPointer ) );
}


class GAL_OPENAXIS_CANVAS final : public OPENAXIS_CANVAS
{
public:
    GAL_OPENAXIS_CANVAS( EDA_DRAW_PANEL_GAL& aPanel, EDA_DRAW_FRAME& aFrame, const char* aWorkspace ) :
            m_panel( aPanel ), m_frame( aFrame ), m_workspace( aWorkspace )
    {
    }

    wxWindow* Window() override { return &m_panel; }

    std::string Target() override
    {
        KIGFX::VIEW* view = m_panel.GetView();
        if( !view || !m_frame.GetScreen() )
            return {};
        return Address( m_frame.GetScreen() ) + "/" + Address( view ) + "/"
               + std::to_string( view->GetContentRevision() ) + "/"
               + std::to_string( view->IsMirroredX() ) + std::to_string( view->IsMirroredY() );
    }

    std::string DocumentId() override
    {
        return m_frame.GetScreen() ? m_workspace + "/" + Address( m_frame.GetScreen() ) : "";
    }

    std::vector<std::string> Tags() override { return { "app.kicad", m_workspace, "viewspace.2d" }; }

    std::optional<openaxis::Pose> ReadCamera() override
    {
        KIGFX::VIEW* view = m_panel.GetView();
        if( !view || m_panel.GetClientSize().y <= 0 )
            return {};
        const VECTOR2D center = view->GetCenter();
        const double   extent = view->ToWorld( double( view->GetScreenPixelSize().y ) );
        if( !std::isfinite( extent ) || extent <= 0 )
            return {};
        // KiCad drawing Y points down. Publish a right-handed camera in Y-up space.
        return KICAD_OPENAXIS_CAMERA::Read2D( center.x, center.y, extent, view->IsMirroredX(),
                                              view->IsMirroredY(), m_depth );
    }

    bool WriteCamera( const openaxis::Pose& aPose ) override
    {
        KIGFX::VIEW* view = m_panel.GetView();
        if( !view || !std::isfinite( aPose.ortho_extent ) || aPose.ortho_extent <= 0 )
            return false;
        const double extent = view->ToWorld( double( view->GetScreenPixelSize().y ) );
        view->SetScale( view->GetScale() * extent / aPose.ortho_extent );
        view->SetCenter( { aPose.t.x, -aPose.t.y } );
        // Depth has no 2D meaning; echo it so the server's camera stays consistent.
        m_depth = aPose.t.z;
        m_panel.Refresh();
        return true;
    }

    // Read2D publishes drawing coordinates in a right-handed, Y-up XY plane.
    // Orbit navigation requires this fact even when viewspace.2d locks rotation.
    std::array<openaxis::Vec3, 2> WorldOrientation() override
    {
        return { openaxis::Vec3{ 0, 1, 0 }, openaxis::Vec3{ 0, 0, -1 } };
    }

    std::optional<OPENAXIS_BOX> ModelBounds() override
    {
        return Box( m_frame.GetDocumentExtents() );
    }

    std::optional<OPENAXIS_BOX> SelectionBounds() override
    {
        const SELECTION& selection = m_frame.GetCurrentSelection();
        return selection.Empty() ? std::nullopt : Box( selection.GetBoundingBox() );
    }

    void DiagnosticsChanged() override
    {
        m_panel.GetView()->MarkTargetDirty( KIGFX::TARGET_OVERLAY );
        m_panel.Refresh();
    }

    void DrawDiagnostics() override
    {
        auto evidence = KIOPENAXIS::Diagnostics( this );
        if( !evidence )
            return;

        KIGFX::GAL& gal = *m_panel.GetGAL();
        KIGFX::VIEW& view = *m_panel.GetView();
        KIGFX::GAL_SCOPED_ATTRS attrs( gal, KIGFX::GAL_SCOPED_ATTRS::ALL_ATTRS );
        const auto target = gal.GetTarget();
        gal.SetTarget( KIGFX::TARGET_OVERLAY );
        gal.SetIsFill( false );
        gal.SetIsStroke( true );
        const double dpi = double( view.GetScreenPixelSize().y )
                           / std::max( 1, m_panel.GetClientSize().y );
        auto color = []( const std::array<int, 3>& rgb, double alpha )
        {
            return KIGFX::COLOR4D( rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0, alpha );
        };
        // Direct overlay primitives never enter the model, spatial index or undo.
        for( const auto& segment : evidence->segments )
        {
            gal.SetStrokeColor( color( segment.color, segment.opacity ) );
            gal.SetLineWidth( view.ToWorld( segment.width * dpi ) );
            gal.DrawLine( { segment.start.x, -segment.start.y },
                          { segment.end.x, -segment.end.y } );
        }

        gal.Save();
        // GAL::Transform changes the immediate GL matrix, whereas Save/Restore
        // and queued primitives use the vertex manager's matrix stack. Express
        // the inverse camera with the common queued transform operations instead.
        const auto& inverse = gal.GetScreenWorldMatrix().m_data;
        const double scaleX = std::hypot( inverse[0][0], inverse[1][0] );
        const double scaleY = ( inverse[0][0] * inverse[1][1]
                                - inverse[0][1] * inverse[1][0] ) / scaleX;
        gal.Translate( { inverse[0][2], inverse[1][2] } );
        gal.Rotate( std::atan2( inverse[1][0], inverse[0][0] ) );
        gal.Scale( { scaleX, scaleY } );
        // Cancel the live camera, including board mirroring, then convert
        // logical display dimensions exactly once.
        gal.Scale( { dpi, dpi } );
        // OpenGL's line shader consumes world-unit widths independently of the
        // vertex-manager transform; Cairo transforms widths with its geometry.
        const double screenStroke = m_panel.GetBackend() == EDA_DRAW_PANEL_GAL::GAL_TYPE_OPENGL
                                            ? view.ToWorld( dpi ) : 1.0;
        auto text = [&]( const std::string& value, VECTOR2I position,
                         const std::array<int, 3>& rgb )
        {
            const wxString label = wxString::FromUTF8( value.c_str() );
            std::vector<std::unique_ptr<KIFONT::GLYPH>> glyphs;
            BOX2I bounds;
            KIFONT::FONT::GetFont()->GetTextAsGlyphs( &bounds, &glyphs, label,
                    { 12, 12 }, position, ANGLE_0, false, { 0, 0 }, 0 );
            gal.SetIsFill( true );
            gal.SetIsStroke( false );
            gal.SetFillColor( KIGFX::COLOR4D( 0.03, 0.03, 0.03, 0.85 ) );
            gal.DrawRectangle( VECTOR2D( bounds.GetLeft() - 2, bounds.GetTop() - 2 ),
                               VECTOR2D( bounds.GetRight() + 2, bounds.GetBottom() + 2 ) );
            gal.SetIsStroke( true );
            gal.SetIsFill( false );
            gal.SetStrokeColor( color( rgb, 1.0 ) );
            gal.SetFillColor( color( rgb, 1.0 ) );
            gal.SetLineWidth( screenStroke );
            // Keep font metrics in logical pixels and line thickness separate:
            // the GL shader consumes world-unit widths, not glyph-space widths.
            for( const auto& glyph : glyphs )
                if( glyph->IsStroke() )
                    for( const auto& stroke : *static_cast<KIFONT::STROKE_GLYPH*>( glyph.get() ) )
                        for( size_t i = 1; i < stroke.size(); ++i )
                            gal.DrawLine( stroke[i - 1], stroke[i] );
        };
        int y = 24;
        for( const auto& line : evidence->lines )
        {
            text( line.text, { 12, y }, line.color );
            y += 15;
        }
        for( const auto& marker : evidence->markers )
        {
            const VECTOR2D point( marker.point[0], marker.point[1] );
            gal.SetStrokeColor( color( marker.color, 0.65 ) );
            gal.SetLineWidth( 2 * screenStroke );
            gal.DrawLine( point - VECTOR2D( 9, 0 ), point + VECTOR2D( 9, 0 ) );
            gal.DrawLine( point - VECTOR2D( 0, 9 ), point + VECTOR2D( 0, 9 ) );
            wxStringTokenizer labels( wxString::FromUTF8( marker.label.c_str() ), "\n" );
            int offset = -8;
            while( labels.HasMoreTokens() )
            {
                text( labels.GetNextToken().ToStdString(),
                      { int( point.x ) + 12, int( point.y ) + offset }, marker.color );
                offset += 15;
            }
        }
        gal.Restore();
        gal.SetTarget( target );
    }

private:
    // Drawing-plane bounds at z = 0, converted to the published Y-up space.
    static std::optional<OPENAXIS_BOX> Box( BOX2I aBox )
    {
        aBox.Normalize();
        if( aBox.GetWidth() <= 0 && aBox.GetHeight() <= 0 )
            return {};
        return OPENAXIS_BOX{ { double( aBox.GetLeft() ), -double( aBox.GetBottom() ), 0 },
                             { double( aBox.GetRight() ), -double( aBox.GetTop() ), 0 } };
    }

    EDA_DRAW_PANEL_GAL& m_panel;
    EDA_DRAW_FRAME&     m_frame;
    std::string         m_workspace;
    double              m_depth = 0;
};
} // namespace


std::unique_ptr<OPENAXIS_CANVAS> CreateOpenAxisCanvas( EDA_DRAW_PANEL_GAL& aPanel )
{
    EDA_DRAW_FRAME* frame = aPanel.GetParentEDAFrame();
    const char*     workspace = frame ? Workspace( frame->GetFrameType() ) : nullptr;

    // Only an editor's own canvas: preview panels in dialogs report the parent
    // frame's type but are not that editor's view.
    if( !workspace || !aPanel.GetView() || wxGetTopLevelParent( &aPanel ) != frame )
        return nullptr;

    return std::make_unique<GAL_OPENAXIS_CANVAS>( aPanel, *frame, workspace );
}
