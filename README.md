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
* [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG)


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
```
Open folder in Visual Studio and build. If `SkyrimPluginTargets` is set, then compiled dlls/pdb will be copied to `${SkyrimPluginTargets}/SKSE/Plugins/`.
## License
[MIT](LICENSE)

# Credits
 * [Ryan-rsm-McKenzie](https://github.com/Ryan-rsm-McKenzie) - Original code and CommonlibSSE
 * [CharmedBaryon](https://github.com/CharmedBaryon) - [CommonlibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG) and [Sample Plugin Template](https://gitlab.com/colorglass/commonlibsse-sample-plugin)

