#include "Config.h"

#include <articuno/archives/ryml/ryml.h>

using namespace articuno::ryml;
namespace CrashLogger
{

	const Config& Config::GetSingleton() noexcept
	{
		static Config instance;

		static std::atomic_bool initialized;
		static std::latch latch(1);
		if (!initialized.exchange(true)) {
			auto input = "Data\\SKSE\\Plugins\\CrashLogger.yaml";
			std::ifstream inputFile(input);
			if (inputFile.good()) {
				yaml_source ar(inputFile);
				ar >> instance;
			}
			latch.count_down();
		}
		latch.wait();

		return instance;
	}

}
