cmake_minimum_required(VERSION 3.16)
project(OunceDesk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SDL2 REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

set(SOURCES
    source/main.cpp
    source/rdp_backend.cpp
    source/vnc_backend.cpp
    source/profiles.cpp
    source/input_mapper.cpp
    source/ui_renderer.cpp
    source/platform_desktop.cpp
    source/platform_switch2.cpp
    source/wg_tunnel.cpp
)

add_executable(OunceDesk ${SOURCES})
target_include_directories(OunceDesk PRIVATE source ${SDL2_INCLUDE_DIRS} ${ZLIB_INCLUDE_DIRS})
target_link_libraries(OunceDesk PRIVATE ${SDL2_LIBRARIES} ZLIB::ZLIB Threads::Threads mbedtls mbedx509 mbedcrypto)

if(WIN32)
    target_compile_definitions(OunceDesk PRIVATE _WIN32_WINNT=0x0600)
    target_link_libraries(OunceDesk PRIVATE ws2_32)
endif()
