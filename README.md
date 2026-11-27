# CrashLoggerSSE

SKSE/SKSEVR plugin that generates crash logs when the game Just Works™.
*	[AE Version](https://www.nexusmods.com/skyrimspecialedition/mods/59596)
*	[VR Version](https://www.nexusmods.com/skyrimspecialedition/mods/59818)

## Requirements
* [CMake](https://cmake.org/)
	* Add this to your `PATH`
* [PowerShell](https://github.com/PowerShell/PowerShell/releases/latest)
* [Vcpkg](https://github.com/microsoft/vcpkg)
	* Add the environment variable `VCPKG_ROOT` with the value as the path to the folder containing vcpkg
* [Visual Studio Community 2022 or Visual Studio 2026](https://visualstudio.microsoft.com/)
	* Desktop development with C++ and the x64/x86 MSVC tools
	* If selecting a side-by-side MSVC toolset, install its matching ATL component; the PDB handler requires ATL headers.
* [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG)


## User Requirements
* [Address Library for SKSE](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
	* Needed for AE
* [VR Address Library for SKSEVR](https://www.nexusmods.com/skyrimspecialedition/mods/58101)
	* Needed for VR

## Building

The `Release-MSVC` CMake preset uses Ninja and the vcpkg manifest. Set `VCPKG_ROOT` to your vcpkg checkout, then open an x64 Native Tools Command Prompt for the Visual Studio instance you want to use. To select a side-by-side toolset explicitly, initialize that prompt with (for example):

```
call "%VSINSTALLDIR%\Common7\Tools\VsDevCmd.bat" -no_logo -arch=x64 -vcvars_ver=14.52
```

Use the version installed in that Visual Studio instance (for example, `14.51` or `14.52`). From the repository root, initialize submodules, then configure and build:

```
git submodule update --init --recursive
cmake --preset Release-MSVC
cmake --build --preset Release-MSVC --target CrashLogger --parallel 4
```

If `SkyrimPluginTargets` is set, the build also copies the compiled DLL/PDB to `${SkyrimPluginTargets}/SKSE/Plugins/`. Clear that environment variable if you do not want the build to deploy to your game folders.
## License
[GPL-3.0-or-later](COPYING) WITH [Modding Exception AND GPL-3.0 Linking Exception (with Corresponding Source)](EXCEPTIONS.md).
Specifically, the Modded Code is Skyrim (and its variants) and Modding Libraries include [SKSE](https://skse.silverlock.org/), Commonlib (and variants), and Windows.

Originally based on code under [MIT](LICENSE)

PDB requires `msdia140.dll` distributed under [Visual Studio C++ Redistributable](https://docs.microsoft.com/en-us/visualstudio/releases/2022/redistribution#dia-sdk)

[PDB Handler](src/Crash/PDB/PdbHandler.cpp) derived from StackOverflow code.

# Credits
 * [Ryan-rsm-McKenzie](https://github.com/Ryan-rsm-McKenzie) - Original code and CommonlibSSE
 * [CharmedBaryon](https://github.com/CharmedBaryon) - [CommonlibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG) and [Sample Plugin Template](https://gitlab.com/colorglass/commonlibsse-sample-plugin)
