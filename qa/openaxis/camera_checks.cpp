// SPDX-License-Identifier: GPL-3.0-or-later
#include <openaxis/camera.h>
// Compile the shared wx adapter against the pinned SDK API as well.
#include <openaxis/navigation.h>
#include <track_ball.h>
#include <iostream>
#include <stdexcept>

using namespace KICAD_OPENAXIS_CAMERA;

static void require( bool value, const char* message )
{
    if( !value ) throw std::runtime_error( message );
}

static bool close( double a, double b, double tolerance = 1e-4 )
{
    return std::abs( a - b ) <= tolerance * std::max( 1.0, std::abs( b ) );
}

int main()
{
    try
    {
        for( bool mirrorX : { false, true } )
        for( bool mirrorY : { false, true } )
        {
            const auto pose = Read2D( 123, 456, 900, mirrorX, mirrorY, 789 );
            const auto q = openaxis::Quat::from_rotvec( pose.r );
            const auto right = q.rotate( { 1, 0, 0 } );
            const auto up = q.rotate( { 0, 1, 0 } );
            require( close( right.x, mirrorX ? -1 : 1 ) && close( up.y, mirrorY ? -1 : 1 ),
                     "mirrored 2D camera basis" );
            require( close( right.cross( up ).dot( q.rotate( { 0, 0, 1 } ) ), 1 ),
                     "2D camera is not right handed" );
            require( pose.t.x == 123 && pose.t.y == -456 && pose.t.z == 789,
                     "2D Y conversion or depth preservation" );
            require( pose.fov == 0 && pose.ortho_extent == 900, "2D projection facts" );
        }
        for( auto projection : { PROJECTION_TYPE::PERSPECTIVE, PROJECTION_TYPE::ORTHO } )
        {
            TRACK_BALL camera( { 0, 0, -100 }, { 0, 0, 0 }, projection );
            camera.SetCurWindowSize( wxSize( 1280, 720 ) );
            const auto initial = *Read( camera );
            auto requested = initial;
            requested.t.x += 4;
            requested.t.y += 3;
            // Rotate about the native look-at point while preserving an admissible distance.
            const auto rotation = openaxis::Quat::from_rotvec( { 0.2, -0.15, 0.1 } );
            requested.r = rotation.rotvec();
            requested.t = rotation.rotate( requested.t );
            require( Write( camera, requested ), "camera write failed" );
            const auto realized = *Read( camera );
            require( close( realized.t.x, requested.t.x ) && close( realized.t.y, requested.t.y )
                     && close( realized.t.z, requested.t.z ), "pose translation round trip" );
            require( ( openaxis::Quat::from_rotvec( realized.r ).rotate( { 0, 1, 0 } )
                     - rotation.rotate( { 0, 1, 0 } ) ).length() < 1e-4, "pose rotation round trip" );
            camera.SetCurMousePosition( wxPoint( 400, 300 ) );
            camera.Drag( wxPoint( 400, 300 ) );
            const auto afterMouse = *Read( camera );
            require( ( afterMouse.t - realized.t ).length() < 1e-3,
                     "native zero-delta drag jumps after SDK write" );
            camera.Pan( SFVEC3F( 1, 0, 0 ) );
            require( ( Read( camera )->t - afterMouse.t ).length() > 0.5,
                     "native pan does not continue from SDK pose" );
            if( projection == PROJECTION_TYPE::ORTHO )
            {
                auto zoom = *Read( camera );
                zoom.ortho_extent *= 0.5;
                require( Write( camera, zoom ), "ortho zoom write" );
                require( close( Read( camera )->ortho_extent, zoom.ortho_extent ), "ortho extent mapping" );
                zoom.ortho_extent = 1e-12;
                require( Write( camera, zoom ), "clamped zoom write" );
                require( close( camera.GetZoom(), camera.GetMinZoom() ), "minimum zoom clamp" );
                require( Read( camera )->ortho_extent > zoom.ortho_extent, "clamp readback missing" );
                zoom = *Read( camera );
                zoom.ortho_extent *= 2;
                Write( camera, zoom );
                require( camera.GetZoom() > camera.GetMinZoom(), "cannot reverse at zoom limit" );
            }
            else
                require( close( realized.fov, initial.fov ), "native fixed FOV changed" );
        }
        std::cout << "2D mirrors, native camera round trips, mouse continuation and zoom limits passed\n";
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
