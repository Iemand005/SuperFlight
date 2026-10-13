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

# NOTE: SDL's WASAPI driver stays enabled on desktop. Forcing it off here (as a
# cached FORCE value) leaked into every sibling project and killed audio capture.

# set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(KISSFFT_TEST OFF CACHE BOOL "Disable KissFFT tests" FORCE)

set(KISSFFT_STATIC ON CACHE BOOL "Build static library" FORCE)

set(FE_EXCLUDE_JOLT OFF CACHE BOOL "Disable JOLT" FORCE)

set(ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../FenixEngine")

# add_subdirectory(${CMAKE_SOURCE_DIR}/../FenixEngine/src/Engine ${CMAKE_BINARY_DIR}/out/Engine)

# if(NOT TARGET Jolt)
#     add_subdirectory(${ENGINE_DIR}/external/JoltPhysics/Build ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/jolt)
# endif()

# set(USE_STATIC_MSVC_RUNTIME_LIBRARY ON CACHE BOOL "Use dynamic MSVC runtime to match rest of project" FORCE)

# option(USE_STATIC_MSVC_RUNTIME_LIBRARY "Use the static MSVC runtime" ON)

if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
endif()

# if(NOT TARGET Jolt)
#     add_subdirectory(${ENGINE_DIR}/external/JoltPhysics/Build ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/jolt)
#     set_target_properties(Jolt PROPERTIES DISABLE_PRECOMPILE_HEADERS ON)
# endif()

if(NOT TARGET Engine)
    message(STATUS "Engine target not found. Looking for engine source...")
    
    if(EXISTS "${ENGINE_DIR}/CMakeLists.txt")
        add_subdirectory("${ENGINE_DIR}" FenixEngine)
    # elif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/FenixEngine/CMakeLists.txt")
    #     add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/FenixEngine" FenixEngine)
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
	# ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
	${IMGUI_DIR}/imgui_tables.cpp
	${IMGUI_DIR}/imgui_widgets.cpp
	${IMGUI_DIR}/imgui_demo.cpp
	${IMGUI_DIR}/imgui_draw.cpp
)

# Keep every ImGui file in its own translation unit. They define
# IMGUI_DEFINE_MATH_OPERATORS immediately before including imgui.h, and
# imgui_internal.h hard-errors when that define did not come first. A unity
# blob that pulls in main.cpp/Chunk.cpp (which include imgui.h) ahead of them
# trips that check.
set_source_files_properties(${IMGUI_SOURCE_FILES} PROPERTIES
	SKIP_UNITY_BUILD_INCLUSION ON
)

set(FOXCRAFT_SOURCES
	${IMGUI_SOURCE_FILES}
	main.cpp
	ChunkManager.cpp
	Chunk.cpp
)

if(ANDROID)
	# Android: the SDL3 Java glue (SDLActivity / nativeRunMain) loads a shared
	# library named "main" (libmain.so) that must export SDL_main.
	add_library(main SHARED ${FOXCRAFT_SOURCES})
	set(FOXCRAFT_TARGET main)
	target_link_libraries(${FOXCRAFT_TARGET} PRIVATE android log)
else()
	add_executable(Foxcraft
		WIN32
		${FOXCRAFT_SOURCES}
	)
	set(FOXCRAFT_TARGET Foxcraft)
endif()

target_include_directories(${FOXCRAFT_TARGET} PRIVATE
	include
	${ENGINE_DIR}
	${ENGINE_DIR}/include
	${ENGINE_DIR}/external
	${ENGINE_DIR}/external/imgui
	${ENGINE_DIR}/external/kissfft
	${ENGINE_DIR}/external/SDL/include
	${ENGINE_DIR}/src/Engine
)

target_link_libraries(${FOXCRAFT_TARGET} PRIVATE
	Engine
)

target_precompile_headers(${FOXCRAFT_TARGET} PRIVATE
	"$<$<COMPILE_LANGUAGE:CXX>:${ENGINE_DIR}/src/Engine/pch.hpp>"
)

if(NOT WIN32 AND NOT EMSCRIPTEN AND NOT ANDROID)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
target_include_directories(${FOXCRAFT_TARGET} PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
target_link_libraries(${FOXCRAFT_TARGET} PRIVATE ${PIPEWIRE_LIBRARIES})
target_compile_options(${FOXCRAFT_TARGET} PRIVATE ${PIPEWIRE_CFLAGS_OTHER})

find_package(X11 REQUIRED)

target_link_libraries(${FOXCRAFT_TARGET} PRIVATE X11::X11)
endif()

set_property(TARGET ${FOXCRAFT_TARGET} PROPERTY VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_custom_command(TARGET ${FOXCRAFT_TARGET} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		${CMAKE_CURRENT_SOURCE_DIR}/resources
		$<TARGET_FILE_DIR:${FOXCRAFT_TARGET}>/resources
	COMMENT "Copying Foxcraft resources to build directory..."
)

if(MSVC)
	target_link_options(${FOXCRAFT_TARGET} PRIVATE $<$<CONFIG:Debug>:/DEBUG>)

	add_compile_options(/RTC1)
endif()

set_property(TARGET ${FOXCRAFT_TARGET} PROPERTY VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})


# ---------------------------------------------------------------
# Copy all runtime resources (glsl shaders, textures) to build output
# ---------------------------------------------------------------
set(RESOURCE_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/resources)
set(RESOURCE_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/resources)

add_custom_target(CopyResources_Foxcraft ALL
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${RESOURCE_SRC_DIR} ${RESOURCE_OUT_DIR}
	COMMENT "Copying resources to ${RESOURCE_OUT_DIR}"
)

# ---------------------------------------------------------------
# GLSL -> SPIR-V shader compilation
# ---------------------------------------------------------------
find_program(GLSLC glslc HINTS
	$ENV{VULKAN_SDK}/Bin
	$ENV{VULKAN_SDK}/bin
)

if(GLSLC AND NOT ANDROID)
	message(STATUS "Found glslc: ${GLSLC}")

	set(SHADER_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/resources/shaders)
	set(SHADER_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/resources/shaders)
	file(MAKE_DIRECTORY ${SHADER_OUT_DIR})

	set(SHADER_SOURCES
		${SHADER_SRC_DIR}/VertexShader_vk.glsl
		${SHADER_SRC_DIR}/FragmentShader_vk.glsl
		${SHADER_SRC_DIR}/VertexShader_vk_array.glsl
		${SHADER_SRC_DIR}/FragmentShader_vk_array.glsl
	)

	foreach(SHADER ${SHADER_SOURCES})
		get_filename_component(SHADER_NAME ${SHADER} NAME_WE)

		if(${SHADER_NAME} MATCHES ".*Fragment.*")
			set(SHADER_STAGE frag)
		else()
			set(SHADER_STAGE vert)
		endif()

		set(SPIRV_OUTPUT ${SHADER_OUT_DIR}/${SHADER_NAME}.spv)

		add_custom_command(
			OUTPUT ${SPIRV_OUTPUT}
			COMMAND ${GLSLC} -fshader-stage=${SHADER_STAGE} --target-env=vulkan1.0 -DFE_VULKAN ${SHADER} -o ${SPIRV_OUTPUT}
			DEPENDS ${SHADER}
			COMMENT "Compiling ${SHADER_NAME} -> ${SHADER_NAME}.spv"
			VERBATIM
		)

		list(APPEND SPIRV_OUTPUTS ${SPIRV_OUTPUT})
	endforeach()

	add_custom_target(SPIRV_Shaders_Foxcraft ALL DEPENDS ${SPIRV_OUTPUTS})
	add_dependencies(${FOXCRAFT_TARGET} SPIRV_Shaders_Foxcraft)
elseif(ANDROID)
	message(STATUS "Android build — skipping SPIR-V shader compilation (GLES path)")
else()
	message(WARNING "glslc not found — shader compilation skipped. Set VULKAN_SDK environment variable.")
endif()


include(${CMAKE_SOURCE_DIR}/EmscriptenConfig.cmake)
if(EMSCRIPTEN)
    # target_link_libraries(Foxcraft PRIVATE emscripten_wasm_settings)
    
    # configure_emscripten_target(Foxcraft "${CMAKE_CURRENT_SOURCE_DIR}/resources")

	setup_emscripten_target(Foxcraft
		RESOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/resources
	)

	# Publish the runnable Foxcraft build to the FenixWeb Pages docs folder.
	# Release emscripten builds only; the custom Foxcraft.html is never touched.
	if(CMAKE_BUILD_TYPE STREQUAL "Release")
		set(FOXCRAFT_PAGES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../FenixWeb/docs/FoxCraft)
		set(FOXCRAFT_OUT_DIR $<TARGET_FILE_DIR:Foxcraft>)

		add_custom_command(TARGET Foxcraft POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy
				${FOXCRAFT_OUT_DIR}/Foxcraft.wasm ${FOXCRAFT_PAGES_DIR}/Foxcraft.wasm
			COMMAND ${CMAKE_COMMAND} -E copy
				${FOXCRAFT_OUT_DIR}/Foxcraft.js ${FOXCRAFT_PAGES_DIR}/Foxcraft.js
			COMMAND ${CMAKE_COMMAND} -E copy
				${FOXCRAFT_OUT_DIR}/Foxcraft.data ${FOXCRAFT_PAGES_DIR}/Foxcraft.data
			COMMENT "Publishing Foxcraft build artifacts to ${FOXCRAFT_PAGES_DIR} (Release only)"
		)
	endif()
endif()
