#pragma once
#include <Geode/Geode.hpp>
#include <unordered_map>

using namespace geode::prelude;

// One real decoration object from art_library.json (made by build_library_json.py).
struct LibObj {
	int id = 0;
	std::string role;    // edge | scatter | focal | fill | corner
	std::string facing;  // edge pieces: which side of the picture is the exposed face (up/down/left/right)
	float w = 1.f, h = 1.f; // size in blocks at scale 1
	std::vector<std::string> tags;
};

class ArtLibrary {
public:
	static ArtLibrary& get();
	// Loads resources/art_library.json once. Returns false if the file is missing or empty
	// (the mod then keeps working with the procedural style only).
	bool load();
	LibObj const* find(int id) const;
	size_t size() const { return m_objs.size(); }

private:
	std::unordered_map<int, LibObj> m_objs;
	bool m_loaded = false;
};
