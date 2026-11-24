#include "Test.h"

#include "game/HookTarget.h"

// Whether a vtable slot is hooked, whatever plugin hooked it first (src/game/HookTarget.h).

namespace
{
	using HookTarget::Verdict;
	constexpr std::uint32_t COMMIT = HookTarget::MEM_COMMITTED;
	constexpr std::uint32_t EXECUTE_READ = 0x20;
	constexpr std::uint32_t READ_ONLY = 0x02;
	constexpr std::uint32_t HOOK = 0x7FF0'0000;
}

TEST("hook target: the game's own function is hooked")
{
	CHECK(HookTarget::Judge(0x1400'1000, HOOK, true, COMMIT, EXECUTE_READ, true) == Verdict::kGame);
	CHECK(HookTarget::Hooks(Verdict::kGame));
}

TEST("hook target: another plugin's hook of the slot is chained to, so the load order does not matter")
{
	// A combat plugin loaded first put its hook in the slot: ours goes in front and calls it, as it calls the game's.
	CHECK(HookTarget::Judge(0x7FF8'1234, HOOK, true, COMMIT, EXECUTE_READ, false) == Verdict::kChained);
	CHECK(HookTarget::Hooks(Verdict::kChained));
	// Every executable protection a module's code or a trampoline has.
	for (const std::uint32_t protect : { 0x10u, 0x20u, 0x40u, 0x80u }) {
		CHECK(HookTarget::Judge(0x7FF8'1234, HOOK, true, COMMIT, protect, false) == Verdict::kChained);
	}
}

TEST("hook target: a slot that points at no code is refused")
{
	CHECK(HookTarget::Judge(0, HOOK, false, 0, 0, false) == Verdict::kNotCode);                      // empty
	CHECK(HookTarget::Judge(0x1234, HOOK, false, 0, 0, false) == Verdict::kNotCode);                 // Windows knows nothing there
	CHECK(HookTarget::Judge(0x7FF8'1234, HOOK, true, 0x2000, EXECUTE_READ, false) == Verdict::kNotCode);  // reserved, not committed
	CHECK(HookTarget::Judge(0x7FF8'1234, HOOK, true, 0x10000, 0, false) == Verdict::kNotCode);       // freed
	CHECK(HookTarget::Judge(0x7FF8'1234, HOOK, true, COMMIT, READ_ONLY, false) == Verdict::kNotCode);     // data
	CHECK(HookTarget::Judge(0x7FF8'1234, HOOK, true, COMMIT, EXECUTE_READ | HookTarget::PAGE_GUARDED, false) == Verdict::kNotCode);
	CHECK(!HookTarget::Hooks(Verdict::kNotCode));
}

TEST("hook target: our own hook already in the slot is left, never chained to itself")
{
	CHECK(HookTarget::Judge(HOOK, HOOK, true, COMMIT, EXECUTE_READ, false) == Verdict::kOurs);
	CHECK(!HookTarget::Hooks(Verdict::kOurs));
}

TEST("hook target: a module is named in the log by its file name, never its folders")
{
	CHECK(HookTarget::FileName("C:\\Games\\Skyrim\\Data\\SKSE\\Plugins\\SomeCombat.dll") == "SomeCombat.dll");
	CHECK(HookTarget::FileName("D:/x/y/z.dll") == "z.dll");
	CHECK(HookTarget::FileName("plain.dll") == "plain.dll");
	CHECK(HookTarget::FileName("") == "");
}
