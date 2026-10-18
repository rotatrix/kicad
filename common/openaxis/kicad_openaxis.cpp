// SPDX-License-Identifier: GPL-3.0-or-later
#include <openaxis/kicad_openaxis.h>
#include <openaxis/wx_scheduler.h>
#include <openaxis/connection_manager.hpp>
#include <openaxis/logging.hpp>
#include <openaxis/navigation.hpp>
#include <openaxis/process_identity.hpp>
#include <build_version.h>
#include <wx/app.h>
#include <wx/toplevel.h>
#include <wx/utils.h>
#include <algorithm>
#include <memory>

namespace
{
bool g_diagnosticsVisible = false;

openaxis::Value VectorValue( const openaxis::Vec3& aVector )
{
    return openaxis::vector_value( aVector );
}

openaxis::Value BoxValue( const std::optional<OPENAXIS_BOX>& aBox )
{
    if( !aBox )
        return nullptr;
    return { { "min", VectorValue( aBox->min ) }, { "max", VectorValue( aBox->max ) } };
}

bool SamePose( const std::optional<openaxis::Pose>& a, const std::optional<openaxis::Pose>& b )
{
    if( !a || !b )
        return a.has_value() == b.has_value();
    return a->t.x == b->t.x && a->t.y == b->t.y && a->t.z == b->t.z && a->r.x == b->r.x
           && a->r.y == b->r.y && a->r.z == b->r.z && a->fov == b->fov
           && a->ortho_extent == b->ortho_extent;
}

std::array<int, 3> ToneColor( const std::string& aTone )
{
    const auto& colors = openaxis::diagnostic_colors();
    const auto  it = colors.find( aTone );
    return it != colors.end() ? it->second : colors.at( "text" );
}


// One client, connection manager and navigation session per process. The
// session binds whichever registered canvas is in the foreground window.
class SERVICE final : public openaxis::NavigationAdapter, private wxEvtHandler
{
public:
    SERVICE() :
            m_log( openaxis::DiagnosticLog::configure( "kicad", {},
                                                       GetBuildVersion().ToStdString() ) ),
            m_client( ClientOptions( &m_scheduler ) ),
            m_collector( CollectorOptions() ),
            m_session( m_client, *this, &m_collector, Options() ),
            m_connection( m_client, { [this] { return Metadata(); } } )
    {
        m_collector.on_changed = [this] { RequestRepaint(); };
        m_connection.on_state = [this]( const openaxis::ConnectionStatus& ) { RequestRepaint(); };
        m_scheduler.SetBeforeDispatch( [this] { Refresh(); } );
        wxTheApp->Bind( wxEVT_IDLE, &SERVICE::OnIdle, this );
        m_connection.start();
    }

    ~SERVICE() override
    {
        wxTheApp->Unbind( wxEVT_IDLE, &SERVICE::OnIdle, this );
        m_scheduler.SetBeforeDispatch( {} );
        m_connection.stop();
        m_session.close();
        m_log->close();
    }

    void Register( OPENAXIS_CANVAS* aCanvas ) { m_canvases.push_back( { aCanvas, ++m_serial } ); }

    /// Returns true when no canvases remain.
    bool Unregister( OPENAXIS_CANVAS* aCanvas )
    {
        auto it = std::find_if( m_canvases.begin(), m_canvases.end(),
                                [aCanvas]( const ENTRY& e ) { return e.canvas == aCanvas; } );
        if( it != m_canvases.end() )
        {
            if( it->serial == m_bound )
                m_session.cancel( "view_closed" );
            m_canvases.erase( it );
        }
        return m_canvases.empty();
    }

    void Invalidate( OPENAXIS_CANVAS* aCanvas )
    {
        if( ENTRY* entry = Find( aCanvas ) )
        {
            ++entry->generation;
            if( entry->serial == m_bound )
                m_session.cancel( "view_changed" );
        }
    }

    void SetDiagnosticsVisible( bool aVisible )
    {
        m_collector.set_enabled( aVisible );
        for( const ENTRY& entry : m_canvases )
            entry.canvas->DiagnosticsChanged();
    }

    std::optional<OPENAXIS_DIAGNOSTICS> Diagnostics( OPENAXIS_CANVAS* aCanvas )
    {
        // Evidence stays on the last foreground canvas while KiCad is inactive.
        ENTRY* entry = Find( aCanvas );
        if( !g_diagnosticsVisible || !entry || entry->serial != m_shown )
            return {};

        OPENAXIS_DIAGNOSTICS result;
        const auto status = m_connection.status();
        std::string text = "OpenAxis " + status.state;
        if( status.state == "retrying" && !status.error.empty() )
            text += ": " + status.error;
        result.lines.push_back( { text, ToneColor( status.state == "ready" ? "pass"
                                                   : status.state == "retrying" ? "missing"
                                                                                : "skipped" ) } );

        const auto frame = m_collector.presentation();
        if( frame.context == Key( *entry ) )
        {
            for( const auto& line : frame.lines )
                result.lines.push_back( { line.text, ToneColor( line.tone ) } );
            for( const auto& s : frame.segments )
                result.segments.push_back( { s.start, s.end, ToneColor( s.tone ), s.width, s.opacity } );
            for( const auto& m : frame.markers )
                result.markers.push_back( { m.label, m.point, ToneColor( m.tone ) } );
        }

        // Expired status changes without a notification; repaint at the deadline.
        if( frame.expires_at && *frame.expires_at != m_expiry )
        {
            m_expiry = *frame.expires_at;
            m_scheduler.post_at( m_expiry, [this] { RequestRepaint(); } );
        }
        return result;
    }

private:
    struct ENTRY
    {
        OPENAXIS_CANVAS*   canvas;
        unsigned long long serial;
        unsigned long long generation = 0;
    };

    struct CONTEXT
    {
        unsigned long long serial;
        std::string        key;
    };

    static openaxis::OpenAxisClientOptions ClientOptions( openaxis::Scheduler* aScheduler )
    {
        openaxis::OpenAxisClientOptions options;
        options.client_name = "KiCad";
        options.client_version = GetBuildVersion().ToStdString();
        options.scheduler = aScheduler;
        options.target = { { "pid", openaxis::current_process_id() } };
        return options;
    }

    static openaxis::DiagnosticOptions CollectorOptions()
    {
        openaxis::DiagnosticOptions options;
        options.enabled = g_diagnosticsVisible;
        return options;
    }

    openaxis::NavigationOptions Options()
    {
        openaxis::NavigationOptions options;
        options.scheduler = &m_scheduler;
        options.observation = [this]( const openaxis::NavigationContext& context )
        {
            ENTRY* entry = is_current( context ) ? Find( Serial( context ) ) : nullptr;
            return entry ? entry->canvas->ReadCamera() : std::nullopt;
        };
        options.compare_camera = [this]( const openaxis::Pose& a, const openaxis::Pose& b )
        {
            openaxis::ComparisonOptions tolerance;
            if( m_tolerance > 0 )
                tolerance = { m_tolerance, m_tolerance, m_tolerance, m_tolerance };
            return openaxis::compare_poses( a, b, tolerance );
        };
        options.on_event = [this]( const openaxis::NavigationEvent& event )
        {
            // Bind evidence to the view that produced it before the query completes.
            if( event.event == "query_context" )
                if( const auto* context = std::any_cast<CONTEXT>( &event.context ) )
                    m_collector.set_context( context->key );
        };
        return options;
    }

    openaxis::ConnectionMetadata Metadata() const
    {
        openaxis::ConnectionMetadata metadata;
        metadata.tags = m_tags;
        metadata.capabilities = { "navigation" };
        metadata.focused = m_focused;
        return metadata;
    }

    ENTRY* Find( OPENAXIS_CANVAS* aCanvas )
    {
        auto it = std::find_if( m_canvases.begin(), m_canvases.end(),
                                [aCanvas]( const ENTRY& e ) { return e.canvas == aCanvas; } );
        return it != m_canvases.end() ? &*it : nullptr;
    }

    ENTRY* Find( unsigned long long aSerial )
    {
        auto it = std::find_if( m_canvases.begin(), m_canvases.end(),
                                [aSerial]( const ENTRY& e ) { return e.serial == aSerial; } );
        return it != m_canvases.end() ? &*it : nullptr;
    }

    static unsigned long long Serial( const openaxis::NavigationContext& aContext )
    {
        const auto* context = std::any_cast<CONTEXT>( &aContext );
        return context ? context->serial : 0;
    }

    static bool Focused( const ENTRY& aEntry )
    {
        wxWindow* window = aEntry.canvas->Window();
        auto*     top = dynamic_cast<wxTopLevelWindow*>( wxGetTopLevelParent( window ) );
        const wxSize size = window->GetClientSize();
        return top && top->IsActive() && top->IsEnabled() && window->IsShownOnScreen()
               && window->IsEnabled() && size.x > 0 && size.y > 0
               && !aEntry.canvas->Target().empty();
    }

    // Everything whose change must retire a gesture bound to this canvas.
    static std::string Key( const ENTRY& aEntry )
    {
        wxWindow*    window = aEntry.canvas->Window();
        const wxSize size = window->GetClientSize();
        return std::to_string( aEntry.serial ) + "/" + std::to_string( aEntry.generation ) + "/"
               + aEntry.canvas->Target() + "/" + std::to_string( size.x ) + "x"
               + std::to_string( size.y ) + "/"
               + std::to_string( window->GetContentScaleFactor() ) + "/"
               + std::to_string( window->GetDPIScaleFactor() );
    }

    ENTRY* FocusedEntry()
    {
        for( ENTRY& entry : m_canvases )
            if( Focused( entry ) )
                return &entry;
        return nullptr;
    }

    void OnIdle( wxIdleEvent& aEvent )
    {
        Refresh();
        aEvent.Skip();
    }

    // Bind the foreground canvas, announce its tags and focus, and report native
    // camera changes. Runs on idle and before every scheduled SDK callback.
    void Refresh()
    {
        ENTRY*            entry = FocusedEntry();
        const std::string key = entry ? Key( *entry ) : std::string();

        if( key != m_key )
        {
            if( !m_key.empty() )
                m_session.cancel( "context_changed" );
            m_key = key;
            m_bound = entry ? entry->serial : 0;
            m_lastPose.reset();
            if( entry )
            {
                m_shown = entry->serial;
                m_collector.set_context( key );
            }
            // Even the same canvas can now show a different document or size.
            // Remove old evidence as soon as its captured context is invalid.
            RepaintAll();
        }

        if( entry )
        {
            m_tolerance = entry->canvas->ComparisonTolerance();
            auto tags = entry->canvas->Tags();
            if( tags != m_tags || !m_focused )
            {
                m_tags = std::move( tags );
                m_focused = true;
                m_connection.refresh_metadata();
            }

            const auto pose = entry->canvas->ReadCamera();
            if( !SamePose( pose, m_lastPose ) )
            {
                const bool known = m_lastPose.has_value();
                m_lastPose = pose;
                if( known )
                    m_session.native_camera_changed();
            }
        }
        else if( m_focused )
        {
            // Keep the last canvas's tags while KiCad is in the background.
            m_focused = false;
            m_connection.refresh_metadata();
        }
    }

    void RequestRepaint()
    {
        if( ENTRY* entry = Find( m_shown ) )
            entry->canvas->DiagnosticsChanged();
    }

    void RepaintAll()
    {
        if( g_diagnosticsVisible )
            for( const ENTRY& entry : m_canvases )
                entry.canvas->DiagnosticsChanged();
    }

    // NavigationAdapter

    openaxis::NavigationContext capture_context() override
    {
        Refresh();
        ENTRY* entry = Find( m_bound );
        if( !entry || m_key.empty() )
            return {};
        return CONTEXT{ entry->serial, m_key };
    }

    bool is_current( const openaxis::NavigationContext& aContext ) override
    {
        const auto* context = std::any_cast<CONTEXT>( &aContext );
        ENTRY*      entry = context ? Find( context->serial ) : nullptr;
        return entry && Focused( *entry ) && Key( *entry ) == context->key;
    }

    struct CAPTURE final : openaxis::NavigationCapture
    {
        SERVICE&                      owner;
        openaxis::NavigationContext   context;
        OPENAXIS_CANVAS*              canvas;
        std::optional<openaxis::Pose> pose;
        openaxis::Value               aspect, cursor;

        CAPTURE( SERVICE& aOwner, const openaxis::NavigationContext& aContext,
                 OPENAXIS_CANVAS* aCanvas ) :
                owner( aOwner ), context( aContext ), canvas( aCanvas ),
                pose( aCanvas->ReadCamera() )
        {
            wxWindow*    window = canvas->Window();
            const wxSize size = window->GetClientSize();
            aspect = double( size.x ) / size.y;
            const wxPoint point = window->ScreenToClient( wxGetMousePosition() );
            if( window->GetClientRect().Contains( point ) )
                cursor = { { "x", 2.0 * point.x / size.x - 1.0 },
                           { "y", 1.0 - 2.0 * point.y / size.y } };
        }

        std::optional<openaxis::Pose> initial_observation() override { return pose; }

        openaxis::Value resolve( const std::string& name ) override
        {
            if( !owner.is_current( context ) )
                return nullptr;
            if( name == "camera.pose" )
                return pose ? openaxis::pose_value( *pose ) : openaxis::Value();
            if( name == "viewport.aspect" )
                return aspect;
            if( name == "viewport.cursor" )
                return cursor;
            if( name == "world.orientation" )
            {
                const auto [up, forward] = canvas->WorldOrientation();
                return { { "up", VectorValue( up ) }, { "forward", VectorValue( forward ) },
                         { "handedness", "right" } };
            }
            if( name == "document.id" )
            {
                const std::string id = canvas->DocumentId();
                return id.empty() ? openaxis::Value() : openaxis::Value( id );
            }
            if( name == "model.bounds" )
                return BoxValue( canvas->ModelBounds() );
            if( name == "selection.bounds" )
                return BoxValue( canvas->SelectionBounds() );
            if( name == "camera.view_target" )
            {
                const auto target = canvas->ViewTarget();
                return target ? VectorValue( *target ) : openaxis::Value();
            }
            if( name == "pick.cursor" || name == "pick.viewport_center" )
            {
                const auto pick = canvas->Pick( name == "pick.viewport_center" );
                if( !pick )
                    return nullptr;
                // A marker-only result is a tested miss; the SDK strips the marker.
                openaxis::Value value = { { "markerPosition", { pick->marker[0], pick->marker[1] } } };
                if( pick->point )
                {
                    value["point"] = VectorValue( *pick->point );
                    if( pick->bounds )
                        value["bounds"] = BoxValue( pick->bounds );
                }
                return value;
            }
            // Selection-restricted picks are deferred.
            return nullptr;
        }
    };

    std::unique_ptr<openaxis::NavigationCapture>
    begin_query( const openaxis::NavigationContext& aContext ) override
    {
        if( !is_current( aContext ) )
            return nullptr;
        return std::make_unique<CAPTURE>( *this, aContext, Find( Serial( aContext ) )->canvas );
    }

    openaxis::WriteResult apply_pose( const openaxis::NavigationContext& aContext,
                                      const openaxis::NavigationPose& aPose, const openaxis::Value&,
                                      std::optional<openaxis::Vec3> ) override
    {
        if( !is_current( aContext ) )
            return {};
        OPENAXIS_CANVAS* canvas = Find( Serial( aContext ) )->canvas;
        if( !canvas->WriteCamera( aPose ) )
            return {};
        // Report the realized pose; the next idle comparison must not treat it as native input.
        m_lastPose = canvas->ReadCamera();
        return { true, m_lastPose };
    }

    void show_pivot( const openaxis::NavigationContext& aContext,
                     std::optional<openaxis::Vec3> aPivot ) override
    {
        ENTRY* entry = Find( Serial( aContext ) );
        if( entry && ( !aPivot || is_current( aContext ) ) )
            entry->canvas->ShowPivot( aPivot );
    }

    std::shared_ptr<openaxis::DiagnosticLog> m_log;
    OPENAXIS_WX_SCHEDULER                    m_scheduler;
    openaxis::OpenAxisClient                 m_client;
    openaxis::NavigationDiagnostics          m_collector;
    openaxis::NavigationSession              m_session;
    openaxis::OpenAxisConnectionManager      m_connection;

    std::vector<ENTRY>            m_canvases;
    unsigned long long            m_serial = 0;
    unsigned long long            m_bound = 0; ///< Canvas eligible for navigation
    unsigned long long            m_shown = 0; ///< Last foreground canvas, for diagnostics
    std::string                   m_key;
    std::vector<std::string>      m_tags{ "app.kicad" };
    bool                          m_focused = false;
    std::optional<openaxis::Pose> m_lastPose;
    double                        m_tolerance = 0;
    double                        m_expiry = 0;
};

std::unique_ptr<SERVICE> g_service;
} // namespace


void KIOPENAXIS::Register( OPENAXIS_CANVAS* aCanvas )
{
    if( !g_service )
        g_service = std::make_unique<SERVICE>();
    g_service->Register( aCanvas );
}


void KIOPENAXIS::Unregister( OPENAXIS_CANVAS* aCanvas )
{
    // The last canvas stops networking and closes the session while wx still exists.
    if( g_service && g_service->Unregister( aCanvas ) )
        g_service.reset();
}


void KIOPENAXIS::Invalidate( OPENAXIS_CANVAS* aCanvas )
{
    if( g_service )
        g_service->Invalidate( aCanvas );
}


bool KIOPENAXIS::DiagnosticsVisible()
{
    return g_diagnosticsVisible;
}


void KIOPENAXIS::SetDiagnosticsVisible( bool aVisible )
{
    g_diagnosticsVisible = aVisible;
    if( g_service )
        g_service->SetDiagnosticsVisible( aVisible );
}


std::optional<OPENAXIS_DIAGNOSTICS> KIOPENAXIS::Diagnostics( OPENAXIS_CANVAS* aCanvas )
{
    return g_service ? g_service->Diagnostics( aCanvas ) : std::nullopt;
}
