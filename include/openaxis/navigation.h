// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <openaxis/connection_manager.hpp>
#include <openaxis/navigation.hpp>
#include <openaxis/logging.hpp>
#include <openaxis/wx_scheduler.h>
#include <wx/app.h>
#include <wx/toplevel.h>
#include <wx/utils.h>

// Host callbacks and all SDK navigation work run on the wx UI thread.
class OPENAXIS_NAVIGATION final : public openaxis::NavigationAdapter, private wxEvtHandler
{
public:
    struct HOST
    {
        std::function<std::string()> context;
        std::function<std::optional<openaxis::Pose>()> read;
        std::function<bool( const openaxis::Pose& )> write;
        std::function<openaxis::Value( const std::string& )> fact;
        std::function<void( std::optional<openaxis::Vec3> )> pivot;
        std::vector<std::string> tags;
    };

    OPENAXIS_NAVIGATION( wxWindow& aWindow, HOST aHost ) :
        m_window( aWindow ), m_host( std::move( aHost ) ),
        m_client( ClientOptions( &m_scheduler ) ),
        m_session( m_client, *this, nullptr, Options() ),
        m_connection( m_client, { [this] {
            openaxis::ConnectionMetadata metadata;
            metadata.tags = m_host.tags;
            metadata.capabilities = { "navigation" };
            metadata.focused = Focused();
            return metadata;
        } } )
    {
        m_scheduler.SetBeforeDispatch( [this] { Refresh(); } );
        wxTheApp->Bind( wxEVT_IDLE, &OPENAXIS_NAVIGATION::OnIdle, this );
        m_connection.start();
    }

    ~OPENAXIS_NAVIGATION() override
    {
        wxTheApp->Unbind( wxEVT_IDLE, &OPENAXIS_NAVIGATION::OnIdle, this );
        m_scheduler.SetBeforeDispatch( {} );
        m_connection.stop();
        m_session.close();
    }

    void Invalidate()
    {
        ++m_generation;
        m_session.cancel( "view_changed" );
    }

private:
    static openaxis::OpenAxisClientOptions ClientOptions( openaxis::Scheduler* aScheduler )
    {
        openaxis::DiagnosticLog::configure( "kicad" );
        openaxis::OpenAxisClientOptions options;
        options.client_name = "KiCad";
        options.scheduler = aScheduler;
        options.target = { { "pid", openaxis::current_process_id() } };
        return options;
    }

    openaxis::NavigationOptions Options()
    {
        openaxis::NavigationOptions options;
        options.scheduler = &m_scheduler;
        options.observation = [this]( const openaxis::NavigationContext& context ) {
            return is_current( context ) ? m_host.read() : std::nullopt;
        };
        return options;
    }

    bool Focused() const
    {
        auto* top = dynamic_cast<wxTopLevelWindow*>( wxGetTopLevelParent( &m_window ) );
        const wxSize size = m_window.GetClientSize();
        return top && top->IsActive() && top->IsEnabled() && m_window.IsShownOnScreen()
               && m_window.IsEnabled() && size.x > 0 && size.y > 0
               && !m_host.context().empty();
    }

    std::string Key() const
    {
        const wxSize size = m_window.GetClientSize();
        return m_host.context() + "/" + std::to_string( m_generation ) + "/"
               + std::to_string( size.x ) + "/" + std::to_string( size.y ) + "/"
               + std::to_string( m_window.GetContentScaleFactor() ) + "/"
               + std::to_string( m_window.GetDPIScaleFactor() );
    }

    void OnIdle( wxIdleEvent& event ) { Refresh(); event.Skip(); }

    void Refresh()
    {
        const bool focused = Focused();
        const auto key = Key();
        if( key != m_key || ( m_focused && !focused ) )
            m_session.cancel( "context_changed" );
        m_key = key;
        if( focused != m_focused )
        {
            m_focused = focused;
            m_connection.refresh_metadata();
        }
        const auto pose = m_host.read();
        const auto value = pose ? openaxis::pose_value( *pose ) : openaxis::Value{};
        if( value != m_lastPose )
        {
            m_lastPose = value;
            if( focused )
                m_session.native_camera_changed();
        }
    }

    openaxis::NavigationContext capture_context() override
    {
        return Focused() ? openaxis::NavigationContext( Key() ) : openaxis::NavigationContext{};
    }

    bool is_current( const openaxis::NavigationContext& context ) override
    {
        const auto* key = std::any_cast<std::string>( &context );
        return key && Focused() && *key == Key();
    }

    struct CAPTURE final : openaxis::NavigationCapture
    {
        OPENAXIS_NAVIGATION& owner;
        openaxis::NavigationContext context;
        std::optional<openaxis::Pose> pose;
        openaxis::Value aspect, cursor;
        CAPTURE( OPENAXIS_NAVIGATION& aOwner, const openaxis::NavigationContext& aContext ) :
            owner( aOwner ), context( aContext ), pose( owner.m_host.read() )
        {
            const auto size = owner.m_window.GetClientSize();
            aspect = double( size.x ) / size.y;
            const auto point = owner.m_window.ScreenToClient( wxGetMousePosition() );
            if( owner.m_window.GetClientRect().Contains( point ) )
                cursor = { 2.0 * point.x / size.x - 1.0, 1.0 - 2.0 * point.y / size.y };
        }
        std::optional<openaxis::Pose> initial_observation() override { return pose; }
        openaxis::Value resolve( const std::string& name ) override
        {
            if( !owner.is_current( context ) ) return nullptr;
            if( name == "camera.pose" ) return pose ? openaxis::pose_value( *pose ) : openaxis::Value{};
            if( name == "viewport.aspect" ) return aspect;
            if( name == "viewport.cursor" ) return cursor;
            return owner.m_host.fact ? owner.m_host.fact( name ) : openaxis::Value{};
        }
    };

    std::unique_ptr<openaxis::NavigationCapture> begin_query(
            const openaxis::NavigationContext& context ) override
    {
        return is_current( context ) ? std::make_unique<CAPTURE>( *this, context ) : nullptr;
    }

    openaxis::WriteResult apply_pose( const openaxis::NavigationContext& context,
            const openaxis::NavigationPose& pose, const openaxis::Value&,
            std::optional<openaxis::Vec3> ) override
    {
        if( !is_current( context ) || !m_host.write( pose ) ) return {};
        auto realized = m_host.read();
        m_lastPose = realized ? openaxis::pose_value( *realized ) : openaxis::Value{};
        return { true, realized };
    }

    void show_pivot( const openaxis::NavigationContext& context,
                     std::optional<openaxis::Vec3> pivot ) override
    {
        if( m_host.pivot && ( !pivot || is_current( context ) ) ) m_host.pivot( pivot );
    }

    wxWindow& m_window;
    HOST m_host;
    OPENAXIS_WX_SCHEDULER m_scheduler;
    openaxis::OpenAxisClient m_client;
    openaxis::NavigationSession m_session;
    openaxis::OpenAxisConnectionManager m_connection;
    std::string m_key;
    openaxis::Value m_lastPose;
    unsigned long long m_generation = 0;
    bool m_focused = false;
};
