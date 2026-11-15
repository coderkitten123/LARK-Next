cmake_minimum_required(VERSION 3.5)
project(larkplayer)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(GTK IMPORTED_TARGET REQUIRED gtk+-2.0)
pkg_check_modules(XML IMPORTED_TARGET REQUIRED libxml-2.0)
pkg_check_modules(SQLite IMPORTED_TARGET REQUIRED sqlite3)
#pkg_check_modules(GLIB REQUIRED glib-2.0)
#pkg_check_modules(GST REQUIRED gstreamer-0.10)

find_package(Threads REQUIRED)

add_subdirectory(miniaudio)

add_executable(${PROJECT_NAME}
    m4b_player.cpp
    music_backend.cpp
    database_manager.cpp
    abs_client.cpp
    abs_ui.cpp
    third_party/cJSON/cJSON.c
    mpeg4/mp4read.c
    mpeg4/unicode_support.c
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    PkgConfig::GTK
    PkgConfig::XML
    PkgConfig::SQLite
    Threads::Threads
    miniaudio
    faad
    faad_drm
    gstreamer-0.10
    gthread-2.0
    lipc
    dl
    m
    rt
)

target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)

# Kindle firmware ships libstdc++ with GLIBCXX up to 3.4.28; modern gcc emits
# 3.4.29+. Static-link the C++ runtime so the binary needs no newer lib.
target_link_options(${PROJECT_NAME} PRIVATE -static-libstdc++ -static-libgcc)
