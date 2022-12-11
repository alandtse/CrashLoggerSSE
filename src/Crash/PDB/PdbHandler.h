#pragma once

namespace Crash
{
	namespace PDB
	{
		std::string processSymbol(IDiaSymbol* symbol, IDiaSession* pSession, const DWORD& rva, std::string_view& a_name, uintptr_t& a_offset, std::string& a_result);
		std::string pdb_details(std::string_view a_name, uintptr_t a_offset);

		const std::string_view sPluginPath = "Data/SKSE/Plugins"sv;
	}
}

