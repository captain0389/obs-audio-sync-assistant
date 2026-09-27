cmake_minimum_required(VERSION 3.28)
project(obs-audio-sync-assistant VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_QT "Build the OBS Qt dock UI" ON)

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets)

add_library(obs-audio-sync-assistant MODULE
    src/plugin-main.cpp
    src/sync-engine.cpp
    src/sync-dock.cpp
    src/audio-capture.cpp
)

target_link_libraries(obs-audio-sync-assistant PRIVATE
    OBS::libobs
    OBS::obs-frontend-api
    Qt6::Core
    Qt6::Widgets
)

target_include_directories(obs-audio-sync-assistant PRIVATE src)

target_compile_definitions(obs-audio-sync-assistant PRIVATE
    OBS_AUDIO_SYNC_ASSISTANT_VERSION="${PROJECT_VERSION}"
)

if(MSVC)
    target_compile_options(obs-audio-sync-assistant PRIVATE /W4 /permissive-)
endif()

set_target_properties(obs-audio-sync-assistant PROPERTIES
    OUTPUT_NAME "obs-audio-sync-assistant"
    PREFIX ""
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
)

install(TARGETS obs-audio-sync-assistant
    RUNTIME DESTINATION obs-plugins/64bit
    LIBRARY DESTINATION obs-plugins
)

install(DIRECTORY data/
    DESTINATION data/obs-plugins/obs-audio-sync-assistant
)
