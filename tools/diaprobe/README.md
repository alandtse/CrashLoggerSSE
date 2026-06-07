# diaprobe

A tiny DIA tool that resolves a PDB's public symbols by RVA **exactly the way CrashLogger does**
(`loadDataFromPdb` → `openSession` → `findSymbolByRVA(rva, SymTagPublicSymbol)`).

Use it to verify a freshly generated PDB (from [pdbgen](../../scripts/package_skyrim_pdbs.py))
actually resolves **before** packaging it. This catches a failure mode a name/string dump (or
`llvm-pdbutil --publics`) cannot: a PDB can contain public-symbol *names* yet still fail
`findSymbolByRVA` when its section headers / section map are malformed (e.g. the large-PE
`SectionHdr` corruption that produced "No public symbol found" for every VR frame).

## Build

From an x64 Native Tools prompt (or after `vcvars64`):

```bat
cl /nologo /EHsc /std:c++17 probe.cpp ^
   /I"%VSINSTALLDIR%DIA SDK\include" ^
   /link "%VSINSTALLDIR%DIA SDK\lib\amd64\diaguids.lib" ole32.lib oleaut32.lib advapi32.lib
```

## Usage

```bat
probe <pdb> <rva-hex> [<rva-hex> ...]
```

`rva-hex` is the module-relative offset CrashLogger prints (`SkyrimVR.exe+0CBFD2A` -> `CBFD2A`).
Exit code is 0 only if every RVA resolved.

```text
> probe "SkyrimVR.pdb" CBFD2A C9E13B
PDB: SkyrimVR.pdb
  RVA 0xCBFD2A -> PUBLIC 'BSCullingProcess::sub_140C79410' (@0xCBFC60)
  RVA 0xC9E13B -> PUBLIC 'NiNode::OnVisible_140C58C70' (@0xC9E0D0)
```

`msdia140.dll` is located via `%MSDIA140_DLL%`, then `msdia140.dll` on PATH/cwd, then the
Visual Studio DIA SDK default. Set `MSDIA140_DLL` to override.
