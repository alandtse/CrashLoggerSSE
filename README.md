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
* [Visual Studio Community 2022](https://visualstudio.microsoft.com/)
	* Desktop development with C++
* [CommonLibSSE](https://github.com/Ryan-rsm-McKenzie/CommonLibSSE)
	* Add this as as an environment variable `CommonLibSSEPath` or use submodule in `/external`
* [CommonLibVR](https://github.com/alandtse/CommonLibVR/tree/vr)
	* Add this as as an environment variable `CommonLibVRPath`

## User Requirements
* [Address Library for SKSE](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
	* Needed for AE
* [VR Address Library for SKSEVR](https://www.nexusmods.com/skyrimspecialedition/mods/58101)
	* Needed for VR

## Register Visual Studio as a Generator
* Open `x64 Native Tools Command Prompt`
* Run `cmake`
* Close the cmd window

## Building
```
git clone https://github.com/Ryan-rsm-McKenzie/CrashLoggerSSE.git
cd CrashLoggerSSE
```
### CommonLibSSE/CommonLibVR
```
# pull CommonLibSSE and CommonLibVR
# alternatively, do not pull and set environment variable `CommonLibSSEPath` or `CommonLibVRPath` if you need something different from external

git submodule update --init --recursive

```
### SSE
```
cmake -B build -S . -DVCPKG_TARGET_TRIPLET=x64-windows-static-md
```
Open build/CrashLogger.sln in Visual Studio to build dll.
### VR
```
cmake -B build2 -S . -DVCPKG_TARGET_TRIPLET=x64-windows-static-md -DBUILD_SKYRIMVR=On
```
Open build2/CrashLogger.sln in Visual Studio to build dll.

## License
[MIT](LICENSE)

# Credits
 * Ryan-rsm-McKenzie - Original code