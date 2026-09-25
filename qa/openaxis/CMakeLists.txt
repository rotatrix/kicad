cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(KiCadOpenAxisChecks LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(OPENAXIS_SOURCE_DIR "" CACHE PATH "Pinned OpenAxis SDK checkout")
    if(NOT EXISTS "${OPENAXIS_SOURCE_DIR}/cpp/include/openaxis/geometry.hpp")
        message(FATAL_ERROR "Set OPENAXIS_SOURCE_DIR to the OpenAxis SDK checkout")
    endif()
    find_package(wxWidgets REQUIRED COMPONENTS core base)
    include(${wxWidgets_USE_FILE})
    find_package(glm CONFIG QUIET)
    if(NOT TARGET glm::glm)
        include(FetchContent)
        FetchContent_Declare(glm URL https://github.com/g-truc/glm/archive/refs/tags/0.9.9.8.tar.gz
            URL_HASH SHA256=7d508ab72cb5d43227a3711420f06ff99b0a0cb63ee2f93631b162bfe1fe9592)
        FetchContent_MakeAvailable(glm)
    endif()
    set(OPENAXIS_CHECK_LIBS glm::glm)
else()
    set(OPENAXIS_CHECK_LIBS OpenAxis::openaxis)
endif()
set(KICAD_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
add_executable(openaxis_camera_checks
    camera_checks.cpp
    ${KICAD_ROOT}/common/gal/3d/camera.cpp
    ${KICAD_ROOT}/3d-viewer/3d_rendering/track_ball.cpp
    ${KICAD_ROOT}/3d-viewer/3d_rendering/trackball.cpp)
target_include_directories(openaxis_camera_checks PRIVATE
    ${KICAD_ROOT}/include ${KICAD_ROOT}/thirdparty/nlohmann_json
    ${KICAD_ROOT}/3d-viewer/3d_rendering ${OPENAXIS_SOURCE_DIR}/cpp/include)
target_compile_definitions(openaxis_camera_checks PRIVATE GAL_DLL _USE_MATH_DEFINES)
target_link_libraries(openaxis_camera_checks PRIVATE ${OPENAXIS_CHECK_LIBS} ${wxWidgets_LIBRARIES})
enable_testing()
add_test(NAME openaxis_native_camera COMMAND openaxis_camera_checks)

add_executable(openaxis_scheduler_checks scheduler_checks.cpp)
target_include_directories(openaxis_scheduler_checks PRIVATE
    ${KICAD_ROOT}/include ${KICAD_ROOT}/thirdparty/nlohmann_json
    ${OPENAXIS_SOURCE_DIR}/cpp/include)
set(OPENAXIS_CHECK_EXTRA_LIBRARIES "" CACHE STRING "Extra dependencies of a standalone static wx build")
target_link_libraries(openaxis_scheduler_checks PRIVATE ${wxWidgets_LIBRARIES} ${OPENAXIS_CHECK_EXTRA_LIBRARIES})
if(TARGET OpenAxis::openaxis)
    target_link_libraries(openaxis_scheduler_checks PRIVATE OpenAxis::openaxis)
else()
    target_sources(openaxis_scheduler_checks PRIVATE ${OPENAXIS_SOURCE_DIR}/cpp/src/diagnostics.cpp)
endif()
add_test(NAME openaxis_wx_scheduler COMMAND openaxis_scheduler_checks)

if(MSVC AND TARGET kicommon)
    # Full-tree checks use KiCad's shared JSON exports, unlike standalone checks.
    foreach(check IN ITEMS openaxis_camera_checks openaxis_scheduler_checks)
        add_custom_command(TARGET ${check} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${check}> $<TARGET_FILE_DIR:${check}>
            COMMAND_EXPAND_LISTS)
    endforeach()
endif()
