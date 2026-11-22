#include "game/Console.h"

namespace Console
{
	bool Run(std::string_view a_command)
	{
		const auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
		const auto script = factory ? factory->Create() : nullptr;
		if (!script) {
			logs::warn("Console: the game made no script to run a command in");
			return false;
		}
		script->SetCommand(a_command);
		script->CompileAndRun(nullptr);
		delete script;
		return true;
	}
}
