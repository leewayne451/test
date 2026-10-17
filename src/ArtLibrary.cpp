#include "ArtLibrary.hpp"
#include <Geode/utils/file.hpp>

ArtLibrary& ArtLibrary::get() {
	static ArtLibrary lib;
	return lib;
}

LibObj const* ArtLibrary::find(int id) const {
	auto it = m_objs.find(id);
	return it == m_objs.end() ? nullptr : &it->second;
}

bool ArtLibrary::load() {
	if (m_loaded) return !m_objs.empty();
	m_loaded = true;

	auto path = Mod::get()->getResourcesDir() / "art_library.json";
	auto txt = geode::utils::file::readString(path);
	if (txt.isErr()) {
		log::warn("Art Agent: {} not found, library layers disabled", path.string());
		return false;
	}
	auto parsed = matjson::parse(txt.unwrap());
	if (parsed.isErr()) {
		log::warn("Art Agent: art_library.json is not valid JSON, library layers disabled");
		return false;
	}
	auto root = parsed.unwrap();
	auto arr = root["objects"].asArray();
	if (arr.isErr()) {
		log::warn("Art Agent: art_library.json has no \"objects\" array");
		return false;
	}
	for (auto& o : arr.unwrap()) {
		LibObj e;
		e.id = (int)o["id"].asInt().unwrapOr(0);
		if (e.id <= 0) continue;
		e.role = o["role"].asString().unwrapOr("");
		e.facing = o["facing"].asString().unwrapOr("");
		e.w = (float)o["w"].asDouble().unwrapOr(1.0);
		e.h = (float)o["h"].asDouble().unwrapOr(1.0);
		if (auto t = o["tags"].asArray(); t.isOk())
			for (auto& tag : t.unwrap()) e.tags.push_back(tag.asString().unwrapOr(""));
		m_objs[e.id] = e;
	}
	log::info("Art Agent: loaded {} library objects from {}", m_objs.size(), path.string());
	return !m_objs.empty();
}
