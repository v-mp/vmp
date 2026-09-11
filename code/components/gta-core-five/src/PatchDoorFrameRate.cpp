#include "StdInc.h"
#include "Hooking.Patterns.h"
struct PatternPair
{
	std::string_view pattern;
	int offset;
};

static HookFunction hookFunction([]()
{
	// Opening speed: divisor = Max(0.03, realDt) clamps to 0.03 above ~33 FPS
	{
		auto loc = hook::get_pattern("F3 44 0F 10 3D ? ? ? ? 41 0F 2F D7");
		hook::nop(loc, 9);
		hook::put<uint32_t>(loc, 0xFF570F45); // xorps xmm15, xmm15
	}

	// Wake the collider for ALL auto door types
	{
		std::initializer_list<PatternPair> doorTorqueLocs = {
			{ "44 0F 2F 0D ? ? ? ? 76 ? 48 8B CB", 8 }, // BAR
			{ "44 0F 2F C8 76 ? 48 8B CB", 4 }, // GRG
			{ "0F 2F F9 76 ? 48 8B CB", 3 } // SLD
		};

		for (auto& entry : doorTorqueLocs)
		{
			hook::nop(hook::get_pattern(entry.pattern, entry.offset), 2);
		}
	}
});
