# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

CrashLoggerSSE is an SKSE/SKSEVR plugin that generates crash logs when Skyrim "Just Works™". This is a C++23 project that provides detailed crash analysis and reporting for Skyrim Special Edition and VR.

## Build System and Commands

The project uses CMake with vcpkg for package management and provides multiple build configurations:

### Building
```bash
# Configure and build with CMake presets
cmake --preset Release-MSVC
cmake --build --preset Release-MSVC

# Or configure and build with specific preset
cmake --preset Debug-MSVC
cmake --build --preset Debug-MSVC

# Build with Clang
cmake --preset Release-Clang
cmake --build --preset Release-Clang
```

### Testing
```bash
# Run all tests
ctest --preset All-Tests

# Run specific test categories
ctest --preset Unit-Tests
ctest --preset Integration-Tests
ctest --preset E2E-Tests
```

### Visual Studio Integration
- Open the folder in Visual Studio to use integrated CMake support
- The project is configured to automatically copy built DLLs/PDBs to `${SkyrimPluginTargets}/SKSE/Plugins/` if the environment variable is set

## Dependencies and Requirements

### Development Requirements
- CMake 3.21+
- Visual Studio Community 2022 with Desktop development with C++
- vcpkg (set `VCPKG_ROOT` environment variable)
- PowerShell

### Key Dependencies (managed via vcpkg)
- CommonLibSSE-NG (core SKSE functionality)
- Boost stacktrace (for crash stack traces)
- DirectX Toolkit
- Magic enum
- Zydis (disassembly)
- DIA SDK (PDB symbol resolution)

## Code Architecture

### Core Components

**Main Entry Point (`src/main.cpp:217`)**
- `SKSEPluginLoad()` - Primary plugin initialization
- Sets up crash handler, logging, and PDB symbol resolution

**Crash Handling System (`src/Crash/`)**
- `CrashHandler` - Main crash interception and reporting
- `Callstack` - Stack trace generation and analysis
- `Introspection/` - Runtime code analysis
- `Modules/ModuleHandler` - Loaded module tracking
- `PDB/PdbHandler` - Symbol resolution from PDB files

**Configuration (`src/Settings.*`)**
- INI-based configuration system
- Debug settings including log levels and crash directory paths

### Key Features
- Automatic crash detection and logging
- Detailed stack traces with symbol resolution
- Module information and loaded library analysis
- Configurable crash log output directory
- Integration with Skyrim's SKSE plugin system

## Development Workflow

### Project Structure
- `src/` - Source code organized by functionality
- `contrib/Distribution/` - Plugin deployment artifacts
- `cmake/` - Build configuration and vcpkg port overrides
- CMake automatically packages built plugins into 7z archives

### Plugin Deployment
- Built plugins are automatically copied to deployment directories
- Supports automatic deployment to Mod Organizer 2 via `SkyrimPluginTargets` environment variable
- Creates distribution packages in `contrib/Distribution/`

### Symbol Resolution
The project requires DIA SDK for PDB symbol resolution, automatically located from Visual Studio installation.
