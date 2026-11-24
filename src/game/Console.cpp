#include "game/Console.h"

#include "ConsoleTarget.h"

namespace Console
{
	std::string Run(std::string_view a_command)
	{
		// "<ref id>.<command>" runs on that reference. The compiler, given no target, ignores the prefix and runs
		// nothing, so the reference is looked up here and passed as the target (src/ConsoleTarget.h).
		const auto         split = ConsoleTarget::SplitTarget(a_command);
		RE::TESObjectREFR* target = nullptr;
		if (split.id != 0) {
			target = RE::TESForm::LookupByID<RE::TESObjectREFR>(split.id);
			if (!target) {
				return std::format("no reference {:08X}", split.id);
			}
		}

		const auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
		const auto script = factory ? factory->Create() : nullptr;
		if (!script) {
			logs::warn("Console: the game made no script to run a command in");
			return "the game could not run it";
		}
		script->SetCommand(split.id != 0 ? split.command : a_command);
		script->CompileAndRun(target);
		delete script;
		return {};
	}
}
