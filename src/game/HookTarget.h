#pragma once

#include <cstdint>
#include <string_view>

// Whether a vtable slot may be hooked (src/game/VtableHook.h), from what Windows says of the memory the slot points at.
// Another plugin may have hooked the slot first (a combat or animation plugin hooks the same graph notify and the same
// update): its hook is chained to, as it chains to the game's, so ours works in any load order. What is refused is a
// slot whose function is not code: junk, freed or guarded memory (a call there would crash), or our own hook already
// (a second install would call itself). No game here: tests/HookTargetTests.cpp.
namespace HookTarget
{
	// The values of MEMORY_BASIC_INFORMATION's State and Protect (winnt.h), so this needs no Windows header.
	inline constexpr std::uint32_t MEM_COMMITTED = 0x1000;
	inline constexpr std::uint32_t PAGE_NO_ACCESS = 0x01;
	inline constexpr std::uint32_t PAGE_GUARDED = 0x100;
	inline constexpr std::uint32_t PAGE_EXECUTABLE = 0x10 | 0x20 | 0x40 | 0x80;  // EXECUTE, _READ, _READWRITE, _WRITECOPY

	enum class Verdict
	{
		kGame,     // the game's own function: hooked, the game's called after
		kChained,  // code of another module (a plugin's hook): hooked, that hook called after
		kOurs,     // our hook is there already: left as it is
		kNotCode,  // not committed, executable memory: refused
	};

	[[nodiscard]] constexpr bool Executable(std::uint32_t a_state, std::uint32_t a_protect)
	{
		return a_state == MEM_COMMITTED && (a_protect & PAGE_EXECUTABLE) != 0 && (a_protect & (PAGE_GUARDED | PAGE_NO_ACCESS)) == 0;
	}

	// a_queried: Windows answered the query of a_current at all; a_inGame: its allocation is the game's image.
	[[nodiscard]] constexpr Verdict Judge(std::uintptr_t a_current, std::uintptr_t a_hook, bool a_queried, std::uint32_t a_state, std::uint32_t a_protect, bool a_inGame)
	{
		if (a_current != 0 && a_current == a_hook) {
			return Verdict::kOurs;
		}
		if (a_current == 0 || !a_queried || !Executable(a_state, a_protect)) {
			return Verdict::kNotCode;
		}
		return a_inGame ? Verdict::kGame : Verdict::kChained;
	}

	[[nodiscard]] constexpr bool Hooks(Verdict a_verdict)
	{
		return a_verdict == Verdict::kGame || a_verdict == Verdict::kChained;
	}

	// The file name at the end of a module's path, for the log: never the folders, which name the user's account.
	[[nodiscard]] constexpr std::string_view FileName(std::string_view a_path)
	{
		const auto slash = a_path.find_last_of("\\/");
		return slash == std::string_view::npos ? a_path : a_path.substr(slash + 1);
	}
}
