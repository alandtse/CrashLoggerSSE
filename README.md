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
	* You need to build from the vr branch
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
git clone https://github.com/alandtse/CrashLoggerSSE.git
cd CrashLoggerSSE
# pull commonlib /extern to override the path settings
git submodule init
# to update submodules to checked in build
git submodule update
```

### SSE
```
cmake --preset vs2022-windows-vcpkg
cmake --build build --config Release
```
### VR
```
cmake --preset vs2022-windows-vcpkg-vr
cmake --build buildvr --config Release
```
## License
[MIT](LICENSE)

# Credits
 * Ryan-rsm-McKenzie - Original code