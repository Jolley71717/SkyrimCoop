#pragma once

#include <Windows.h>

// Puts a_hook in a vtable's slot when the function there is the game's own: in its image, not another plugin's hook
// (whose own original we would call twice) or junk. a_original holds the function the slot held before the slot changes
// (a job thread may call the hook at once); false: not hooked, a_original left as it was. a_who names the system in the
// log ("Players", "NPCs").
namespace VtableHook
{
	template <class Fn>
	bool Slot(std::uintptr_t a_vtable, std::size_t a_index, Fn a_hook, Fn& a_original, std::string_view a_who, std::string_view a_what)
	{
		const auto slot = a_vtable + sizeof(void*) * a_index;
		const auto current = *reinterpret_cast<const std::uintptr_t*>(slot);
		const auto image = reinterpret_cast<std::uintptr_t>(REX::W32::GetModuleHandleW(nullptr));
		MEMORY_BASIC_INFORMATION info{};
		if (!::VirtualQuery(reinterpret_cast<void*>(current), &info, sizeof(info)) || reinterpret_cast<std::uintptr_t>(info.AllocationBase) != image) {
			logs::error("{}: the {} slot holds {:#x}, outside the game's image: not hooked", a_who, a_what, current);
			return false;
		}
		a_original = reinterpret_cast<Fn>(current);
		REL::safe_write(slot, reinterpret_cast<std::uintptr_t>(a_hook));
		logs::info("{}: hooked the {} (game+{:X})", a_who, a_what, current - image);
		return true;
	}
}
