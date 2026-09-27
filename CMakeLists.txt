cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    src/plugin-main.cpp
    src/sync-engine.cpp
    src/sync-dock.cpp
    src/audio-capture.cpp
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    src
)

if(ENABLE_FRONTEND_API)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
        ENABLE_FRONTEND_API
    )
endif()

if(ENABLE_QT)
    find_package(Qt6 REQUIRED COMPONENTS Widgets)

    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
        Qt6::Widgets
    )

    set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES
        AUTOMOC ON
        AUTOUIC ON
        AUTORCC ON
    )
endif()

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
    OBS::libobs
)

if(ENABLE_FRONTEND_API)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
        OBS::obs-frontend-api
    )
endif()

setup_plugin_target(${CMAKE_PROJECT_NAME})

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/obs-audio-sync-assistant-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/obs-audio-sync-assistant-config.cmake"
    INSTALL_DESTINATION "lib/cmake/obs-audio-sync-assistant"
)

install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/obs-audio-sync-assistant-config.cmake"
    DESTINATION
        "lib/cmake/obs-audio-sync-assistant"
)

install(
    DIRECTORY data/
    DESTINATION data/obs-plugins/obs-audio-sync-assistant
)
