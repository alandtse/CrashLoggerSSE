cmake_minimum_required(VERSION 3.21)

# #######################################################################################################################
# # Define project
# #######################################################################################################################
set(NAME "CrashLogger")
set(VERSION 1.10.0)

project(
        ${NAME}
        VERSION ${VERSION}
        DESCRIPTION "SKSE/SKSEVR plugin that generates crash logs when the game Just Works™."
        LANGUAGES CXX)
set(PROJECT_FRIENDLY_NAME "CrashLogger NG")
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF")

configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/version.rc
        @ONLY)

set(headers
        src/Settings.h
)

set(sources
        src/Crash/Introspection/Introspection.cpp
        src/Crash/Introspection/Introspection.h
        src/Crash/Modules/ModuleHandler.cpp
        src/Crash/Modules/ModuleHandler.h
        src/Crash/PDB/PdbHandler.cpp
        src/Crash/PDB/PdbHandler.h
        src/Crash/CrashHandler.cpp
        src/Crash/CrashHandler.h
        src/main.cpp
        src/Settings.cpp
        src/Settings.h
        src/PCH.cpp
        src/PCH.h
        ${CMAKE_CURRENT_BINARY_DIR}/version.rc
)

source_group(
        TREE ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
        ${headers}
        ${sources}

        # ${tests}
)

# ########################################################################################################################
# ## Build options
# ########################################################################################################################
message("Options:")
option(BUILD_TESTS "Build unit tests." OFF)
message("\tTests: ${BUILD_TESTS}")

# #######################################################################################################################
# # Add CMake features
# #######################################################################################################################
include(CheckIPOSupported)
include(GNUInstallDirs)

# #######################################################################################################################
# # Find dependencies
# #######################################################################################################################
find_package(CommonLibSSE CONFIG REQUIRED)
find_package(Boost
        MODULE
        REQUIRED
        COMPONENTS
        stacktrace_windbg
)
find_package(directxtk CONFIG REQUIRED)
find_package(frozen REQUIRED CONFIG)
find_package(infoware REQUIRED CONFIG)
find_package(magic_enum CONFIG REQUIRED)
find_path(CLIB_UTIL_INCLUDE_DIRS "ClibUtil/utils.hpp")
find_package(zycore CONFIG REQUIRED)
find_package(zydis CONFIG REQUIRED)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(OpenVR REQUIRED)

# https://github.com/microsoft/DirectXShaderCompiler/blob/main/cmake/modules/FindDiaSDK.cmake
# Find the DIA SDK path.
# It will typically look something like this:
# C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\DIA SDK\include

# CMAKE_GENERATOR_INSTANCE has the location of Visual Studio used
# i.e. C:/Program Files (x86)/Microsoft Visual Studio/2019/Community
set(VS_PATH ${CMAKE_GENERATOR_INSTANCE})
get_filename_component(VS_DIA_INC_PATH "${VS_PATH}/DIA SDK/include" ABSOLUTE CACHE)

# Starting in VS 15.2, vswhere is included.
# Unclear what the right component to search for is, might be Microsoft.VisualStudio.Component.VC.DiagnosticTools
# (although the friendly name of that is C++ profiling tools).  The toolset is the most likely target.
set(PROGRAMFILES_X86 "ProgramFiles(x86)")
execute_process(
        COMMAND "$ENV{${PROGRAMFILES_X86}}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        OUTPUT_VARIABLE VSWHERE_LATEST
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
)

find_path(DIASDK_INCLUDE_DIR # Set variable DIASDK_INCLUDE_DIR
        dia2.h # Find a path with dia2.h
        HINTS "${VS_DIA_INC_PATH}"
        HINTS "${VSWHERE_LATEST}/DIA SDK/include"
        DOC "path to DIA SDK header files"
)

# VS 2017
if(CMAKE_GENERATOR MATCHES "Visual Studio 15 2017.*")
        if(CMAKE_GENERATOR MATCHES "Visual Studio.*Win64")
                find_library(DIASDK_GUIDS_LIBRARY NAMES diaguids.lib HINTS ${DIASDK_INCLUDE_DIR}/../lib/amd64)
        elseif(CMAKE_GENERATOR_PLATFORM STREQUAL "ARM")
                find_library(DIASDK_GUIDS_LIBRARY NAMES diaguids.lib HINTS ${DIASDK_INCLUDE_DIR}/../lib/arm)
        else(CMAKE_GENERATOR MATCHES "Visual Studio.*Win64")
                find_library(DIASDK_GUIDS_LIBRARY NAMES diaguids.lib HINTS ${DIASDK_INCLUDE_DIR}/../lib)
        endif(CMAKE_GENERATOR MATCHES "Visual Studio.*Win64")
else(CMAKE_GENERATOR MATCHES "Visual Studio 15 2017.*")
        if((CMAKE_GENERATOR_PLATFORM STREQUAL "x64") OR("${DXC_BUILD_ARCH}" STREQUAL "x64"))
                find_library(DIASDK_GUIDS_LIBRARY NAMES diaguids.lib HINTS ${DIASDK_INCLUDE_DIR}/../lib/amd64)
        elseif((CMAKE_GENERATOR_PLATFORM STREQUAL "ARM") OR("${DXC_BUILD_ARCH}" STREQUAL "ARM"))
                find_library(DIASDK_GUIDS_LIBRARY NAMES diaguids.lib HINTS ${DIASDK_INCLUDE_DIR}/../lib/arm)
        elseif((CMAKE_GENERATOR_PLATFORM MATCHES "ARM64.*") OR("${DXC_BUILD_ARCH}" MATCHES "ARM64.*"))
                find_library(DIASDK_GUIDS_LIBRARY NAMES diaguids.lib HINTS ${DIASDK_INCLUDE_DIR}/../lib/arm64)
        else((CMAKE_GENERATOR_PLATFORM STREQUAL "x64") OR("${DXC_BUILD_ARCH}" STREQUAL "x64"))
                find_library(DIASDK_GUIDS_LIBRARY NAMES diaguids.lib HINTS ${DIASDK_INCLUDE_DIR}/../lib)
        endif((CMAKE_GENERATOR_PLATFORM STREQUAL "x64") OR("${DXC_BUILD_ARCH}" STREQUAL "x64"))
endif(CMAKE_GENERATOR MATCHES "Visual Studio 15 2017.*")

set(DIASDK_LIBRARIES ${DIASDK_GUIDS_LIBRARY})
set(DIASDK_INCLUDE_DIRS ${DIASDK_INCLUDE_DIR})
set(DIA_DLL "${DIASDK_INCLUDE_DIRS}/../bin/amd64/msdia140.dll")

include(FindPackageHandleStandardArgs)

# handle the QUIETLY and REQUIRED arguments and set DIASDK_FOUND to TRUE
# if all listed variables are TRUE
find_package_handle_standard_args(DiaSDK DEFAULT_MSG
        DIASDK_LIBRARIES DIASDK_INCLUDE_DIR)

mark_as_advanced(DIASDK_INCLUDE_DIRS DIASDK_LIBRARIES)

if(BUILD_TESTS)
        find_package(Catch2 CONFIG REQUIRED)
endif()

# #######################################################################################################################
# # Configuration for all targets
# #######################################################################################################################
if(WIN32)
        add_compile_definitions(
                UNICODE
                _UNICODE
                NOMINMAX
                _AMD64_
                WIN32_LEAN_AND_MEAN)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        add_compile_definitions(
                __cpp_lib_char8_t
                __cpp_consteval
                __cpp_lib_format)
endif()

check_ipo_supported(RESULT USE_IPO OUTPUT IPO_OUTPUT)

if(USE_IPO)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
else()
        message(WARNING "Interprocedural optimizations are not supported.")
endif()

# #######################################################################################################################
# # Configure target DLL
# #######################################################################################################################
add_commonlibsse_plugin(${PROJECT_NAME} SOURCES ${headers} ${sources})

add_library("${PROJECT_NAME}::${PROJECT_NAME}" ALIAS "${PROJECT_NAME}")

target_include_directories(${PROJECT_NAME}
        PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/src>
        $<INSTALL_INTERFACE:src>
)

if(DIASDK_LIBRARIES)
        target_include_directories(${PROJECT_NAME}
                PUBLIC
                $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                ${DIASDK_INCLUDE_DIRS}
                ${DIASDK_INCLUDE_DIRS}/../lib
        )
        target_link_libraries(${PROJECT_NAME}
                PUBLIC
                ${DIASDK_INCLUDE_DIRS}/../lib/amd64/diaguids.lib
        )
endif()

target_include_directories(${PROJECT_NAME}
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        ${CLIB_UTIL_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME}
        PRIVATE
        Boost::stacktrace_windbg
        Dbghelp.lib
        frozen::frozen
        infoware
        magic_enum::magic_enum
        Zydis::Zydis
)
target_precompile_headers(${PROJECT_NAME}
        PRIVATE
        src/PCH.h)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/Sample"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

install(TARGETS ${PROJECT_NAME}
        DESTINATION "${CMAKE_INSTALL_LIBDIR}")

# #######################################################################################################################
# # Automatic plugin deployment
# #######################################################################################################################
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(BUILD_NAME "Debug")
else()
        set(BUILD_NAME "Release")
endif()

install(DIRECTORY DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}/contrib/Distribution/Papyrus${BUILD_NAME}/")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/contrib/Distribution/Plugin${BUILD_NAME}/")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> "${CMAKE_CURRENT_SOURCE_DIR}/contrib/Distribution/Plugin${BUILD_NAME}/")

if(${CMAKE_BUILD_TYPE} STREQUAL Debug OR ${CMAKE_BUILD_TYPE} STREQUAL RelWithDebInfo OR ${CMAKE_BUILD_TYPE} STREQUAL Release)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PDB_FILE:${PROJECT_NAME}> "${CMAKE_CURRENT_SOURCE_DIR}/contrib/Distribution/Plugin${BUILD_NAME}/")
endif()

file(GLOB_RECURSE CONFIG_FILES "${CMAKE_CURRENT_SOURCE_DIR}/contrib/Distribution/Config/*.ini")
file(GLOB_RECURSE OUTPUT_DLLS "${CMAKE_CURRENT_SOURCE_DIR}/contrib/Distribution/**/*.dll")
file(GLOB_RECURSE OUTPUT_PDBS "${CMAKE_CURRENT_SOURCE_DIR}/contrib/Distribution/**/*.pdb")
file(GLOB_RECURSE OUTPUT_SCRIPTS "${CMAKE_CURRENT_SOURCE_DIR}/contrib/Distribution/**/*.pex")
set_property(TARGET ${PROJECT_NAME}
        APPEND PROPERTY ADDITIONAL_CLEAN_FILES "${OUTPUT_DLLS}")
set_property(TARGET ${PROJECT_NAME}
        APPEND PROPERTY ADDITIONAL_CLEAN_FILES "${OUTPUT_PDBS}")
set_property(TARGET ${PROJECT_NAME}
        APPEND PROPERTY ADDITIONAL_CLEAN_FILES "${OUTPUT_SCRIPTS}")

# Automatic deployment to Mod Organizer 2 mod directory.
foreach(DEPLOY_TARGET $ENV{SkyrimPluginTargets})
        message("Adding deployment target ${DEPLOY_TARGET}.")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> "${DEPLOY_TARGET}/SKSE/Plugins/")

        if(${CMAKE_BUILD_TYPE} STREQUAL Debug OR ${CMAKE_BUILD_TYPE} STREQUAL RelWithDebInfo OR ${CMAKE_BUILD_TYPE} STREQUAL Release)
                add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PDB_FILE:${PROJECT_NAME}> "${DEPLOY_TARGET}/SKSE/Plugins/")
        endif()

        if(DIASDK_LIBRARIES)
                add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy ${DIA_DLL} "${DEPLOY_TARGET}/SKSE/Plugins/")
        endif()
endforeach()

set(ZIP_DIR "${CMAKE_CURRENT_BINARY_DIR}/zip")
add_custom_target(build-time-make-directory ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ZIP_DIR}"
        "${ZIP_DIR}/SKSE/Plugins/"
)

message("Copying mod into ${ZIP_DIR}.")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> "${ZIP_DIR}/SKSE/Plugins/"
)

if(CONFIG_FILES)
        message("Copying default config files ${CONFIG_FILES} into ${ZIP_DIR}.")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy ${CONFIG_FILES} "${ZIP_DIR}/SKSE/Plugins/")
endif()

if(DIASDK_LIBRARIES)
        message("Copying ${DIA_DLL} into ${ZIP_DIR}.")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy ${DIA_DLL} "${ZIP_DIR}/SKSE/Plugins/")
endif()

if(${CMAKE_BUILD_TYPE} STREQUAL Debug OR ${CMAKE_BUILD_TYPE} STREQUAL RelWithDebInfo OR ${CMAKE_BUILD_TYPE} STREQUAL Release)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PDB_FILE:${PROJECT_NAME}> "${ZIP_DIR}/SKSE/Plugins/")
endif()

set(TARGET_ZIP "${PROJECT_NAME}_${PROJECT_VERSION}.7z")
message("Zipping ${ZIP_DIR} to ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_ZIP}.")
ADD_CUSTOM_COMMAND(
        TARGET ${PROJECT_NAME}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E tar cf ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_ZIP} --format=7zip -- .
        WORKING_DIRECTORY ${ZIP_DIR}
)

# #######################################################################################################################
# # Configure unit tests
# #######################################################################################################################
if(BUILD_TESTS)
        include(CTest)
        include(Catch)

        add_executable(
                ${PROJECT_NAME}Tests
                ${headers}
                ${tests})

        target_link_libraries(
                ${PROJECT_NAME}Tests
                PRIVATE
                ${PROJECT_NAME}
                Catch2::Catch2WithMain)

        target_precompile_headers(${PROJECT_NAME}Tests
                PRIVATE
                src/PCH.h)

        catch_discover_tests(${PROJECT_NAME}Tests)
        add_test(NAME ${PROJECT_NAME}Tests COMMAND ${PROJECT_NAME}Tests)
endif()
