#include "Crash/MissingMasters.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace Crash::MissingMasters
{
	namespace
	{
		// The lists can be corrupt in an unrelated crash, so every walk is bounded.
		constexpr std::size_t MAX_FILES = 4096;
		constexpr std::size_t MAX_MASTERS = 1024;
		constexpr std::size_t MAX_DEPTH = 64;
		constexpr std::size_t MAX_CHAINS = 32;

		struct Plugin
		{
			std::string name;
			std::vector<std::string> masters;
			bool enabled{ false };
		};

		using PluginMap = std::unordered_map<std::string, Plugin>;

		std::string lower(std::string_view a_str)
		{
			std::string result(a_str);
			std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return result;
		}

		std::string trim(std::string_view a_str)
		{
			while (!a_str.empty() && std::isspace(static_cast<unsigned char>(a_str.front())))
				a_str.remove_prefix(1);
			while (!a_str.empty() && std::isspace(static_cast<unsigned char>(a_str.back())))
				a_str.remove_suffix(1);
			return std::string(a_str);
		}

		std::unordered_set<std::string> read_enabled_plugins()
		{
			std::unordered_set<std::string> enabled;

			wchar_t buffer[MAX_PATH]{};
			const auto len = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
			if (len == 0 || len >= MAX_PATH)
				return enabled;

			const auto folder = REL::Module::IsVR() ? L"Skyrim VR" : L"Skyrim Special Edition";
			std::ifstream file(std::filesystem::path(buffer) / folder / L"Plugins.txt");
			std::string line;
			while (std::getline(file, line)) {
				line = trim(line);
				if (line.empty() || line.front() != '*')
					continue;
				enabled.insert(lower(trim(std::string_view(line).substr(1))));
			}
			return enabled;
		}

		// ReadHeader runs lazily during the master walk, so `masters` can still be empty for
		// files the aborted load never reached. Fall back to parsing the TES4 header on disk.
		std::vector<std::string> read_masters_from_disk(const char* a_name)
		{
			std::vector<std::string> masters;

			std::ifstream file(std::filesystem::path(L"Data") / std::filesystem::u8path(a_name), std::ios::binary);
			char header[24]{};
			if (!file.read(header, sizeof(header)) || std::memcmp(header, "TES4", 4) != 0)
				return masters;

			std::uint32_t dataSize = 0;
			std::memcpy(&dataSize, header + 4, sizeof(dataSize));

			std::string data(std::min<std::size_t>(dataSize, 1u << 20), '\0');
			if (!file.read(data.data(), static_cast<std::streamsize>(data.size())))
				return masters;

			for (std::size_t pos = 0; pos + 6 <= data.size() && masters.size() < MAX_MASTERS;) {
				std::uint16_t size = 0;
				std::memcpy(&size, data.data() + pos + 4, sizeof(size));
				if (pos + 6 + size > data.size())
					break;
				if (std::memcmp(data.data() + pos, "MAST", 4) == 0)
					masters.emplace_back(data.c_str() + pos + 6, strnlen(data.c_str() + pos + 6, size));
				pos += 6 + size;
			}
			return masters;
		}

		PluginMap build_map()
		{
			PluginMap map;
			const auto enabled = read_enabled_plugins();

			const auto datahandler = RE::TESDataHandler::GetSingleton();
			if (!datahandler)
				return map;

			std::size_t fileCount = 0;
			for (const auto file : datahandler->files) {
				if (!file || ++fileCount > MAX_FILES)
					break;

				Plugin plugin;
				plugin.name = file->fileName;
				if (plugin.name.empty())
					continue;

				std::size_t masterCount = 0;
				for (const auto master : file->masters) {
					if (!master || ++masterCount > MAX_MASTERS)
						break;
					plugin.masters.emplace_back(master);
				}

				if (plugin.masters.empty())
					plugin.masters = read_masters_from_disk(file->fileName);

				const auto key = lower(plugin.name);
				plugin.enabled = enabled.contains(key);
				map.emplace(key, std::move(plugin));
			}
			return map;
		}

		struct Walker
		{
			const PluginMap& map;
			std::vector<std::string> chains;
			std::map<std::string, std::string> missing;
			std::unordered_set<std::string> affected;
			std::unordered_set<std::string> done;

			bool visit(const std::string& a_key, std::vector<std::string>& a_path)
			{
				if (a_path.size() > MAX_DEPTH)
					return false;
				if (done.contains(a_key))
					return affected.contains(a_key);

				// A cycle is not a failure in itself; the game treats a revisited file as OK.
				if (std::ranges::find(a_path, map.at(a_key).name) != a_path.end())
					return false;

				const auto& plugin = map.at(a_key);
				a_path.push_back(plugin.name);

				bool bad = false;
				for (const auto& master : plugin.masters) {
					const auto masterKey = lower(master);
					if (!map.contains(masterKey)) {
						bad = true;
						missing.emplace(masterKey, master);
						if (chains.size() < MAX_CHAINS) {
							std::string chain;
							for (const auto& name : a_path)
								chain += name + " -> ";
							chains.push_back(chain + master + ": NOT FOUND");
						}
					} else if (visit(masterKey, a_path)) {
						bad = true;
					}
				}

				a_path.pop_back();
				done.insert(a_key);
				if (bad)
					affected.insert(a_key);
				return bad;
			}
		};
	}

	std::vector<std::string> Diagnose()
	{
		std::vector<std::string> lines;

		const auto datahandler = RE::TESDataHandler::GetSingleton();
		if (!datahandler)
			return lines;

		// Only a failed load leaves nothing compiled; any normal crash has loaded plugins.
		if (datahandler->GetLoadedModCount() != 0 || datahandler->GetLoadedLightModCount() != 0)
			return lines;

		const auto map = build_map();
		if (map.empty())
			return lines;

		// Mirrors BuildLoadOrder: an enabled plugin with an absent DIRECT master is never checked
		// (silently skipped). Only checked plugins start the recursive walk, which then follows
		// masters through any present file; a dead end there aborts the whole load.
		const auto hasAbsentMaster = [&](const Plugin& a_plugin) {
			return std::ranges::any_of(a_plugin.masters, [&](const auto& m) { return !map.contains(lower(m)); });
		};
		const auto anyEnabled = std::ranges::any_of(map, [](const auto& kv) { return kv.second.enabled; });

		Walker walker{ map };
		std::vector<std::string> skipped;
		for (const auto& [key, plugin] : map) {
			// Without a readable Plugins.txt we can't tell enabled from not, so walk everything.
			if (anyEnabled && !plugin.enabled)
				continue;
			if (anyEnabled && hasAbsentMaster(plugin))
				continue;
			std::vector<std::string> path;
			walker.visit(key, path);
		}
		if (walker.chains.empty())
			return lines;

		for (const auto& [key, plugin] : map) {
			if (anyEnabled && plugin.enabled && hasAbsentMaster(plugin) && !walker.done.contains(key))
				skipped.push_back(plugin.name);
		}
		std::ranges::sort(skipped);

		std::ranges::sort(walker.chains);

		lines.emplace_back("MISSING MASTERS (FATAL: data load aborted before any plugin was loaded):");
		for (const auto& chain : walker.chains)
			lines.push_back("\t" + chain);

		std::unordered_set<std::string> hasAffectedDependent;
		for (const auto& key : walker.affected) {
			for (const auto& master : map.at(key).masters) {
				const auto masterKey = lower(master);
				if (walker.affected.contains(masterKey))
					hasAffectedDependent.insert(masterKey);
			}
		}

		const auto anyAffectedEnabled = std::ranges::any_of(walker.affected, [&](const auto& key) { return map.at(key).enabled; });

		std::vector<std::string> roots;
		std::vector<std::string> others;
		for (const auto& key : walker.affected) {
			const auto& plugin = map.at(key);
			// A disabled master is silently re-enabled by its dependents, so only a plugin the
			// user enabled is actionable; everything else on the chain is an intermediate.
			const auto actionable = plugin.enabled || (!hasAffectedDependent.contains(key) && !anyAffectedEnabled);
			(actionable ? roots : others).push_back(plugin.name + (plugin.enabled ? " (enabled in Plugins.txt)" : " (pulled in as a master)"));
		}
		std::ranges::sort(roots);
		std::ranges::sort(others);

		std::vector<std::string> missingNames;
		for (const auto& [key, name] : walker.missing)
			missingNames.push_back(name);
		std::ranges::sort(missingNames);

		lines.emplace_back("\tFix: install the missing file(s), or disable the plugin(s) that depend on them:");
		for (const auto& name : missingNames)
			lines.push_back("\t\tmissing: " + name);
		for (const auto& name : roots)
			lines.push_back("\t\tdisable: " + name);
		for (const auto& name : others)
			lines.push_back("\t\tintermediate: " + name);

		if (!skipped.empty()) {
			lines.emplace_back("\tNot fatal alone (direct master absent, game skips the plugin):");
			for (const auto& name : skipped)
				lines.push_back("\t\t" + name);
		}
		return lines;
	}
}
