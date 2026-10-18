// SPDX-License-Identifier: GPL-3.0-or-later
// Orbit, pan and zoom for the board 3D viewer (viewspace.3d).

#include <kicad_gl/kiglad.h> // Must precede native GL headers.
#include "eda_3d_canvas.h"
#include <3d_rendering/raytracing/render_3d_raytrace_gl.h>
#include <3d_rendering/opengl/render_3d_opengl.h>
#include <font/font.h>
#include <font/glyph.h>
#include <glm/gtc/type_ptr.hpp>
#include <wx/utils.h>
#include <cmath>
#include <sstream>
#include <openaxis/kicad_openaxis.h>
#include <openaxis/camera.h>
#include <eda_3d_viewer_frame.h>
#include <board.h>
#include <memory>

class EDA_3D_OPENAXIS_CANVAS final : public OPENAXIS_CANVAS
{
public:
    explicit EDA_3D_OPENAXIS_CANVAS( EDA_3D_CANVAS& aCanvas ) : m_canvas( aCanvas ) {}

    wxWindow* Window() override { return &m_canvas; }

    // Unavailable while the scene reloads or a native camera animation runs.
    std::string Target() override
    {
        const BOARD* board = m_canvas.m_boardAdapter.GetBoard();
        if( !board || m_canvas.IsReloadRequestPending() || m_canvas.m_camera_is_moving )
            return {};
        return DocumentId() + "/" + std::to_string( m_canvas.GetProjectionMode() );
    }

    std::string DocumentId() override
    {
        const BOARD* board = m_canvas.m_boardAdapter.GetBoard();
        return board ? "board/" + std::to_string( reinterpret_cast<std::uintptr_t>( board ) ) : "";
    }

    std::vector<std::string> Tags() override
    {
        return { "app.kicad", "workspace.3dviewer", "viewspace.3d" };
    }

    std::optional<openaxis::Pose> ReadCamera() override
    {
        return KICAD_OPENAXIS_CAMERA::Read( m_canvas.m_camera );
    }

    bool WriteCamera( const openaxis::Pose& aPose ) override
    {
        if( !KICAD_OPENAXIS_CAMERA::Write( m_canvas.m_camera, aPose ) )
            return false;
        m_canvas.m_mouse_was_moved = true;
        m_canvas.restart_editingTimeOut_Timer();
        m_canvas.Request_refresh();
        return true;
    }

    // Board in the XY plane with +Z toward the top side.
    std::array<openaxis::Vec3, 2> WorldOrientation() override
    {
        return { openaxis::Vec3{ 0, 0, 1 }, openaxis::Vec3{ 0, 1, 0 } };
    }

    // CAMERA stores single-precision matrices.
    double ComparisonTolerance() override { return 1e-5; }

    std::optional<OPENAXIS_BOX> ModelBounds() override
    {
        const BBOX_3D& bounds = m_canvas.m_3d_render_raytracing->NavigationBounds();
        if( !bounds.IsInitialized() )
            return {};
        const SFVEC3F& lo = bounds.Min();
        const SFVEC3F& hi = bounds.Max();
        if( !( lo.x <= hi.x && lo.y <= hi.y && lo.z <= hi.z ) )
            return {};
        return OPENAXIS_BOX{ { lo.x, lo.y, lo.z }, { hi.x, hi.y, hi.z } };
    }

    std::optional<openaxis::Vec3> ViewTarget() override
    {
        const SFVEC3F& target = m_canvas.m_camera.GetLookAtPos();
        return openaxis::Vec3{ target.x, target.y, target.z };
    }

    std::optional<OPENAXIS_BOX> SelectionBounds() override
    {
        BBOX_3D bounds;
        if( !m_canvas.m_3d_render_raytracing->SelectedBounds( bounds ) )
            return {};
        return Box( bounds );
    }

    std::optional<OPENAXIS_PICK> Pick( bool aViewportCenter ) override
    {
        if( Target().empty() || !m_canvas.m_3d_render_raytracing->HasPickGeometry() )
            return {};
        const wxSize size = m_canvas.GetClientSize();
        if( size.x <= 0 || size.y <= 0 )
            return {};
        wxPoint cursor = m_canvas.ScreenToClient( wxGetMousePosition() );
        const double x = aViewportCenter ? size.x * 0.5 : cursor.x;
        const double y = aViewportCenter ? size.y * 0.5 : cursor.y;
        if( x < 0 || y < 0 || x >= size.x || y >= size.y )
            return {};

        OPENAXIS_PICK result;
        result.marker = { x, y }; // logical viewport pixels, top-left origin
        // Unproject the exact sample, avoiding CAMERA's mouse-position cache and
        // integer ray tables. The same path handles perspective and orthographic.
        const glm::mat4 inverse = m_canvas.m_camera.GetViewMatrix_Inv()
                                  * m_canvas.m_camera.GetProjectionMatrixInv();
        glm::vec4 nearPoint = inverse * glm::vec4( 2*x/size.x-1, 1-2*y/size.y, -1, 1 );
        glm::vec4 farPoint = inverse * glm::vec4( 2*x/size.x-1, 1-2*y/size.y, 1, 1 );
        nearPoint /= nearPoint.w;
        farPoint /= farPoint.w;
        RAY ray;
        ray.Init( glm::vec3( nearPoint ), glm::normalize( glm::vec3( farPoint-nearPoint ) ) );
        SFVEC3F point;
        BBOX_3D bounds;
        if( m_canvas.m_3d_render_raytracing->PickSurface( ray, point, bounds ) )
        {
            const glm::vec4 clip = m_canvas.m_camera.GetProjectionMatrix()
                                   * m_canvas.m_camera.GetViewMatrix() * glm::vec4( point, 1 );
            if( clip.w > 0 && clip.z >= -clip.w && clip.z <= clip.w )
            {
                result.point = openaxis::Vec3{ point.x, point.y, point.z };
                result.bounds = Box( bounds );
            }
        }
        return result; // Preserve tested misses for diagnostic sample evidence.
    }

    void ShowPivot( std::optional<openaxis::Vec3> aPoint ) override
    {
        if( aPoint )
            m_canvas.m_camera.SetLookAtPosKeepingView(
                    SFVEC3F( aPoint->x, aPoint->y, aPoint->z ) );
        // Keep the native orbit reference after the transient marker disappears.
        m_pivot = aPoint;
        m_canvas.Request_refresh();
    }

    void DiagnosticsChanged() override { m_canvas.Request_refresh(); }

    void Render();

private:
    static OPENAXIS_BOX Box( const BBOX_3D& aBox )
    {
        return { { aBox.Min().x, aBox.Min().y, aBox.Min().z },
                 { aBox.Max().x, aBox.Max().y, aBox.Max().z } };
    }
    EDA_3D_CANVAS& m_canvas;
    std::optional<openaxis::Vec3> m_pivot;
};


void EDA_3D_CANVAS::InitOpenAxis()
{
    // Only the board 3D viewer; model previews in dialogs are not navigated.
    if( m_openaxis || !dynamic_cast<EDA_3D_VIEWER_FRAME*>( wxGetTopLevelParent( this ) ) )
        return;

    m_openaxis = std::make_unique<EDA_3D_OPENAXIS_CANVAS>( *this );
    m_3d_render_opengl->m_openaxisOverlay = [this] { RenderOpenAxis(); };
    KIOPENAXIS::Register( m_openaxis.get() );
}


void EDA_3D_CANVAS::ReleaseOpenAxis()
{
    if( m_3d_render_opengl )
        m_3d_render_opengl->m_openaxisOverlay = {};
    if( m_openaxis )
        KIOPENAXIS::Unregister( m_openaxis.get() );
    m_openaxis.reset();
}

void EDA_3D_CANVAS::RenderOpenAxis()
{
    if( m_openaxis )
        static_cast<EDA_3D_OPENAXIS_CANVAS*>( m_openaxis.get() )->Render();
}


void EDA_3D_CANVAS::InvalidateOpenAxis()
{
    if( m_openaxis )
        KIOPENAXIS::Invalidate( m_openaxis.get() );
}

void EDA_3D_OPENAXIS_CANVAS::Render()
{
    const auto diagnostics = KIOPENAXIS::Diagnostics( this );
    if( !m_pivot && !diagnostics )
        return;
    const wxSize size = m_canvas.GetClientSize();
    const wxSize pixels = m_canvas.GetNativePixelSize();
    if( size.x <= 0 || size.y <= 0 )
        return;
    const double dpi = double( pixels.y ) / size.y;
    const glm::mat4 projection = m_canvas.m_camera.GetProjectionMatrix();
    const glm::mat4 view = m_canvas.m_camera.GetViewMatrix();

    // The upstream canvas is compatibility OpenGL. Preserve all state touched
    // here, including the matrix mode, depth mask and the caller's viewport.
    GLint matrixMode;
    glGetIntegerv( GL_MATRIX_MODE, &matrixMode );
    glPushAttrib( GL_ALL_ATTRIB_BITS );
    glViewport( 0, 0, pixels.x, pixels.y );
    glDisable( GL_LIGHTING );
    glDisable( GL_TEXTURE_2D );
    glDisable( GL_CULL_FACE );
    glDisable( GL_STENCIL_TEST );
    glDisable( GL_ALPHA_TEST );
    glDisable( GL_SCISSOR_TEST );
    glEnable( GL_BLEND );
    glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
    glDepthMask( GL_FALSE );
    glMatrixMode( GL_PROJECTION );
    glPushMatrix();
    glLoadMatrixf( glm::value_ptr( projection ) );
    glMatrixMode( GL_MODELVIEW );
    glPushMatrix();
    glLoadMatrixf( glm::value_ptr( view ) );

    // Diagnostic geometry is presentation evidence, not scene geometry. It
    // follows the live camera even after native mouse navigation.
    glDisable( GL_DEPTH_TEST );
    if( diagnostics )
        for( const auto& segment : diagnostics->segments )
        {
            glColor4d( segment.color[0]/255.0, segment.color[1]/255.0,
                       segment.color[2]/255.0, segment.opacity );
            glLineWidth( segment.width * dpi );
            glBegin( GL_LINES );
            glVertex3d( segment.start.x, segment.start.y, segment.start.z );
            glVertex3d( segment.end.x, segment.end.y, segment.end.z );
            glEnd();
        }

    // Screen coordinates keep marker dimensions constant in both projections.
    // Keep the projected NDC depth so the disc tests against each scene fragment.
    glMatrixMode( GL_PROJECTION );
    glLoadIdentity();
    glOrtho( 0, size.x, size.y, 0, -1, 1 );
    glMatrixMode( GL_MODELVIEW );
    glLoadIdentity();
    if( m_pivot )
    {
        glm::vec4 clip = projection * view
                         * glm::vec4( m_pivot->x, m_pivot->y, m_pivot->z, 1 );
        if( clip.w > 0 && std::abs( clip.x ) <= clip.w
            && std::abs( clip.y ) <= clip.w && std::abs( clip.z ) <= clip.w )
        {
            const double x = ( clip.x/clip.w + 1 ) * size.x/2;
            const double y = ( 1 - clip.y/clip.w ) * size.y/2;
            const double z = -clip.z/clip.w;
            auto disc = [&]( double opacity )
            {
                glColor4d( 0, 1, 0, opacity );
                glBegin( GL_TRIANGLE_FAN );
                glVertex3d( x, y, z );
                for( int i = 0; i <= 48; ++i )
                {
                    const double angle = i * 2 * M_PI / 48;
                    glVertex3d( x+4*std::cos(angle), y+4*std::sin(angle), z );
                }
                glEnd();
                // An annulus avoids blending a translucent green disc over black.
                glColor4d( 0, 0, 0, opacity );
                glBegin( GL_QUAD_STRIP );
                for( int i = 0; i <= 48; ++i )
                {
                    const double angle = i * 2 * M_PI / 48;
                    glVertex3d( x+4*std::cos(angle), y+4*std::sin(angle), z );
                    glVertex3d( x+5.5*std::cos(angle), y+5.5*std::sin(angle), z );
                }
                glEnd();
            };
            if( m_canvas.m_3d_render == m_canvas.m_3d_render_opengl )
            {
                glEnable( GL_DEPTH_TEST );
                glDepthFunc( GL_GREATER );
                disc( 0.23 );
                glDepthFunc( GL_LEQUAL );
                disc( 1 );
            }
            else
            {
                // Raytracing blits a color image without a scene depth buffer.
                glDisable( GL_DEPTH_TEST );
                disc( 1 );
            }
        }
    }

    glDisable( GL_DEPTH_TEST );
    if( diagnostics )
    {
        auto text = [&]( const std::string& value, int x, int y,
                         const std::array<int, 3>& color )
        {
            std::istringstream input( value );
            std::string row;
            while( std::getline( input, row ) )
            {
                std::vector<std::unique_ptr<KIFONT::GLYPH>> glyphs;
                BOX2I box;
                KIFONT::FONT::GetFont()->GetTextAsGlyphs( &box, &glyphs,
                        wxString::FromUTF8( row.c_str() ), { 11, 11 }, { x, y },
                        ANGLE_0, false, { 0, 0 }, 0 );
                glColor4d( 0, 0, 0, 0.75 );
                glBegin( GL_QUADS );
                glVertex2d( box.GetLeft()-3, box.GetTop()-3 );
                glVertex2d( box.GetRight()+3, box.GetTop()-3 );
                glVertex2d( box.GetRight()+3, box.GetBottom()+3 );
                glVertex2d( box.GetLeft()-3, box.GetBottom()+3 );
                glEnd();
                glColor4d( color[0]/255.0, color[1]/255.0, color[2]/255.0, 1 );
                glLineWidth( dpi );
                for( const auto& glyph : glyphs )
                    if( glyph->IsStroke() )
                        for( const auto& stroke : *static_cast<KIFONT::STROKE_GLYPH*>( glyph.get() ) )
                        {
                            glBegin( GL_LINE_STRIP );
                            for( const auto& point : stroke )
                                glVertex2d( point.x, point.y );
                            glEnd();
                        }
                y += 15;
            }
        };
        int y = 24;
        for( const auto& line : diagnostics->lines )
        {
            text( line.text, 12, y, line.color );
            y += 15;
        }
        for( const auto& marker : diagnostics->markers )
        {
            const double x = marker.point[0], y = marker.point[1];
            glColor4d( marker.color[0]/255.0, marker.color[1]/255.0,
                       marker.color[2]/255.0, 0.65 );
            glLineWidth( 2*dpi );
            glBegin( GL_LINES );
            glVertex2d( x-9, y ); glVertex2d( x+9, y );
            glVertex2d( x, y-9 ); glVertex2d( x, y+9 );
            glEnd();
            text( marker.label, x+12, y-8, marker.color );
        }
    }
    glMatrixMode( GL_MODELVIEW );
    glPopMatrix();
    glMatrixMode( GL_PROJECTION );
    glPopMatrix();
    glMatrixMode( matrixMode );
    glPopAttrib();
}
