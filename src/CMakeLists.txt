set(ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")

set(SOURCE_DIR "${ROOT_DIR}/src")
set(SOURCE_FILES
	"${SOURCE_DIR}/Crash/Introspection/Introspection.cpp"
	"${SOURCE_DIR}/Crash/Introspection/Introspection.h"
	"${SOURCE_DIR}/Crash/Modules/ModuleHandler.cpp"
	"${SOURCE_DIR}/Crash/Modules/ModuleHandler.h"
	"${SOURCE_DIR}/Crash/CrashHandler.cpp"
	"${SOURCE_DIR}/Crash/CrashHandler.h"
	"${SOURCE_DIR}/main.cpp"
	"${SOURCE_DIR}/PCH.cpp"
	"${SOURCE_DIR}/PCH.h"
)

source_group(TREE "${ROOT_DIR}" FILES ${SOURCE_FILES})

set(VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/src/Plugin.h")
configure_file(
	"${ROOT_DIR}/cmake/Plugin.h.in"
	"${VERSION_HEADER}"
	@ONLY
)

source_group("src" FILES "${VERSION_HEADER}")

configure_file(
	"${ROOT_DIR}/cmake/version.rc.in"
	"${CMAKE_CURRENT_BINARY_DIR}/version.rc"
	@ONLY
)

add_library(
	"${PROJECT_NAME}"
	SHARED
	${SOURCE_FILES}
	"${VERSION_HEADER}"
	"${CMAKE_CURRENT_BINARY_DIR}/version.rc"
	"${ROOT_DIR}/.clang-format"
	"${ROOT_DIR}/.editorconfig"
)

target_compile_features(
	"${PROJECT_NAME}"
	PRIVATE
		cxx_std_20
)

if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
	target_compile_options(
		"${PROJECT_NAME}"
		PRIVATE
			"/EHa"	# Exception handling model
			"/sdl"	# Enable Additional Security Checks
			"/utf-8"	# Set Source and Executable character sets to UTF-8
			"/Zi"	# Debug Information Format

			"/permissive-"	# Standards conformance
			"/Zc:preprocessor"	# Enable preprocessor conformance mode

			"/wd4200" # nonstandard extension used : zero-sized array in struct/union

			"$<$<CONFIG:DEBUG>:>"
			"$<$<CONFIG:RELEASE>:/Zc:inline;/JMC-;/Ob3>"
	)

	target_link_options(
		"${PROJECT_NAME}"
		PRIVATE
			"$<$<CONFIG:DEBUG>:/INCREMENTAL;/OPT:NOREF;/OPT:NOICF>"
			"$<$<CONFIG:RELEASE>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>"
	)
endif()

target_include_directories(
	"${PROJECT_NAME}"
	PRIVATE
		"${CMAKE_CURRENT_BINARY_DIR}/src"
		"${INCLUDE_DIR}"
		"${SOURCE_DIR}"
)

if (DEFINED CommonLibPath AND NOT ${CommonLibPath} STREQUAL "" AND IS_DIRECTORY ${CommonLibPath})
	add_subdirectory(${CommonLibPath} ${CommonLibName})
else ()
	message(
		FATAL_ERROR
		"Variable ${CommonLibName}Path is not set."
	)
endif()

find_package(Boost
	MODULE
	REQUIRED
	COMPONENTS
		stacktrace_windbg
)
find_package(fmt REQUIRED CONFIG)
find_package(frozen REQUIRED CONFIG)
find_package(infoware REQUIRED CONFIG)
find_package(spdlog REQUIRED CONFIG)

target_link_libraries(
	"${PROJECT_NAME}"
	PRIVATE
		Boost::stacktrace_windbg
		${CommonLibName}::${CommonLibName}
		Dbghelp.lib
		fmt::fmt
		frozen::frozen
		infoware
		spdlog::spdlog
)

target_precompile_headers(
	"${PROJECT_NAME}"
	PRIVATE
		"${SOURCE_DIR}/PCH.h"
)

if("${COPY_OUTPUT}")
	if (DEFINED SkyrimPath)
	add_custom_command(
		TARGET ${PROJECT_NAME}
		POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${PROJECT_NAME}> ${SkyrimPath}/Data/SKSE/Plugins/
		COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_PDB_FILE:${PROJECT_NAME}> ${SkyrimPath}/Data/SKSE/Plugins/
	)
	else ()
	message(
		WARNING
		"Variable ${SkyrimPath} is not defined. Skipping post-build copy command."
	)
	endif ()
endif()
