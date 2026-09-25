// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <gal/3d/camera.h>
#include <openaxis/geometry.hpp>

namespace KICAD_OPENAXIS_CAMERA
{
inline openaxis::Pose Read2D( double x, double y, double extent, bool mirrorX,
                             bool mirrorY, double depth )
{
    const double sx = mirrorX ? -1 : 1;
    const double sy = mirrorY ? -1 : 1;
    openaxis::Pose pose{ { x, -y, depth },
        openaxis::Quat::from_basis( { sx, 0, 0 }, { 0, sy, 0 }, { 0, 0, sx * sy } ).rotvec() };
    pose.ortho_extent = extent;
    return pose;
}
inline std::optional<openaxis::Pose> Read( CAMERA& camera )
{
        const auto& matrix = camera.GetViewMatrix_Inv();
        const auto vec = []( const glm::vec4& v ) -> openaxis::Vec3 { return { v.x, v.y, v.z }; };
        openaxis::Pose pose{ vec( matrix[3] ),
            openaxis::Quat::from_basis( vec( matrix[0] ), vec( matrix[1] ), vec( matrix[2] ) ).rotvec() };
        const double yy = camera.GetProjectionMatrix()[1][1];
        if( !std::isfinite( yy ) || yy <= 0 ) return {};
        if( camera.GetProjection() == PROJECTION_TYPE::PERSPECTIVE )
            pose.fov = 2 * std::atan( 1 / yy );
        else
            pose.ortho_extent = 2 / yy;
        return pose;
}
inline bool Write( CAMERA& camera, const openaxis::Pose& pose )
{
        const auto q = openaxis::Quat::from_rotvec( pose.r );
        const auto column = []( openaxis::Vec3 v ) { return glm::vec4( v.x, v.y, v.z, 0 ); };
        glm::mat4 world( 1.0f );
        world[0] = column( q.rotate( { 1, 0, 0 } ) );
        world[1] = column( q.rotate( { 0, 1, 0 } ) );
        world[2] = column( q.rotate( { 0, 0, 1 } ) );
        world[3] = glm::vec4( pose.t.x, pose.t.y, pose.t.z, 1 );
        auto view = glm::inverse( world );
        if( camera.GetProjection() == PROJECTION_TYPE::ORTHO )
        {
            if( !std::isfinite( pose.ortho_extent ) || pose.ortho_extent <= 0 ) return false;
            camera.Zoom( ( 2.0 / camera.GetProjectionMatrix()[1][1] ) / pose.ortho_extent );
            // Native orthographic zoom is coupled to camera depth. Preserve its realized depth.
            const glm::vec4 lookat( camera.GetLookAtPos(), 1 );
            view[3].z += ( camera.GetViewMatrix() * lookat ).z - ( view * lookat ).z;
        }
        camera.SetViewMatrix( view );
        camera.Update();
        camera.SetT0_and_T1_current_T();

        return true;
}
}

