// diaprobe — verify a PDB resolves public symbols by RVA the way CrashLogger does.
//
// CrashLogger's PdbHandler resolves call-stack frames with DIA:
//   loadDataForExe / loadDataFromPdb -> openSession -> findSymbolByRVA(rva, SymTagPublicSymbol).
// This tool reproduces exactly that lookup so you can confirm a freshly generated PDB
// (e.g. from pdbgen) actually resolves before packaging it. A PDB can contain public-symbol
// *names* (visible to llvm-pdbutil / a string dump) yet still fail findSymbolByRVA if its
// section headers / section map are malformed — this probe catches that, a name dump does not.
//
// Build (x64 Native Tools / vcvars64):
//   cl /nologo /EHsc /std:c++17 probe.cpp ^
//      /I"%VSINSTALLDIR%DIA SDK\include" ^
//      /link "%VSINSTALLDIR%DIA SDK\lib\amd64\diaguids.lib" ole32.lib oleaut32.lib advapi32.lib
//
// Usage:
//   probe <pdb> <rva-hex> [<rva-hex> ...]
// e.g.
//   probe SkyrimVR.pdb CBFD2A C9E13B
//
// msdia140.dll is located via (in order): %MSDIA140_DLL%, msdia140.dll on PATH/cwd,
// then the Visual Studio DIA SDK default. Override with MSDIA140_DLL if needed.
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <dia2.h>
#include <diacreate.h>

static IDiaDataSource* create_source()
{
	const wchar_t* candidates[] = {
		_wgetenv(L"MSDIA140_DLL"),
		L"msdia140.dll",
		L"F:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\DIA SDK\\bin\\amd64\\msdia140.dll",
	};
	for (const wchar_t* dll : candidates) {
		if (!dll || !*dll) {
			continue;
		}
		IDiaDataSource* source = nullptr;
		if (SUCCEEDED(NoRegCoCreate(dll, __uuidof(DiaSource), __uuidof(IDiaDataSource), (void**)&source))) {
			return source;
		}
	}
	return nullptr;
}

int wmain(int argc, wchar_t** argv)
{
	if (argc < 3) {
		wprintf(L"usage: probe <pdb> <rva-hex> [<rva-hex> ...]\n");
		return 2;
	}
	if (FAILED(CoInitialize(nullptr))) {
		wprintf(L"CoInitialize failed\n");
		return 1;
	}

	IDiaDataSource* source = create_source();
	if (!source) {
		wprintf(L"could not load msdia140.dll (set MSDIA140_DLL to its full path)\n");
		return 1;
	}

	HRESULT hr = source->loadDataFromPdb(argv[1]);
	if (FAILED(hr)) {
		wprintf(L"loadDataFromPdb('%s') failed 0x%08lx\n", argv[1], hr);
		return 1;
	}

	IDiaSession* session = nullptr;
	if (FAILED(hr = source->openSession(&session))) {
		wprintf(L"openSession failed 0x%08lx\n", hr);
		return 1;
	}

	wprintf(L"PDB: %s\n", argv[1]);
	int unresolved = 0;
	for (int i = 2; i < argc; ++i) {
		DWORD rva = (DWORD)wcstoul(argv[i], nullptr, 16);
		IDiaSymbol* sym = nullptr;
		hr = session->findSymbolByRVA(rva, SymTagPublicSymbol, &sym);
		if (hr == S_OK && sym) {
			BSTR name = nullptr;
			sym->get_name(&name);
			DWORD symrva = 0;
			sym->get_relativeVirtualAddress(&symrva);
			wprintf(L"  RVA 0x%06X -> PUBLIC '%s' (@0x%06X)\n", rva, name ? name : L"<null>", symrva);
			if (name) {
				SysFreeString(name);
			}
			sym->Release();
		} else {
			wprintf(L"  RVA 0x%06X -> NO PUBLIC SYMBOL (hr=0x%08lx)\n", rva, hr);
			++unresolved;
		}
	}
	return unresolved == 0 ? 0 : 1;
}
