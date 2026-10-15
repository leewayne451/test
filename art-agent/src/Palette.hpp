#pragma once
#include <vector>

// Objects the agent is allowed to place. w/h are the size in blocks at scale 1.
// VERIFY these IDs in your editor and extend the list freely: more variety = better art.
struct PaletteEntry { int id; const char* name; float w, h; };

inline std::vector<PaletteEntry> const& palette() {
	static std::vector<PaletteEntry> const p = {
		{1,   "square block",                        1.f, 1.f},
		{40,  "slab (half-height block)",            1.f, 0.5f},
		{289, "45-degree slope, rising to the right",1.f, 1.f},
		{211, "decorative square (no hitbox)",       1.f, 1.f},
	};
	return p;
}
