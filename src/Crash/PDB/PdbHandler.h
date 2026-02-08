#pragma once

namespace Crash
{
	namespace PDB
	{
		std::string processSymbol(IDiaSymbol* symbol, IDiaSession* pSession, const DWORD& rva, std::string_view& a_name, uintptr_t& a_offset, std::string& a_result);
		std::string pdb_details(std::string_view a_name, uintptr_t a_offset);
		std::string pdb_function_parameters(std::string_view a_name, uintptr_t a_offset);
		void dump_symbols(bool exe = false);
		void dumpFileSymbols(const std::filesystem::path& path, int& retflag);
		std::string demangle(const std::wstring& mangled);  // Existing overload
		// Overload for narrow string demangling
		std::string demangle(const std::string& mangled);

		const std::string_view sPluginPath = "Data/SKSE/Plugins"sv;
		static HRESULT hr{ -1 };
	}
}
