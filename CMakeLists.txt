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

# The Engine defines FE_HAS_VULKAN inside its own directory, so those defines do
# not reach this sibling target. SuperFlight renders through the Vulkan backend,
# so its translation units (engine inline headers) must see the same define.
if(NOT ANDROID AND NOT EMSCRIPTEN AND NOT FE_EXCLUDE_VULKAN)
	target_compile_definitions(${SUPERFLIGHT_TARGET} PRIVATE FE_HAS_VULKAN)
endif()

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
	target_compile_options(${SUPERFLIGHT_TARGET} PRIVATE /EHsc)
	target_link_options(${SUPERFLIGHT_TARGET} PRIVATE $<$<CONFIG:Debug>:/DEBUG>)

	add_compile_options(/RTC1)
endif()

set_property(TARGET ${SUPERFLIGHT_TARGET} PROPERTY VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# ---------------------------------------------------------------
# Vulkan runtime resources + GLSL -> SPIR-V shader compilation
# ---------------------------------------------------------------
if(NOT ANDROID)
	set(RESOURCE_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/resources)
	set(RESOURCE_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/resources)

	add_custom_target(CopyResources_SuperFlight ALL
		COMMAND ${CMAKE_COMMAND} -E copy_directory ${RESOURCE_SRC_DIR} ${RESOURCE_OUT_DIR}
		COMMENT "Copying SuperFlight resources to ${RESOURCE_OUT_DIR}"
	)

	find_program(GLSLC glslc HINTS
		$ENV{VULKAN_SDK}/Bin
		$ENV{VULKAN_SDK}/bin
	)

	if(GLSLC)
		message(STATUS "Found glslc: ${GLSLC}")

		set(SHADER_SRC_DIR ${RESOURCE_SRC_DIR}/shaders)
		set(SHADER_OUT_DIR ${RESOURCE_OUT_DIR}/shaders)
		file(MAKE_DIRECTORY ${SHADER_OUT_DIR})

		set(SHADER_SOURCES
			${SHADER_SRC_DIR}/VertexShader_vk.glsl
			${SHADER_SRC_DIR}/FragmentShader_vk.glsl
			${SHADER_SRC_DIR}/VertexShader_vk_foxcraft.glsl
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

		add_custom_target(SPIRV_ShadersSuperFlight ALL DEPENDS ${SPIRV_OUTPUTS})
		add_dependencies(SPIRV_ShadersSuperFlight CopyResources_SuperFlight)
		add_dependencies(${SUPERFLIGHT_TARGET} SPIRV_ShadersSuperFlight)
	else()
		message(WARNING "glslc not found - SuperFlight shader compilation skipped. Set VULKAN_SDK environment variable.")
	endif()

	# Keep resources next to the executable too, so SDL_GetBasePath() resolves the
	# SPIR-V regardless of the launcher's working directory (VS, VS Code, exe).
	add_custom_command(TARGET ${SUPERFLIGHT_TARGET} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_directory ${RESOURCE_OUT_DIR} $<TARGET_FILE_DIR:${SUPERFLIGHT_TARGET}>/resources
		COMMENT "Copying SuperFlight resources next to the executable"
	)

	# The shared voxel block textures live in the repo-root Assets folder. Copy
	# them next to the executable so ChunkMesher::BuildBlockTextureImages() can
	# resolve "Assets/Grass.png" via ImageLoader's executable-directory fallback
	# regardless of the working directory.
	set(ASSETS_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../Assets)
	if(EXISTS ${ASSETS_SRC_DIR})
		add_custom_command(TARGET ${SUPERFLIGHT_TARGET} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_directory ${ASSETS_SRC_DIR} $<TARGET_FILE_DIR:${SUPERFLIGHT_TARGET}>/Assets
			COMMENT "Copying shared Assets next to the executable"
		)
	endif()
endif()
