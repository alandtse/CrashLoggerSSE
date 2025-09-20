// SPDX-License-Identifier: CC-BY-SA-4.0
// Code from StackOverflow

#pragma once
#include "PdbHandler.h"
#include "Settings.h"
#include <DbgHelp.h>
#include <atlcomcli.h>
#include <codecvt>  // For string conversions
#include <comdef.h>
#include <regex>
#include <unordered_set>

// PDB error constants - these should be defined in cvconst.h but may not be available
// If available through DIA SDK, we can use them directly
#ifndef E_PDB_USAGE
#	define E_PDB_USAGE HRESULT(0x806D0001L)
#	define E_PDB_OUT_OF_MEMORY HRESULT(0x806D0002L)
#	define E_PDB_FILE_SYSTEM HRESULT(0x806D0003L)
#	define E_PDB_NOT_FOUND HRESULT(0x806D0004L)
#	define E_PDB_INVALID_SIG HRESULT(0x806D0005L)
#	define E_PDB_INVALID_AGE HRESULT(0x806D0006L)
#	define E_PDB_PRECOMP_REQUIRED HRESULT(0x806D0007L)
#	define E_PDB_OUT_OF_TI HRESULT(0x806D0008L)
#	define E_PDB_NOT_IMPLEMENTED HRESULT(0x806D0009L)
#	define E_PDB_V1_PDB HRESULT(0x806D000AL)
#	define E_PDB_FORMAT HRESULT(0x806D000CL)
#	define E_PDB_LIMIT HRESULT(0x806D000DL)
#	define E_PDB_CORRUPT HRESULT(0x806D000EL)
#	define E_PDB_TI16 HRESULT(0x806D000FL)
#	define E_PDB_ACCESS_DENIED HRESULT(0x806D0010L)
#	define E_PDB_ILLEGAL_TYPE_EDIT HRESULT(0x806D0011L)
#	define E_PDB_INVALID_EXECUTABLE HRESULT(0x806D0012L)
#	define E_PDB_DBG_NOT_FOUND HRESULT(0x806D0013L)
#	define E_PDB_NO_DEBUG_INFO HRESULT(0x806D0014L)
#	define E_PDB_INVALID_EXE_TIMESTAMP HRESULT(0x806D0015L)
#	define E_PDB_RESERVED HRESULT(0x806D0016L)
#	define E_PDB_DEBUG_INFO_NOT_IN_PDB HRESULT(0x806D0017L)
#	define E_PDB_SYMSRV_BAD_CACHE_PATH HRESULT(0x806D0018L)
#	define E_PDB_SYMSRV_CACHE_FULL HRESULT(0x806D0019L)
#	define E_PDB_MAX HRESULT(0x806D001AL)
#endif

namespace Crash
{
	namespace PDB
	{
		std::atomic<bool> symcacheChecked = false;
		std::atomic<bool> symcacheValid = false;
		//https://stackoverflow.com/questions/6284524/bstr-to-stdstring-stdwstring-and-vice-versa
		std::string ConvertWCSToMBS(const wchar_t* pstr, long wslen)
		{
			int len = ::WideCharToMultiByte(CP_ACP, 0, pstr, wslen, NULL, 0, NULL, NULL);

			std::string dblstr(len, '\0');
			len = ::WideCharToMultiByte(CP_ACP, 0 /* no flags */,
				pstr, wslen /* not necessary NULL-terminated */,
				&dblstr[0], len,
				NULL, NULL /* no default char */);

			return dblstr;
		}

		std::string ConvertBSTRToMBS(BSTR bstr)
		{
			int wslen = ::SysStringLen(bstr);
			return ConvertWCSToMBS((wchar_t*)bstr, wslen);
		}

		BSTR ConvertMBSToBSTR(const std::string& str)
		{
			int wslen = ::MultiByteToWideChar(CP_ACP, 0 /* no flags */,
				str.data(), str.length(),
				NULL, 0);

			BSTR wsdata = ::SysAllocStringLen(NULL, wslen);
			::MultiByteToWideChar(CP_ACP, 0 /* no flags */,
				str.data(), str.length(),
				wsdata, wslen);
			return wsdata;
		}

		std::wstring utf8_to_utf16(const std::string& utf8)
		{
			std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
			return converter.from_bytes(utf8);
		}

		std::string utf16_to_utf8(const std::wstring& utf16)
		{
			std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
			return converter.to_bytes(utf16);
		}

		[[nodiscard]] static std::string trim(const std::string& str)
		{
			const auto start = str.find_first_not_of(" \t\n\r");
			const auto end = str.find_last_not_of(" \t\n\r");
			return (start == std::string::npos) ? "" : str.substr(start, end - start + 1);
		}

		std::wstring trim(const std::wstring& wstr)
		{
			auto start = wstr.begin();
			while (start != wstr.end() && std::iswspace(*start)) {
				++start;
			}

			auto end = wstr.end();
			do {
				--end;
			} while (end != start && std::iswspace(*end));

			return std::wstring(start, end + 1);
		}

		[[nodiscard]] std::string demangle(const std::wstring& mangled)
		{
			// Early return for non-mangled names (Microsoft mangled names start with '?')
			if (mangled.empty() || mangled[0] != L'?') {
				return utf16_to_utf8(mangled);
			}

			static std::mutex demangle_mutex;
			std::lock_guard lock{ demangle_mutex };

			// Use a larger buffer for complex names
			std::array<wchar_t, 0x2000> buffer{ L'\0' };

			const auto length = UnDecorateSymbolNameW(
				mangled.c_str(),
				buffer.data(),
				static_cast<DWORD>(buffer.size()),
				UNDNAME_COMPLETE |                    // Full demangling
					UNDNAME_NO_LEADING_UNDERSCORES |  // Remove leading underscores
					UNDNAME_NO_MS_KEYWORDS |          // Remove MS-specific keywords
					//UNDNAME_NO_FUNCTION_RETURNS |     // Don't show function return types
					UNDNAME_NO_ALLOCATION_MODEL |     // Remove allocation model
					UNDNAME_NO_ALLOCATION_LANGUAGE |  // Remove allocation language
					UNDNAME_NO_THISTYPE |             // Don't show 'this' type
					UNDNAME_NO_ACCESS_SPECIFIERS |    // Remove public/private/protected
					UNDNAME_NO_THROW_SIGNATURES |     // Remove throw specifications
					UNDNAME_NO_RETURN_UDT_MODEL |     // Remove return UDT model
					static_cast<DWORD>(0x8000));      // Disable enum/class/struct/union prefix

			// Check if demangling succeeded
			if (length == 0 || buffer[0] == L'\0') {
				return utf16_to_utf8(mangled);  // Failed, return original
			}

			// Ensure proper null termination
			if (length < buffer.size()) {
				buffer[length] = L'\0';
			}

			std::wstring demangled{ buffer.data() };

			// Trim whitespace
			demangled.erase(0, demangled.find_first_not_of(L" \t\r\n"));
			demangled.erase(demangled.find_last_not_of(L" \t\r\n") + 1);

			// Check for failed demangling indicators
			if (demangled.empty() ||
				demangled == L"<unknown>" ||
				demangled == L"UNKNOWN" ||
				demangled.starts_with(L"??")) {
				return utf16_to_utf8(mangled);
			}

			// For crash analysis, show both demangled and original
			return utf16_to_utf8(demangled) + " [" + utf16_to_utf8(mangled) + "]";
		}

		std::string processSymbol(IDiaSymbol* a_symbol, IDiaSession* a_session, const DWORD& a_rva, std::string_view& a_name, uintptr_t& a_offset, std::string& a_result)
		{
			BSTR name;
			a_symbol->get_name(&name);

			// Demangle the symbol name
			std::string demangledName = demangle(name);

			DWORD rva;
			if (a_rva == 0)
				a_symbol->get_relativeVirtualAddress(&rva);  // find rva if not provided
			else
				rva = a_rva;

			ULONGLONG length = 0;
			if (a_symbol->get_length(&length) == S_OK) {
				IDiaEnumLineNumbers* lineNums[100];
				if (a_session->findLinesByRVA(rva, length, lineNums) == S_OK) {
					auto& lineNumsPtr = lineNums[0];
					CComPtr<IDiaLineNumber> line;
					IDiaLineNumber* lineNum;
					ULONG fetched = 0;
					bool found_source = false;
					bool found_line = false;

					for (uint8_t i = 0; i < 5; ++i) {
						if (lineNumsPtr->Next(i, &lineNum, &fetched) == S_OK && fetched == 1) {
							found_source = false;
							found_line = false;
							DWORD sline;
							IDiaSourceFile* srcFile;
							BSTR fileName = nullptr;
							std::string convertedFileName;

							if (lineNum->get_sourceFile(&srcFile) == S_OK) {
								BSTR fileName;
								srcFile->get_fileName(&fileName);
								convertedFileName = ConvertBSTRToMBS(fileName);
								found_source = true;
							}

							if (lineNum->get_lineNumber(&sline) == S_OK)
								found_line = true;

							if (found_source && found_line)
								a_result += fmt::format(" {}:{} {}", convertedFileName, +sline ? (uint64_t)sline : 0, demangledName);
							else if (found_source)
								a_result += fmt::format(" {} {}", convertedFileName, demangledName);
							else if (found_line)
								a_result += fmt::format(" unk_:{} {}", +sline ? (uint64_t)sline : 0, demangledName);
						}
					}

					if (!found_source && !found_line) {
						auto sRva = fmt::format("{:X}", rva);
						if (demangledName.ends_with(sRva))
							sRva = "";
						else
							sRva = "_" + sRva;

						a_result += fmt::format(" {}{}", demangledName, sRva);
					}
				}
			}

			if (a_result.empty())
				logger::info("No symbol found for {}+{:07X}"sv, a_name, a_offset);
			else
				logger::info("Symbol returning: {}", a_result);

			return a_result;
		}

		std::string print_hr_failure(HRESULT hr)
		{
			auto errMsg = "";
			switch (hr) {
			// PDB-specific error codes
			case E_PDB_USAGE:
				errMsg = "Invalid PDB usage";
				break;
			case E_PDB_OUT_OF_MEMORY:
				errMsg = "Out of memory during PDB operation";
				break;
			case E_PDB_FILE_SYSTEM:
				errMsg = "File system error accessing PDB";
				break;
			case E_PDB_NOT_FOUND:
				errMsg = "PDB file not found";
				break;
			case E_PDB_INVALID_SIG:
				errMsg = "PDB signature mismatch";
				break;
			case E_PDB_INVALID_AGE:
				errMsg = "PDB age mismatch";
				break;
			case E_PDB_PRECOMP_REQUIRED:
				errMsg = "Precompiled header required";
				break;
			case E_PDB_OUT_OF_TI:
				errMsg = "Out of type indices";
				break;
			case E_PDB_NOT_IMPLEMENTED:
				errMsg = "PDB feature not implemented";
				break;
			case E_PDB_V1_PDB:
				errMsg = "Unsupported PDB v1.0 format";
				break;
			case E_PDB_FORMAT:
				errMsg = "Invalid PDB format";
				break;
			case E_PDB_LIMIT:
				errMsg = "PDB internal limit exceeded";
				break;
			case E_PDB_CORRUPT:
				errMsg = "PDB file is corrupted";
				break;
			case E_PDB_TI16:
				errMsg = "PDB 16-bit type index not supported";
				break;
			case E_PDB_ACCESS_DENIED:
				errMsg = "Access denied to PDB file";
				break;
			case E_PDB_ILLEGAL_TYPE_EDIT:
				errMsg = "Illegal type edit in PDB";
				break;
			case E_PDB_INVALID_EXECUTABLE:
				errMsg = "Invalid executable format for PDB";
				break;
			case E_PDB_DBG_NOT_FOUND:
				errMsg = "DBG file not found";
				break;
			case E_PDB_NO_DEBUG_INFO:
				errMsg = "No debug information available";
				break;
			case E_PDB_INVALID_EXE_TIMESTAMP:
				errMsg = "Executable timestamp mismatch";
				break;
			case E_PDB_RESERVED:
				errMsg = "Reserved PDB error";
				break;
			case E_PDB_DEBUG_INFO_NOT_IN_PDB:
				errMsg = "Debug info not in PDB format";
				break;
			case E_PDB_SYMSRV_BAD_CACHE_PATH:
				errMsg = "Bad symbol server cache path";
				break;
			case E_PDB_SYMSRV_CACHE_FULL:
				errMsg = "Symbol server cache full";
				break;
			case E_PDB_MAX:
				errMsg = "Maximum PDB error reached";
				break;
			// Common HRESULT codes
			case E_INVALIDARG:
				errMsg = "Invalid argument passed to PDB function";
				break;
			case E_OUTOFMEMORY:
				errMsg = "Out of memory";
				break;
			case E_FAIL:
				errMsg = "Unspecified PDB failure";
				break;
			case E_NOTIMPL:
				errMsg = "PDB function not implemented";
				break;
			case E_NOINTERFACE:
				errMsg = "PDB interface not supported";
				break;
			case E_ACCESSDENIED:
				errMsg = "Access denied to PDB resources";
				break;
			default:
				_com_error err(hr);
				errMsg = CT2A(err.ErrorMessage());
				break;
			}
			return errMsg;
		}

		//https://stackoverflow.com/questions/68412597/determining-source-code-filename-and-line-for-function-using-visual-studio-pdb
		std::string pdb_details(std::string_view a_name, uintptr_t a_offset)
		{
			static std::mutex sync;
			std::lock_guard l{ sync };
			std::string result;

			std::filesystem::path dllPath{ a_name };
			std::string dll_path = a_name.data();
			if (!dllPath.has_parent_path()) {
				dll_path = Crash::PDB::sPluginPath.data() + dllPath.filename().string();
			}

			auto rva = static_cast<DWORD>(a_offset);
			CComPtr<IDiaDataSource> pSource;
			HRESULT hr = S_OK;

			// Initialize COM - handle the case where it's already initialized
			hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
			if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
				// Only fail if it's not already initialized with a different mode
				auto error = print_hr_failure(hr);
				logger::info("Failed to initialize COM library for dll {}+{:07X}\t{}", a_name, a_offset, error);
				return result;
			}
			
			// Track if we need to uninitialize COM later
			bool com_initialized_here = SUCCEEDED(hr);

			// Attempt to load msdia140.dll
			auto* msdia_dll = L"Data/SKSE/Plugins/msdia140.dll";
			hr = NoRegCoCreate(msdia_dll, CLSID_DiaSource, __uuidof(IDiaDataSource), (void**)&pSource);
			if (FAILED(hr)) {
				auto error = print_hr_failure(hr);
				logger::info("Failed to manually load msdia140.dll for dll {}+{:07X}\t{}", a_name, a_offset, error);

				// Try registered copy
				if (FAILED(hr = CoCreateInstance(CLSID_DiaSource, NULL, CLSCTX_INPROC_SERVER, __uuidof(IDiaDataSource), (void**)&pSource))) {
					auto error = print_hr_failure(hr);
					logger::info("Failed to load registered msdia140.dll for dll {}+{:07X}\t{}", a_name, a_offset, error);
					if (com_initialized_here) CoUninitialize();
					return result;
				}
			}

			wchar_t wszFilename[_MAX_PATH];
			wchar_t wszPath[_MAX_PATH];

			// Convert UTF-8 dll_path to UTF-16
			std::wstring dll_path_w = utf8_to_utf16(dll_path);
			wcsncpy(wszFilename, dll_path_w.c_str(), sizeof(wszFilename) / sizeof(wchar_t));

			const auto& debugConfig = Settings::GetSingleton()->GetDebug();
			std::string symcache = debugConfig.symcache;

			// Symcache handling (already UTF-8)
			static bool symcacheChecked = false;
			static bool symcacheValid = false;

			if (!symcacheChecked) {
				if (!symcache.empty() && std::filesystem::exists(symcache) && std::filesystem::is_directory(symcache)) {
					logger::info("Symcache found at {}", symcache);
					symcacheValid = true;
				} else {
					logger::info("Symcache not found at {}", symcache.empty() ? "not defined" : symcache);
				}
				symcacheChecked = true;
			}

			std::vector<std::string> searchPaths = { Crash::PDB::sPluginPath.data() };

			if (symcacheValid) {
				searchPaths.push_back(fmt::format(fmt::runtime("cache*{}"s), symcache.c_str()));
			}

			bool foundPDB = false;
			for (const auto& path : searchPaths) {
				// Convert UTF-8 path to UTF-16
				std::wstring path_w = utf8_to_utf16(path);
				wcsncpy(wszPath, path_w.c_str(), sizeof(wszPath) / sizeof(wchar_t));

				logger::info("Attempting to find pdb for {}+{:07X} with path {}", a_name, a_offset, path);
				hr = pSource->loadDataForExe(wszFilename, wszPath, NULL);
				if (FAILED(hr)) {
					auto error = print_hr_failure(hr);
					logger::info("Failed to open pdb for dll {}+{:07X}\t{}", a_name, a_offset, error);
					continue;
				}
				foundPDB = true;
				break;
			}

			if (!foundPDB) {
				if (com_initialized_here) CoUninitialize();
				return result;
			}

			logger::info("Successfully opened pdb for dll {}+{:07X}", a_name, a_offset);

			// Rest of the PDB processing logic remains the same
			// No other changes needed for UTF-8 handling
			CComPtr<IDiaSession> pSession;
			CComPtr<IDiaSymbol> globalSymbol;
			CComPtr<IDiaEnumTables> enumTables;
			CComPtr<IDiaEnumSymbolsByAddr> enumSymbolsByAddr;

			if (FAILED(hr = pSource->openSession(&pSession))) {
				auto error = print_hr_failure(hr);
				logger::info("Failed to open IDiaSession for pdb for dll {}+{:07X}\t{}", a_name, a_offset, error);
				if (com_initialized_here) CoUninitialize();
				return result;
			}

			if (FAILED(hr = pSession->get_globalScope(&globalSymbol))) {
				auto error = print_hr_failure(hr);
				logger::info("Failed to getGlobalScope for pdb for dll {}+{:07X}\t{}", a_name, a_offset, error);
				if (com_initialized_here) CoUninitialize();
				return result;
			}

			if (FAILED(hr = pSession->getEnumTables(&enumTables))) {
				auto error = print_hr_failure(hr);
				logger::info("Failed to getEnumTables for pdb for dll {}+{:07X}\t{}", a_name, a_offset, error);
				if (com_initialized_here) CoUninitialize();
				return result;
			}

			if (FAILED(hr = pSession->getSymbolsByAddr(&enumSymbolsByAddr))) {
				auto error = print_hr_failure(hr);
				logger::info("Failed to getSymbolsByAddr for pdb for dll {}+{:07X}\t{}", a_name, a_offset, error);
				if (com_initialized_here) CoUninitialize();
				return result;
			}

			CComPtr<IDiaSymbol> publicSymbol;
			if (pSession->findSymbolByRVA(rva, SymTagEnum::SymTagPublicSymbol, &publicSymbol) == S_OK) {
				auto publicResult = processSymbol(publicSymbol, pSession, rva, a_name, a_offset, result);

				// Log the public result (already demangled in processSymbol)
				logger::info("Public symbol found for {}+{:07X}: {}", a_name, a_offset, publicResult);

				DWORD privateRva;
				CComPtr<IDiaSymbol> privateSymbol;
				if (publicSymbol->get_targetRelativeVirtualAddress(&privateRva) == S_OK &&
					pSession->findSymbolByRVA(privateRva, SymTagEnum::SymTagFunction, &privateSymbol) == S_OK) {
					auto privateResult = processSymbol(privateSymbol, pSession, privateRva, a_name, a_offset, result);

					// Log the private result (already demangled in processSymbol)
					logger::info("Private symbol found for {}+{:07X}: {}", a_name, a_offset, privateResult);

					// Combine results
					if (!privateResult.empty() && !publicResult.empty()) {
						result = fmt::format("{}\t{}", privateResult, publicResult);
					} else if (!privateResult.empty()) {
						result = privateResult;
					} else {
						result = publicResult;
					}
				} else {
					result = publicResult;
				}
			} else {
				logger::info("No public symbol found for {}+{:07X}", a_name, a_offset);
			}

			if (com_initialized_here) CoUninitialize();
			return result;
		}

		// dump all symbols in Plugin directory or fakepdb for exe
		// this was the early POC test and written first in this module
		void dump_symbols(bool exe)
		{
			// Initialize COM - handle the case where it's already initialized
			HRESULT com_hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
			bool com_initialized_here = SUCCEEDED(com_hr);
			
			// RPC_E_CHANGED_MODE means COM is already initialized with different threading mode
			if (FAILED(com_hr) && com_hr != RPC_E_CHANGED_MODE) {
				logger::error("Failed to initialize COM for symbol dumping: {}", print_hr_failure(com_hr));
				return;
			}
			int retflag;
			if (exe) {
				const auto string_path = "./SkyrimVR.exe";
				std::filesystem::path file_path{ string_path };
				dumpFileSymbols(file_path, retflag);
			} else {
				for (const auto& elem : std::filesystem::directory_iterator(Crash::PDB::sPluginPath)) {
					if (const auto filename =
							elem.path().has_filename() ?
								std::make_optional(elem.path().filename().string()) :
								std::nullopt;
						filename.value().ends_with("dll")) {
						dumpFileSymbols(elem.path(), retflag);
						if (retflag == 3)
							continue;
					}
				}
			}
		}
		void dumpFileSymbols(const std::filesystem::path& path, int& retflag)
		{
			retflag = 1;
			const auto filename = std::make_optional(path.filename().string());
			logger::info("Found dll {}", *filename);
			auto dll_path = path.string();
			auto search_path = Crash::PDB::sPluginPath.data();
			CComPtr<IDiaDataSource> source;
			auto hr = CoCreateInstance(CLSID_DiaSource,
				NULL,
				CLSCTX_INPROC_SERVER,
				__uuidof(IDiaDataSource),
				(void**)&source);
			if (FAILED(hr)) {
				retflag = 3;
				return;
			};

			{
				wchar_t wszFilename[_MAX_PATH];
				wchar_t wszPath[_MAX_PATH];
				mbstowcs(wszFilename, dll_path.c_str(), sizeof(wszFilename) / sizeof(wszFilename[0]));
				mbstowcs(wszPath, sPluginPath.data(), sizeof(wszPath) / sizeof(wszPath[0]));
				hr = source->loadDataForExe(wszFilename, wszPath, NULL);
				if (FAILED(hr)) {
					retflag = 3;
					return;
				};
				logger::info("Found pdb for dll {}", *filename);
			}

			CComPtr<IDiaSession> pSession;
			if (FAILED(source->openSession(&pSession))) {
				retflag = 3;
				return;
			};

			IDiaEnumSymbolsByAddr* pEnumSymbolsByAddr;
			IDiaSymbol* pSymbol;
			ULONG celt = 0;
			if (FAILED(pSession->getSymbolsByAddr(&pEnumSymbolsByAddr))) {
				{
					retflag = 3;
					return;
				};
			}
			if (FAILED(pEnumSymbolsByAddr->symbolByAddr(1, 0, &pSymbol))) {
				pEnumSymbolsByAddr->Release();
				{
					retflag = 3;
					return;
				};
			}
			do {
				const auto rva = 0;
				std::string_view a_name = *filename;
				uintptr_t a_offset = 0;
				std::string result = "";
				result = processSymbol(pSymbol, pSession, rva, a_name, a_offset, result);
				logger::info("{}", result);
				pSymbol->Release();
				if (FAILED(pEnumSymbolsByAddr->Next(1, &pSymbol, &celt))) {
					pEnumSymbolsByAddr->Release();
					break;
				}
			} while (celt == 1);
			pEnumSymbolsByAddr->Release();
		}
	}
}
