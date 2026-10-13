cmake_minimum_required(VERSION 3.15)
cmake_policy(SET CMP0091 NEW)
project(SuperFlight)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows 7 targeting is set globally in root CMakeLists.txt via add_compile_definitions
if(MSVC)
    set(CMAKE_SYSTEM_VERSION 6.1) # Corresponds to Windows 7 internal version
endif()

# Ensure the Windows SDK exposes modern API structs like AudioClientProperties to SDL3
add_compile_definitions(AUDIO_CLIENT_PROPERTIES_ENUM_DEFINED)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(KISSFFT_TEST OFF CACHE BOOL "Disable KissFFT tests" FORCE)

set(KISSFFT_STATIC ON CACHE BOOL "Build static library" FORCE)

set(FE_EXCLUDE_JOLT OFF CACHE BOOL "Disable JOLT" FORCE)

set(ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../FenixEngine")

if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
endif()

if(NOT TARGET Engine)
    message(STATUS "Engine target not found. Looking for engine source...")

    if(EXISTS "${ENGINE_DIR}/CMakeLists.txt")
        add_subdirectory("${ENGINE_DIR}" FenixEngine)
    else()
        message(FATAL_ERROR "FenixEngine not found!")
    endif()
endif()

set(IMGUI_DIR ../FenixEngine/external/imgui)

file(GLOB_RECURSE IMGUI_SOURCE_FILES
	${IMGUI_DIR}/imgui.cpp
	${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
	${IMGUI_DIR}/backends/imgui_impl_sdlgpu3.cpp
	${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp
	${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
	${IMGUI_DIR}/imgui_tables.cpp
	${IMGUI_DIR}/imgui_widgets.cpp
	${IMGUI_DIR}/imgui_demo.cpp
	${IMGUI_DIR}/imgui_draw.cpp
)

set_source_files_properties(${IMGUI_SOURCE_FILES} PROPERTIES
	SKIP_UNITY_BUILD_INCLUSION ON
)

set(SUPERFLIGHT_SOURCES
	${IMGUI_SOURCE_FILES}
	main.cpp
)

if(ANDROID)
	add_library(main SHARED ${SUPERFLIGHT_SOURCES})
	set(SUPERFLIGHT_TARGET main)
	target_link_libraries(${SUPERFLIGHT_TARGET} PRIVATE android log)
else()
	add_executable(SuperFlight
		WIN32
		${SUPERFLIGHT_SOURCES}
	)
	set(SUPERFLIGHT_TARGET SuperFlight)
endif()

target_include_directories(${SUPERFLIGHT_TARGET} PRIVATE
	include
	${ENGINE_DIR}
	${ENGINE_DIR}/include
	${ENGINE_DIR}/external
	${ENGINE_DIR}/external/imgui
	${ENGINE_DIR}/external/kissfft
	${ENGINE_DIR}/external/SDL/include
	${ENGINE_DIR}/src/Engine
)

target_link_libraries(${SUPERFLIGHT_TARGET} PRIVATE
	Engine
)

target_precompile_headers(${SUPERFLIGHT_TARGET} PRIVATE
	"$<$<COMPILE_LANGUAGE:CXX>:${ENGINE_DIR}/src/Engine/pch.hpp>"
)

if(NOT WIN32 AND NOT EMSCRIPTEN AND NOT ANDROID)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
target_include_directories(${SUPERFLIGHT_TARGET} PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
target_link_libraries(${SUPERFLIGHT_TARGET} PRIVATE ${PIPEWIRE_LIBRARIES})
target_compile_options(${SUPERFLIGHT_TARGET} PRIVATE ${PIPEWIRE_CFLAGS_OTHER})

find_package(X11 REQUIRED)

target_link_libraries(${SUPERFLIGHT_TARGET} PRIVATE X11::X11)
endif()

if(MSVC)
	target_link_options(${SUPERFLIGHT_TARGET} PRIVATE $<$<CONFIG:Debug>:/DEBUG>)

	add_compile_options(/RTC1)
endif()

set_property(TARGET ${SUPERFLIGHT_TARGET} PROPERTY VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
