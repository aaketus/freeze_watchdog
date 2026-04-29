cmake_minimum_required(VERSION 3.21)
project(freeze_watchdog LANGUAGES CXX)

set(PLUGIN_VERSION "0.1.0")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_library(freeze-watchdog MODULE
    src/plugin-main.cpp
)

target_compile_definitions(freeze-watchdog PRIVATE
    PLUGIN_VERSION="${PLUGIN_VERSION}"
)

if(WIN32)
    set(OBS_SDK_DIR "${CMAKE_SOURCE_DIR}/OBS")
    set(OBS_INCLUDE_DIR "${OBS_SDK_DIR}/include")
    set(OBS_LIB_DIR "${OBS_SDK_DIR}/lib")

    target_include_directories(freeze-watchdog PRIVATE
        "${OBS_INCLUDE_DIR}"
        "${OBS_INCLUDE_DIR}/libobs"
        "${OBS_INCLUDE_DIR}/frontend-api"
    )

    target_link_libraries(freeze-watchdog PRIVATE
        Qt6::Widgets
        "${OBS_LIB_DIR}/obs.lib"
        "${OBS_LIB_DIR}/obs-frontend-api.lib"
        gdiplus
    )

    set_target_properties(freeze-watchdog PROPERTIES
        PREFIX ""
        SUFFIX ".dll"
        OUTPUT_NAME "freeze-watchdog"
    )

elseif(UNIX)
    find_package(PkgConfig REQUIRED)

    pkg_check_modules(LIBOBS REQUIRED libobs)

    target_include_directories(freeze-watchdog PRIVATE
        ${LIBOBS_INCLUDE_DIRS}
        /usr/include/obs
    )

    target_link_directories(freeze-watchdog PRIVATE
        ${LIBOBS_LIBRARY_DIRS}
    )

    target_link_libraries(freeze-watchdog PRIVATE
        Qt6::Widgets
        ${LIBOBS_LIBRARIES}
        obs-frontend-api
    )

    set_target_properties(freeze-watchdog PROPERTIES
        PREFIX ""
        SUFFIX ".so"
        OUTPUT_NAME "freeze-watchdog"
    )
endif()

message(STATUS "Qt6_DIR=${Qt6_DIR}")
message(STATUS "Qt6Widgets_DIR=${Qt6Widgets_DIR}")