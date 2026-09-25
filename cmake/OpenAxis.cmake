# SDK's GPL-3.0-only option is used for OpenAxis-enabled distributions.
option(KICAD_OPENAXIS "Enable OpenAxis 3D and 2D navigation" OFF)
set(OPENAXIS_SOURCE_DIR "" CACHE PATH "Local OpenAxis SDK checkout override")
if(KICAD_OPENAXIS)
    set(OPENAXIS_BUILD_TESTS OFF CACHE BOOL "Build SDK tests separately")
    set(OPENAXIS_BUILD_DEMO OFF CACHE BOOL "Build SDK demo separately")
    # Use KiCad's bundled JSON headers consistently across both sides of the ABI.
    if(NOT TARGET nlohmann_json::nlohmann_json)
        add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
    endif()
    if(OPENAXIS_SOURCE_DIR)
        set(_openaxis_source "${OPENAXIS_SOURCE_DIR}")
        add_subdirectory("${OPENAXIS_SOURCE_DIR}/cpp" "${PROJECT_BINARY_DIR}/openaxis")
    else()
        include(FetchContent)
        FetchContent_Declare(openaxis
            GIT_REPOSITORY https://github.com/rotatrix/openaxis.git
            GIT_TAG acc4da095cde6747556245b4b6c110c16b968b6b # cpp/v1.0.0-rc.1
            SOURCE_SUBDIR cpp)
        FetchContent_MakeAvailable(openaxis)
        set(_openaxis_source "${openaxis_SOURCE_DIR}")
    endif()
    set_target_properties(openaxis openaxis_ixwebsocket PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_compile_definitions(KICAD_OPENAXIS)
    install(FILES "${_openaxis_source}/LICENSE" DESTINATION "${KICAD_DOCS}/openaxis")
    install(FILES "${_openaxis_source}/cpp/third_party/ixwebsocket/LICENSE.txt"
        DESTINATION "${KICAD_DOCS}/openaxis" RENAME IXWebSocket-LICENSE.txt)
    install(FILES "${CMAKE_SOURCE_DIR}/Documentation/development/openaxis.md"
        DESTINATION "${KICAD_DOCS}/openaxis")
endif()
