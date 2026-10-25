#include "Crash/GameState.h"

namespace Crash::GameState
{
	namespace
	{
		constexpr std::size_t MAX_MENUS = 256;

		std::string describe_main_loop()
		{
			const auto main = RE::Main::GetSingleton();
			if (!main)
				return "Main loop: not created"s;

			const auto& state = main->GetRuntimeData();
			return fmt::format("Main loop: {}{}"sv,
				state.gameActive ? "active"sv : "inactive"sv,
				state.freezeTime ? ", time frozen"sv : ""sv);
		}

		std::string describe_menus()
		{
			const auto ui = RE::UI::GetSingleton();
			if (!ui)
				return "Open menus: UI not created"s;

			std::string names;
			std::size_t inspected = 0;
			for (const auto& [name, entry] : ui->menuMap) {
				if (++inspected > MAX_MENUS)
					break;
				if (!entry.menu || !entry.menu->OnStack())
					continue;
				names += names.empty() ? ""sv : ", "sv;
				names += name.c_str();
			}

			return fmt::format("Open menus: {}{}"sv,
				names.empty() ? "none"sv : std::string_view{ names },
				ui->numPausesGame ? " (game paused)"sv : ""sv);
		}

		std::string describe_player()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player)
				return "Player: not created"s;

			const auto position = player->GetPosition();
			const auto cell = player->GetParentCell();
			if (!cell) {
				return fmt::format("Player: no parent cell, position ({:.1f}, {:.1f}, {:.1f})"sv,
					position.x, position.y, position.z);
			}

			const auto cellName = cell->GetName();
			const auto worldspace = player->GetWorldspace();
			const auto worldspaceName = worldspace ? worldspace->GetName() : nullptr;
			return fmt::format("Player: {} cell \"{}\" [{:08X}]{}{}, position ({:.1f}, {:.1f}, {:.1f}), 3D {}"sv,
				cell->IsInteriorCell() ? "interior"sv : "exterior"sv,
				cellName ? cellName : ""sv,
				cell->GetFormID(),
				worldspaceName ? " in "sv : ""sv,
				worldspaceName ? worldspaceName : ""sv,
				position.x, position.y, position.z,
				player->Is3DLoaded() ? "loaded"sv : "not loaded"sv);
		}
	}

	std::vector<std::string> Describe()
	{
		std::vector<std::string> lines;
		for (const auto describe : { describe_main_loop, describe_menus, describe_player }) {
			try {
				lines.push_back(describe());
			} catch (...) {
				// One unreadable singleton must not hide the others.
			}
		}
		return lines;
	}
}
