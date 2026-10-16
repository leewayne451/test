#include <cstdlib>
#include <cmath>
#include <cctype>
#include <Geode/utils/file.hpp>
#include "ArtAgentPopup.hpp"
#include "Palette.hpp"
#include <algorithm>
#include <map>
#include <sstream>
#include <random>
#include <functional>
#include <filesystem>
#include <ctime>
#include <Geode/utils/base64.hpp>

static constexpr float B = 30.f; // 1 block in GD units

// Reference screenshots live here (see the Ref button in the editor and the Refs box in the panel).
static std::filesystem::path refsDir() {
	return Mod::get()->getSaveDir() / "references";
}

static bool inTri(SlopeTri const& t, float x, float y) {
	float dx = t.ax - t.px, dy = t.by - t.py;
	if (std::abs(dx) < 1e-4f || std::abs(dy) < 1e-4f) return false;
	float u = (x - t.px) / dx, v = (y - t.py) / dy;
	return u >= 0.f && v >= 0.f && u + v <= 1.f;
}

// Copies a JSON array into a vector (empty if the value isn't an array).
template <class V>
static std::vector<matjson::Value> arrOf(V&& v) {
	std::vector<matjson::Value> out;
	if (auto r = v.asArray(); r.isOk()) out = r.unwrap();
	return out;
}

ArtAgentPopup* ArtAgentPopup::create(EditorUI* ui, std::vector<CCRect> const& rects, std::vector<CCRect> const& hazards, std::vector<Ref<GameObject>> const& slopes) {
	auto ret = new ArtAgentPopup();
	if (ret->init(ui, rects, hazards, slopes)) {
		ret->autorelease();
		return ret;
	}
	delete ret;
	return nullptr;
}

static ButtonSprite* btnSpr(char const* text, int width, char const* tex = "GJ_button_01.png") {
	return ButtonSprite::create(text, width, true, "bigFont.fnt", tex, 24.f, 0.5f);
}

bool ArtAgentPopup::init(EditorUI* ui, std::vector<CCRect> const& rects, std::vector<CCRect> const& hazards, std::vector<Ref<GameObject>> const& slopes) {
	if (!Popup::init(230.f, 312.f)) return false;
	m_ui = ui;
	m_seed = std::random_device{}();
	m_noElasticity = true;
	this->setTitle("Art Agent");
	this->setOpacity(0); // don't dim the editor: the draft is previewed live behind this panel

	// dock the panel on the right so the level stays visible
	auto win = CCDirector::get()->getWinSize();
	m_mainLayer->setPositionX(m_mainLayer->getPositionX() + (win.width / 2 - m_size.width / 2 - 8));

	this->buildGrid(rects, hazards, slopes);

	m_agentText = SimpleTextArea::create("", "chatFont.fnt", 0.6f, 205.f);
	m_agentText->setAnchorPoint({0.5f, 1.f});
	m_mainLayer->addChildAtPosition(m_agentText, Anchor::Top, {0, -34});
	this->say(m_tooBig
		? "That selection is too large (max 100x80 blocks). Select a smaller part."
		: "Tell me what to draw on the " + std::to_string(rects.size() + slopes.size()) + " selected blocks.");

	m_countLabel = CCLabelBMFont::create("", "goldFont.fnt");
	m_countLabel->setScale(0.45f);
	m_mainLayer->addChildAtPosition(m_countLabel, Anchor::Bottom, {0, 134});

	m_prompt = TextInput::create(165.f, "Describe the design...", "chatFont.fnt");
	m_prompt->setMaxCharCount(300);
	m_mainLayer->addChildAtPosition(m_prompt, Anchor::Bottom, {-24, 112});

	auto send = CCMenuItemExt::createSpriteExtra(btnSpr("Go", 30), [this](CCObject* s) { onSend(s); });
	m_buttonMenu->addChildAtPosition(send, Anchor::Bottom, {88, 112});

	// reference screenshots: names of image files in the references folder, comma separated
	m_refsInput = TextInput::create(165.f, "Refs: name, name", "chatFont.fnt");
	m_refsInput->setMaxCharCount(200);
	m_refsInput->setString(Mod::get()->getSavedValue<std::string>("refs", ""));
	m_mainLayer->addChildAtPosition(m_refsInput, Anchor::Bottom, {-24, 86});
	auto dirBtn = CCMenuItemExt::createSpriteExtra(btnSpr("Dir", 30, "GJ_button_04.png"),
		[this](CCObject* s) { onRefsFolder(s); });
	m_buttonMenu->addChildAtPosition(dirBtn, Anchor::Bottom, {88, 86});

	auto limitLbl = CCLabelBMFont::create("Limit", "bigFont.fnt");
	limitLbl->setScale(0.35f);
	m_mainLayer->addChildAtPosition(limitLbl, Anchor::Bottom, {-88, 58});
	m_limitInput = TextInput::create(50.f, "800", "chatFont.fnt");
	m_limitInput->setFilter("0123456789");
	m_limitInput->setMaxCharCount(4);
	m_limitInput->setString(std::to_string(Mod::get()->getSettingValue<int64_t>("object-limit")));
	m_mainLayer->addChildAtPosition(m_limitInput, Anchor::Bottom, {-45, 58});

	auto tog = CCMenuItemExt::createTogglerWithStandardSprites(0.5f, [this](CCMenuItemToggler* t) {
		m_overhang = !t->isToggled(); // callback fires before the state flips
	});
	m_buttonMenu->addChildAtPosition(tog, Anchor::Bottom, {20, 58});
	auto ohLbl = CCLabelBMFont::create("Allow overhang", "bigFont.fnt");
	ohLbl->setScale(0.3f);
	ohLbl->setAnchorPoint({0, 0.5f});
	m_mainLayer->addChildAtPosition(ohLbl, Anchor::Bottom, {36, 58});

	auto undo = CCMenuItemExt::createSpriteExtra(btnSpr("Undo", 50, "GJ_button_06.png"),
		[this](CCObject* s) { onDiscard(s); });
	m_buttonMenu->addChildAtPosition(undo, Anchor::Bottom, {-36, 28});
	auto keep = CCMenuItemExt::createSpriteExtra(btnSpr("Keep", 50),
		[this](CCObject* s) { onKeep(s); });
	m_buttonMenu->addChildAtPosition(keep, Anchor::Bottom, {36, 28});

	this->setCount();
	return true;
}

// Rasterise the selection into a block grid so the model can "see" the shape.
void ArtAgentPopup::buildGrid(std::vector<CCRect> const& rects, std::vector<CCRect> const& hazards, std::vector<Ref<GameObject>> const& slopes) {
	m_tris.clear();
	m_slopeObjs = slopes;

	std::vector<CCRect> slopeRects;
	for (auto const& so : slopes) slopeRects.push_back(static_cast<GameObject*>(so)->getObjectRect());

	float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
	std::vector<CCRect> const* groups[] = {&rects, &hazards, &slopeRects};
	for (auto const* group : groups) {
		for (auto const& r : *group) {
			minX = std::min(minX, r.getMinX()); minY = std::min(minY, r.getMinY());
			maxX = std::max(maxX, r.getMaxX()); maxY = std::max(maxY, r.getMaxY());
		}
	}
	m_origin = ccp(std::floor(minX / B) * B, std::floor(minY / B) * B);
	m_cols = (int)std::ceil((maxX - m_origin.x) / B);
	m_rows = (int)std::ceil((maxY - m_origin.y) / B);
	if (m_cols > 100 || m_rows > 80) { m_tooBig = true; return; }

	std::vector<std::string> rows(m_rows, std::string(m_cols, '.'));
	for (auto const& r : rects) {
		int i0 = (int)std::floor((r.getMinX() - m_origin.x) / B + 0.01f);
		int i1 = (int)std::ceil((r.getMaxX() - m_origin.x) / B - 0.01f);
		int j0 = (int)std::floor((r.getMinY() - m_origin.y) / B + 0.01f);
		int j1 = (int)std::ceil((r.getMaxY() - m_origin.y) / B - 0.01f);
		for (int j = std::max(j0, 0); j < std::min(j1, m_rows); j++)
			for (int i = std::max(i0, 0); i < std::min(i1, m_cols); i++)
				rows[j][i] = '#';
	}

	// Slopes -> right triangles. Default slope sprite has its right angle at the bottom-right;
	// flips apply in local space, then rotation (clockwise, snapped to 90 degrees).
	for (size_t k = 0; k < slopes.size(); k++) {
		GameObject* o = slopes[k];
		auto const& R = slopeRects[k];
		float x0 = (R.getMinX() - m_origin.x) / B, x1 = (R.getMaxX() - m_origin.x) / B;
		float y0 = (R.getMinY() - m_origin.y) / B, y1 = (R.getMaxY() - m_origin.y) / B;
		int sx = 1, sy = -1;
		if (o->m_isFlipX) sx = -sx;
		if (o->m_isFlipY) sy = -sy;
		int q = ((int)std::lround(o->getRotation() / 90.f) % 4 + 4) % 4;
		for (int n = 0; n < q; n++) { int nx = sy, ny = -sx; sx = nx; sy = ny; } // 90 deg clockwise
		SlopeTri t;
		t.px = sx > 0 ? x1 : x0; t.py = sy > 0 ? y1 : y0;
		t.ax = sx > 0 ? x0 : x1; t.ay = t.py;
		t.bx = t.px;             t.by = sy > 0 ? y0 : y1;
		t.rising = (sx > 0) != (sy > 0);
		m_tris.push_back(t);
	}

	// show slope cells to the model ('/' rising hypotenuse, '\\' falling); geometry itself is handled by the mod
	for (int j = 0; j < m_rows; j++) for (int i = 0; i < m_cols; i++) {
		if (rows[j][i] != '.') continue;
		for (auto const& t : m_tris) {
			int hit = 0;
			for (int a = 0; a < 3; a++) for (int c = 0; c < 3; c++)
				if (inTri(t, i + (a + 0.5f) / 3.f, j + (c + 0.5f) / 3.f)) hit++;
			if (hit >= 4) { rows[j][i] = t.rising ? '/' : '\\'; break; }
		}
	}

	// hazards (spikes, saws...): mark the cell they sit in as 'S'; they are left untouched
	for (auto const& h : hazards) {
		int i = (int)std::floor((h.getMidX() - m_origin.x) / B);
		int j = (int)std::floor((h.getMidY() - m_origin.y) / B);
		if (i >= 0 && j >= 0 && i < m_cols && j < m_rows && rows[j][i] == '.') rows[j][i] = 'S';
	}
	std::ostringstream os;
	for (int j = m_rows - 1; j >= 0; j--) os << rows[j] << "\n"; // top row first
	m_grid = os.str();
}

int ArtAgentPopup::limit() {
	int v = numFromString<int>(m_limitInput->getString()).unwrapOr(800);
	return std::clamp(v, 1, 2000);
}

void ArtAgentPopup::say(std::string const& text) { m_agentText->setText(text); }

void ArtAgentPopup::setCount() {
	m_countLabel->setString(fmt::format("{} / {} objects", m_draft.size(), limit()).c_str());
}

static std::string systemPrompt() {
	return R"PROMPT(You are the art director inside Geometry Dash's level editor. The player selected a structure made of blocks and describes how it should look. The theme can be anything: stone castle, wood, ice, hell, sci-fi, neon, desert ruins, toxic sewer, candy, graveyard, deep sea, and so on. You do NOT place objects. You choose a visual STYLE, and the mod itself tiles it precisely over the structure's silhouette, adds a mortar backing, bevel highlights/shadows and an outline.

If reference images are attached, they are screenshots of levels the player likes. Study their palette, material shapes, mood, contrast and level of detail, and make your style capture that feel. Do not try to copy them literally. The written request always wins if it conflicts with the references.

Reply with ONLY one JSON object, no prose, no code fences:
{"summary":"1-2 short sentences",
"pattern":"bricks|stones|cobble|tiles|planks",
"brick_w":1.0,"brick_h":0.5,"gap":0.05,
"rotation_jitter":1,"size_jitter":0.1,
"colors":["#2B1414","#3A1A1A","#4A2222","#5C2A26"],
"mortar":"#0D0606",
"edge":{"style":"none|moss|grass|snow|frost|slime|lava|blood|sand|neon","color":"#FF5A14","depth":0.25},
"accent":{"type":"spots|cracks","color":"#FF7A1A","density":0.05},
"damage":{"missing":0.08,"glow":"#FF5A14"},
"trim":{"every":0,"color":"#8A8A8A","thickness":0.12},
"glow":{"color":"#FF5A14","height":0.3,"strength":0.5}}

Field meanings (sizes are in blocks, 1 block = 1 grid square). Only "summary", "pattern" and "colors" are required; every other field is optional, and leaving a feature out (or 0) switches it off.
- pattern: bricks = running-bond rows; stones = irregular rows of uneven stones; cobble = small, very irregular rounded stones; tiles = aligned grid; planks = long horizontal boards.
- brick_w / brick_h: size of one piece (0.25 to 4). gap: seam between pieces (0 to 0.3) showing the mortar.
- rotation_jitter: max random tilt in degrees; keep it SMALL (0 to 2), large values look sloppy. size_jitter: 0 to 0.3.
- colors: 3 or 4 shades of the MAIN material ordered DARKEST to LIGHTEST, close to each other. The mod darkens low areas and lightens high areas, and clusters shades into natural patches. Make them clearly distinct, but not wildly different. At most 4 colors.
- mortar: very dark color for seams, backing and outline (darker than the darkest shade).
- edge: decoration on TOP surfaces. moss/grass = green growth, snow = white, frost = pale icy drips, slime = dripping goo, lava = glowing drips, blood = dark red drips, sand = drifted sand, neon = glowing strip. depth 0.1 to 0.35. Use "none" for clean materials like metal, tech or plain stone.
- accent: scattered detail. spots = small speckles, cracks = thin slashes. density 0 to 0.3. A bright accent color on cracks makes them glow.
- damage: "missing" 0 to 0.3 = fraction of pieces left out so seams/holes show. "glow" = optional color that shines out of those holes (ember orange, magical blue, toxic green, light leaking in). Leave glow out for plain holes.
- trim: a horizontal band across the structure every N rows (every = 2 to 12, 0 = none). Good for castle cornices, tech stripes, wooden beams. Pick a color lighter or different from the main shades.
- glow: soft light rising from the BOTTOM of the structure. height = fraction of the structure it reaches (0.1 to 0.6), strength 0 to 1. Use it for lava, fire, underwater light, toxic fumes, neon spill; omit for ordinary materials.

Style hints (starting points, not rules; vary freely and match the request):
- Stone castle: stones 1x0.5, greys, moss edge, dark cracks, trim every 6 in light grey.
- Wood: planks 3x0.5, gap 0.03, rotation_jitter 0, browns, no edge, a few dark spots.
- Ice cave: tiles 1x1, pale blues, white spots, frost edge, cyan cracks, glow in pale blue.
- Hell: bricks 1x0.5, very dark reds/blacks, lava edge, orange cracks, damage glow orange, bottom glow orange.
- Sci-fi hull: tiles 1x1 or 2x1, cool dark greys/blues, gap 0.08, trim every 3 in cyan, neon edge, no damage.
- Desert ruins: stones 1x0.5, warm sand and ochre, sand edge, damage 0.15 with no glow.
- Toxic sewer: bricks 1x0.5, murky greens, slime edge in bright green, damage glow green, bottom glow green.
- Graveyard / bone: cobble, desaturated grey-purples, moss edge, cracks, a cold pale glow.
If the user asks for changes, return the COMPLETE updated style object.)PROMPT";
}

std::string ArtAgentPopup::stateNote() {
	return fmt::format("Object limit: {}. Overhang beyond the structure: {}.",
		limit(), m_overhang ? "allowed" : "NOT allowed (stay inside the bounding box)");
}

void ArtAgentPopup::onSend(CCObject*) {
	auto text = m_prompt->getString();
	if (m_busy || m_tooBig || text.empty()) return;
	std::string msg;
	if (m_history.empty()) {
		msg = fmt::format("Structure grid, {} columns x {} rows, top row first ('#' = block, '/' or '\\' = slope, 'S' = spike/hazard left untouched, '.' = empty):\n{}\n{}\nRequest: {}",
			m_cols, m_rows, m_grid, stateNote(), text);
	} else {
		msg = fmt::format("Change request: {}\n{}", text, stateNote());
	}
	m_prompt->setString("");
	Mod::get()->setSavedValue<std::string>("refs", m_refsInput->getString());
	this->request(msg);
}

void ArtAgentPopup::onRefsFolder(CCObject*) {
	std::error_code ec;
	std::filesystem::create_directories(refsDir(), ec);
	(void)geode::utils::file::openFolder(refsDir());
}

// Reads the reference screenshots named in the Refs box (max 4, max 6 MB each).
std::vector<matjson::Value> ArtAgentPopup::refParts(std::string& usedNames, std::string& warning) {
	std::vector<matjson::Value> out;
	std::vector<std::string> missing;
	std::stringstream ss(m_refsInput->getString());
	std::string tok;
	int used = 0;
	while (std::getline(ss, tok, ',')) {
		auto bad = [](unsigned char c) { return std::isspace(c) || c == '"' || c == '\''; };
		while (!tok.empty() && bad(tok.back())) tok.pop_back();
		size_t a = 0;
		while (a < tok.size() && bad(tok[a])) a++;
		tok = tok.substr(a);
		if (tok.empty()) continue;
		if (used >= 4) { warning += " Only the first 4 references are used."; break; }
		// plain file names only: no folders, no ".."
		if (tok.find('/') != std::string::npos || tok.find('\\') != std::string::npos || tok.find("..") != std::string::npos) {
			missing.push_back(tok);
			continue;
		}
		std::filesystem::path found;
		for (char const* ext : {"", ".png", ".jpg", ".jpeg"}) {
			auto p = refsDir() / (tok + ext);
			std::error_code ec;
			if (std::filesystem::is_regular_file(p, ec)) { found = p; break; }
		}
		if (found.empty()) { missing.push_back(tok); continue; }
		auto r = geode::utils::file::readBinary(found);
		if (r.isErr()) { missing.push_back(tok); continue; }
		auto bytes = r.unwrap();
		if (bytes.empty() || bytes.size() > 6u * 1024u * 1024u) { missing.push_back(tok + " (empty or over 6 MB)"); continue; }
		std::string ext = found.extension().string();
		for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
		std::string mime = ext == ".png" ? "image/png" : "image/jpeg";
		out.push_back(matjson::makeObject({{"inlineData", matjson::makeObject({
			{"mimeType", mime},
			{"data", geode::utils::base64::encode(bytes)},
		})}}));
		usedNames += (usedNames.empty() ? "" : ", ") + tok;
		used++;
	}
	if (!missing.empty()) {
		std::string list;
		for (auto& m : missing) list += (list.empty() ? "" : ", ") + m;
		warning = "Refs not found: " + list + "." + warning;
	}
	return out;
}

void ArtAgentPopup::snapReference(EditorUI* ui) {
	std::error_code ec;
	std::filesystem::create_directories(refsDir(), ec);
	auto lel = ui->m_editorLayer;
	auto win = CCDirector::get()->getWinSize();
	auto rt = CCRenderTexture::create((int)win.width, (int)win.height);
	if (!rt) {
		FLAlertLayer::create("Art Agent", "Couldn't capture the editor view.", "OK")->show();
		return;
	}
	rt->begin();
	lel->visit(); // the level only: no editor UI, no panel
	rt->end();

	auto name = fmt::format("snap-{}", (long long)std::time(nullptr));
	auto path = refsDir() / (name + ".png");
	bool ok = false;
	if (auto img = rt->newCCImage(true)) {
		ok = img->saveToFile(geode::utils::string::pathToString(path).c_str(), true);
		img->release();
	}
	std::error_code ec2;
	if (!ok || !std::filesystem::exists(path, ec2)) {
		FLAlertLayer::create("Art Agent", "Couldn't save the screenshot. You can also drop any PNG/JPG into the references folder yourself (Dir button in the Art Agent panel).", "OK")->show();
		return;
	}
	FLAlertLayer::create("Art Agent",
		fmt::format("Saved reference <cy>{}</c>.\nIn the Art Agent panel, type <cy>{}</c> into the Refs box to use it.", name, name),
		"OK")->show();
}

void ArtAgentPopup::onDiscard(CCObject*) {
	this->clearDraft();
	this->setCount();
	this->say("Draft removed. Describe what you'd like instead.");
}

void ArtAgentPopup::onKeep(CCObject*) {
	if (m_draft.empty()) { this->say("Nothing to keep yet."); return; }
	auto arr = CCArray::create();
	for (auto& o : m_draft) arr->addObject(o);
	// Registers the placement as one undoable step in the editor. Verify against your bindings.
	auto undo = UndoObject::createWithArray(arr, UndoCommand::Paste);
	m_ui->m_editorLayer->addToUndoList(undo, false);
	m_draft.clear();
	m_recolored.clear(); // keeping the draft also keeps the mortar-tinted slopes
	Popup::onClose(nullptr);
}

void ArtAgentPopup::onClose(CCObject* sender) {
	this->clearDraft(); // closing without Keep discards the draft
	Popup::onClose(sender);
}

void ArtAgentPopup::request(std::string userMsg) {
	auto key = Mod::get()->getSettingValue<std::string>("api-key");
	// fallback: api-key.txt in the mod's save dir (avoids text-box character filtering)
	{
		auto path = Mod::get()->getSaveDir() / "api-key.txt";
		if (auto r = geode::utils::file::readString(path); r.isOk() && !r.unwrap().empty()) {
			key = r.unwrap();
		}
	}
	// strip whitespace / newlines / quotes picked up when pasting
	{
		auto bad = [](unsigned char c) { return std::isspace(c) || c == '"' || c == '\''; };
		while (!key.empty() && bad(key.back())) key.pop_back();
		size_t i = 0;
		while (i < key.size() && bad(key[i])) i++;
		key = key.substr(i);
	}
	if (key.empty()) {
		this->say("Set your free Gemini API key in the mod's settings first (aistudio.google.com).");
		return;
	}

	m_history.push_back({"user", userMsg});

	// Gemini format: roles are "user" / "model", text lives in parts[].text
	auto textParts = [](std::string const& t) {
		auto parts = matjson::Value::array();
		parts.push(matjson::makeObject({{"text", t}}));
		return parts;
	};

	// reference screenshots ride along with the FIRST user message (the history is re-sent every time)
	std::string refNames, refWarn;
	auto refs = this->refParts(refNames, refWarn);

	auto contents = matjson::Value::array();
	size_t turn = 0;
	for (auto& [role, content] : m_history) {
		auto parts = matjson::Value::array();
		if (turn == 0 && !refs.empty()) {
			for (auto& img : refs) parts.push(img);
			parts.push(matjson::makeObject({{"text", fmt::format(
				"The {} image(s) above are reference screenshots of levels the player likes ({}). "
				"Use them as style reference.", refs.size(), refNames)}}));
		}
		parts.push(matjson::makeObject({{"text", content}}));
		contents.push(matjson::makeObject({
			{"role", role == "assistant" ? "model" : "user"},
			{"parts", parts},
		}));
		turn++;
	}

	// model id: setting, overridden by model.txt in the mod's save dir if present
	std::string model = Mod::get()->getSettingValue<std::string>("model");
	{
		auto path = Mod::get()->getSaveDir() / "model.txt";
		if (auto r = geode::utils::file::readString(path); r.isOk() && !r.unwrap().empty()) {
			model = r.unwrap();
		}
		auto bad = [](unsigned char c) { return std::isspace(c) || c == '"' || c == '\''; };
		while (!model.empty() && bad(model.back())) model.pop_back();
		size_t i = 0;
		while (i < model.size() && bad(model[i])) i++;
		model = model.substr(i);
		if (model.rfind("models/", 0) == 0) model = model.substr(7);
		if (model.empty()) model = "gemini-3.8-flash";
	}

	// Only the 2.5 family takes thinkingBudget:0 to switch thinking off. Newer models are left on their
	// defaults (so no request errors) and get a bigger output allowance, since thinking tokens count toward it.
	bool legacyThinking = model.find("2.5") != std::string::npos;
	auto genCfg = matjson::makeObject({
		{"responseMimeType", "application/json"}, // forces valid JSON output
		{"maxOutputTokens", legacyThinking ? 4000 : 8192},
	});
	if (legacyThinking) genCfg["thinkingConfig"] = matjson::makeObject({{"thinkingBudget", 0}});
	auto body = matjson::makeObject({
		{"systemInstruction", matjson::makeObject({{"parts", textParts(systemPrompt())}})},
		{"contents", contents},
		{"generationConfig", genCfg},
	});
	auto url = fmt::format("https://generativelanguage.googleapis.com/v1beta/models/{}:generateContent", model);

	m_busy = true;
	this->say(refWarn.empty() ? std::string("Thinking...") : "Thinking... " + refWarn);

	web::WebRequest req;
	req.header("x-goog-api-key", key);
	req.header("content-type", "application/json");
	req.timeout(std::chrono::seconds(300));
	req.bodyJSON(body);

	m_task.spawn(req.post(url), [this](web::WebResponse res) {
		m_busy = false;
		if (!res.ok()) {
			m_history.pop_back();
			if (res.code() == 429) {
				this->say("Free-tier rate limit hit. Wait a minute and try again.");
			} else {
				std::string detail;
				if (auto j = res.json(); j.isOk()) {
					detail = j.unwrap()["error"]["message"].asString().unwrapOr("");
				}
				this->say(fmt::format("Request failed (HTTP {}). Check your API key and model name.{}",
					res.code(), detail.empty() ? "" : "\n" + detail.substr(0, 160)));
			}
			return;
		}
		auto jres = res.json();
		if (jres.isErr()) {
			m_history.pop_back();
			this->say("Gemini sent something I couldn't read. Try again.");
			return;
		}
		auto json = jres.unwrap();
		std::string text;
		auto cands = arrOf(json["candidates"]);
		if (!cands.empty()) {
			for (auto& part : arrOf(cands[0]["content"]["parts"]))
				text += part["text"].asString().unwrapOr("");
		}
		if (text.empty()) {
			m_history.pop_back();
			this->say("Gemini returned no answer (it may have been filtered). Try rephrasing.");
			return;
		}
		this->handleReply(text);
	});
}

void ArtAgentPopup::handleReply(std::string text) {
	auto a = text.find('{'), b = text.rfind('}');
	if (a == std::string::npos || b == std::string::npos || b < a) {
		m_history.pop_back();
		this->say("I couldn't read my own answer. Try sending your request again.");
		return;
	}
	auto parsed = matjson::parse(text.substr(a, b - a + 1));
	if (parsed.isErr()) {
		m_history.pop_back();
		this->say("My answer got cut off or malformed. Try sending your request again.");
		return;
	}
	auto v = parsed.unwrap();
	m_history.push_back({"assistant", text});

	if (v["pattern"].isString()) {
		auto gen = this->generateDraft(v);
		this->clearDraft();
		this->spawnDraft(gen);
		this->setCount();
		this->say(v["summary"].asString().unwrapOr("Here's a draft."));
		return;
	}

	std::vector<DraftSpec> specs;
	std::map<int, bool> allowed;
	for (auto& p : palette()) allowed[p.id] = true;

	for (auto& o : arrOf(v["objects"])) {
		DraftSpec s;
		s.id = (int)o["id"].asInt().unwrapOr(0);
		if (!allowed.count(s.id)) continue;
		s.x = (float)o["x"].asDouble().unwrapOr(0.0);
		s.y = (float)o["y"].asDouble().unwrapOr(0.0);
		s.rot = (float)o["rot"].asDouble().unwrapOr(0.0);
		s.sx = std::clamp((float)o["sx"].asDouble().unwrapOr(1.0), 0.2f, 4.f);
		s.sy = std::clamp((float)o["sy"].asDouble().unwrapOr(1.0), 0.2f, 4.f);
		s.color = o["color"].asString().unwrapOr("#FFFFFF");
		s.front = o["front"].asBool().unwrapOr(true);
		if (!m_overhang && (s.x < 0 || s.y < 0 || s.x > m_cols || s.y > m_rows)) continue;
		specs.push_back(s);
		if ((int)specs.size() >= limit()) break; // hard object limit, regardless of the model
	}

	this->clearDraft();
	this->spawnDraft(specs);
	this->setCount();
	this->say(v["summary"].asString().unwrapOr("Here's a draft."));
}


static float frand(std::mt19937& g, float a, float b) {
	return std::uniform_real_distribution<float>(a, b)(g);
}

static float hash2(int x, int y, unsigned sd) {
	unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u + sd * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	h ^= h >> 16;
	return (float)(h & 0xFFFFFF) / 16777215.f;
}

// smooth value noise in [0,1]: neighbouring pieces get related values, so colors form patches
static float vnoise(float x, float y, unsigned sd) {
	int xi = (int)std::floor(x), yi = (int)std::floor(y);
	float fx = x - xi, fy = y - yi;
	fx = fx * fx * (3.f - 2.f * fx);
	fy = fy * fy * (3.f - 2.f * fy);
	float a = hash2(xi, yi, sd), b = hash2(xi + 1, yi, sd);
	float c = hash2(xi, yi + 1, sd), d = hash2(xi + 1, yi + 1, sd);
	float top = c + (d - c) * fx, bot = a + (b - a) * fx;
	return bot + (top - bot) * fy;
}

static std::string hexOf(ccColor3B c) {
	return fmt::format("#{:02X}{:02X}{:02X}", (int)c.r, (int)c.g, (int)c.b);
}

// Deterministic, silhouette-aware tiling. The model only supplies the style.
std::vector<DraftSpec> ArtAgentPopup::generateDraft(matjson::Value v) {
	// occupancy grid (rows[j][i], j = 0 is the bottom row)
	std::vector<std::string> rows(m_rows, std::string(m_cols, '.'));
	{
		std::istringstream is(m_grid);
		std::string line;
		int n = 0;
		while (n < m_rows && std::getline(is, line)) {
			for (int i = 0; i < m_cols && i < (int)line.size(); i++) rows[m_rows - 1 - n][i] = line[i];
			n++;
		}
	}
	auto occ = [&](float x, float y) {
		int i = (int)std::floor(x), j = (int)std::floor(y);
		if (i >= 0 && j >= 0 && i < m_cols && j < m_rows && rows[j][i] == '#') return true;
		for (auto const& t : m_tris) if (inTri(t, x, y)) return true;
		return false;
	};

	auto str = [](matjson::Value& o, char const* k, std::string d) -> std::string {
		return o.isObject() ? o[k].asString().unwrapOr(d) : d;
	};
	auto dbl = [](matjson::Value& o, char const* k, double d) -> double {
		return o.isObject() ? o[k].asDouble().unwrapOr(d) : d;
	};
	auto hex = [](std::string c, std::string fallback) {
		if (c.empty()) c = fallback;
		if (c[0] != '#') c = "#" + c;
		return c;
	};
	auto parse = [](std::string const& c) { return cc3bFromHexString(c).unwrapOr(ccc3(136, 136, 136)); };

	std::string pattern = str(v, "pattern", "bricks");
	float bw = (float)std::clamp(dbl(v, "brick_w", 1.0), 0.25, 4.0);
	float bh = (float)std::clamp(dbl(v, "brick_h", 0.5), 0.25, 4.0);
	float gap = (float)std::clamp(dbl(v, "gap", 0.05), 0.0, 0.3);
	float rotJ = (float)std::clamp(dbl(v, "rotation_jitter", 1.0), 0.0, 3.0); // large tilts look sloppy
	float sizeJ = (float)std::clamp(dbl(v, "size_jitter", 0.1), 0.0, 0.3);

	std::vector<std::string> base;
	for (auto& c : arrOf(v["colors"])) {
		auto cs = c.asString().unwrapOr("");
		if (!cs.empty() && base.size() < 4) base.push_back(hex(cs, "#888888"));
	}
	if (base.empty()) base = {"#3A3A3A", "#4A4A4A", "#5A5A5A"};

	// mortar (seams, backing, outline): from the model or a darkened version of the darkest shade
	std::string mortar;
	{
		auto given = str(v, "mortar", "");
		if (!given.empty()) mortar = hex(given, "#111111");
		else {
			auto d = parse(base[0]);
			mortar = hexOf(ccc3((GLubyte)(d.r * 0.4f), (GLubyte)(d.g * 0.4f), (GLubyte)(d.b * 0.4f)));
		}
	}
	m_mortarHex = mortar;
	// bevel highlight: a lighter tint of the lightest shade
	std::string hl;
	{
		auto l = parse(base.back());
		auto lift = [](GLubyte c) { return (GLubyte)std::min(255.f, c + (255.f - c) * 0.35f); };
		hl = hexOf(ccc3(lift(l.r), lift(l.g), lift(l.b)));
	}

	auto edgeV = v["edge"];
	auto accV = v["accent"];
	std::string edgeStyle = str(edgeV, "style", "none");
	std::string edgeColor = hex(str(edgeV, "color", "#4F7A2E"), "#4F7A2E");
	float edgeDepth = (float)std::clamp(dbl(edgeV, "depth", 0.25), 0.1, 0.35);
	std::string accType = str(accV, "type", "spots");
	std::string accColor = hex(str(accV, "color", "#3E3E3E"), "#3E3E3E");
	float accDens = (float)std::clamp(dbl(accV, "density", 0.0), 0.0, 0.3);

	// transparent / additive channels ("#RRGGBB|a30|add" = 30% opacity, additive blending)
	bool fx = Mod::get()->getSettingValue<bool>("glow");
	std::string glowEdge = edgeColor + "|a30|add";
	std::string glowAcc = accColor + "|a30|add";
	bool glowy = fx && (edgeStyle == "lava" || edgeStyle == "slime" || edgeStyle == "neon");

	// optional, theme-neutral features (all off unless the style asks for them)
	auto damageV = v["damage"];
	auto trimV = v["trim"];
	auto bglowV = v["glow"];
	float missing = (float)std::clamp(dbl(damageV, "missing", 0.0), 0.0, 0.3);
	std::string holeGlow = str(damageV, "glow", "");
	if (!holeGlow.empty()) holeGlow = hex(holeGlow, "#FF5A14");
	std::string holeCol = (fx && !holeGlow.empty()) ? holeGlow + "|a60|add" : std::string();
	int trimEvery = (int)std::clamp(dbl(trimV, "every", 0.0), 0.0, 12.0);
	std::string trimColor = str(trimV, "color", "");
	trimColor = trimColor.empty() ? hl : hex(trimColor, hl);
	float trimTh = (float)std::clamp(dbl(trimV, "thickness", 0.12), 0.06, 0.3);
	std::string bgColor = str(bglowV, "color", "");
	if (!bgColor.empty()) bgColor = hex(bgColor, "#FF5A14");
	float bgHeight = (float)std::clamp(dbl(bglowV, "height", 0.3), 0.1, 0.6);
	float bgStrength = (float)std::clamp(dbl(bglowV, "strength", 0.5), 0.0, 1.0);
	bool accGlowy = false;
	{
		auto ac = parse(accColor);
		accGlowy = fx && accType == "cracks" && (ac.r + ac.g + ac.b) / 765.f > 0.55f;
	}
	std::vector<DraftSpec> glowLayer, accGlowLayer;

	// fixed per panel: follow-up requests ("make it redder") keep the same layout instead of re-rolling it
	unsigned seed = 1337u + m_seed + (unsigned)(m_cols * 31 + m_rows);

	auto mk = [](float x, float y, float rot, float sx, float sy, std::string const& color) {
		DraftSpec s;
		s.id = 211; // decorative square: no hitbox, so it never changes gameplay
		s.x = x; s.y = y; s.rot = rot;
		s.sx = std::clamp(sx, 0.05f, 4.f);
		s.sy = std::clamp(sy, 0.05f, 4.f);
		s.color = color;
		s.front = true;
		return s;
	};

	// ---- 1) mortar backing: hides the original blocks, so seams read as dark mortar (merged per row)
	auto genBacking = [&]() {
		std::vector<DraftSpec> out;
		for (int j = 0; j < m_rows; j++) {
			int i = 0;
			while (i < m_cols) {
				if (rows[j][i] != '#') { i++; continue; }
				int st = i;
				while (i < m_cols && rows[j][i] == '#') i++;
				out.push_back(mk(st + (i - st) / 2.f, j + 0.5f, 0.f, (i - st) + 0.04f, 1.04f, mortar));
			}
		}
		// slopes: stacked horizontal slices (0.25 high) that approximate the triangle; the outline hides the steps
		for (auto const& t : m_tris) {
			float dx = t.ax - t.px, dy = t.by - t.py;
			float w = std::abs(dx), h = std::abs(dy);
			if (w < 0.05f || h < 0.05f) continue;
			int n = std::max(1, (int)std::ceil(h / 0.25f));
			float rh = h / n;
			for (int q = 0; q < n; q++) {
				float vfrac = (q + 0.5f) * rh / h;
				float width = w * (1.f - vfrac);
				if (width < 0.06f) continue;
				float ymid = t.py + (dy > 0.f ? 1.f : -1.f) * (q + 0.5f) * rh;
				float xmid = t.px + (dx > 0.f ? 1.f : -1.f) * width / 2;
				out.push_back(mk(xmid, ymid, 0.f, width + 0.02f, rh + 0.02f, mortar));
			}
		}
		return out;
	};

	// ---- 2) main surface pieces with bevel
	auto genBricks = [&](float k, int bevel) {
		std::vector<DraftSpec> out;
		std::mt19937 g(seed);
		float w = bw * k, h = bh * k;
		bool stagger = pattern != "tiles";
		float irr = pattern == "stones" ? 0.3f : pattern == "cobble" ? 0.45f : 0.f;
		float e = 0.05f;
		float y = 0.f;
		int r = 0;
		int N = (int)base.size();

		auto cornersIn = [&](float L, float R, float B2, float T) {
			return occ(L + e, B2 + e) && occ(R - e, B2 + e) && occ(L + e, T - e) && occ(R - e, T - e)
				&& occ((L + R) / 2, (B2 + T) / 2);
		};
		auto place = [&](float px, float py, float pwid, float phei) {
			// pieces only ever shrink, so neighbours never overlap
			float fw = std::max(0.12f, (pwid - gap) * (1.f - frand(g, 0.f, sizeJ) * 0.5f));
			float fh = std::max(0.12f, (phei - gap) * (1.f - frand(g, 0.f, sizeJ) * 0.5f));
			// damage: leave this piece out; the mortar backing shows through, optionally with a glow inside
			if (missing > 0.f && frand(g, 0.f, 1.f) < missing) {
				if (!holeCol.empty()) out.push_back(mk(px, py, 0.f, fw * 0.8f, fh * 0.7f, holeCol));
				return;
			}
			float rot = frand(g, -rotJ, rotJ);
			// color: smooth patches + lighter toward the top + a little randomness
			float n = vnoise(px / 2.5f, py / 2.5f, seed);
			n = std::clamp(0.5f + (n - 0.5f) * 1.5f, 0.f, 1.f);
			float yf = m_rows > 0 ? py / (float)m_rows : 0.5f;
			float tt = n * 0.55f + yf * 0.2f + frand(g, 0.f, 0.25f);
			int ci = std::clamp((int)(tt * N), 0, N - 1);
			out.push_back(mk(px, py, rot, fw, fh, base[ci]));
			if (bevel >= 1 && fw > 0.45f && fh > 0.28f) {
				float sr = std::sin(rot * 0.0174533f), cr = std::cos(rot * 0.0174533f);
				float d = fh / 2 - 0.045f;
				out.push_back(mk(px + d * sr, py + d * cr, rot, fw * 0.94f, 0.07f, hl));
				out.push_back(mk(px - d * sr, py - d * cr, rot, fw * 0.94f, 0.07f, mortar));
				if (bevel >= 2 && fw > 0.6f) {
					// light from the top-left: highlight on the left edge, shadow on the right edge
					float dxs = fw / 2 - 0.035f;
					out.push_back(mk(px - dxs * cr, py + dxs * sr, rot, 0.06f, fh * 0.86f, hl));
					out.push_back(mk(px + dxs * cr, py - dxs * sr, rot, 0.06f, fh * 0.86f, mortar));
				}
			}
		};
		// Fit a piece; if it pokes outside the shape (block edge or slope diagonal), split it
		// into quarters (up to 2 levels) so edges follow the silhouette with smaller bricks.
		std::function<void(float, float, float, float, int)> fill = [&](float L, float R, float B2, float T, int depth) {
			float ww = R - L, hh = T - B2;
			if (ww < 0.12f || hh < 0.12f) return;
			if (cornersIn(L, R, B2, T)) { place((L + R) / 2, (B2 + T) / 2, ww, hh); return; }
			if (depth >= 2) return;
			float mx = (L + R) / 2, my = (B2 + T) / 2;
			fill(L, mx, B2, my, depth + 1); fill(mx, R, B2, my, depth + 1);
			fill(L, mx, my, T, depth + 1); fill(mx, R, my, T, depth + 1);
		};

		while (y < (float)m_rows - 0.05f) {
			float rh = h * (irr > 0 ? frand(g, 1.f - irr, 1.f + irr) : 1.f);
			float x = stagger ? (irr > 0 ? -frand(g, 0.f, w) : ((r % 2) ? -w * 0.5f : 0.f)) : 0.f;
			while (x < (float)m_cols - 0.05f) {
				float pw = w * (irr > 0 ? frand(g, 1.f - irr, 1.f + irr) : 1.f);
				float l = x, rg = x + pw, b = y, t = y + rh;
				x += pw;
				if (!m_overhang) {
					l = std::max(l, 0.f); rg = std::min(rg, (float)m_cols);
					b = std::max(b, 0.f); t = std::min(t, (float)m_rows);
				}
				float ww = rg - l, hh = t - b;
				if (ww < 0.15f || hh < 0.15f) continue;
				float cx = (l + rg) / 2, cy = (b + t) / 2;

				if (m_overhang) {
					if (occ(cx, cy)) place(cx, cy, ww, hh);
				} else {
					fill(l, rg, b, t, 0);
				}
			}
			y += rh;
			r++;
		}
		return out;
	};

	// Slope triangle edges with outward normals (for crust / outline along diagonals)
	struct Seg { float x0, y0, x1, y1, nx, ny; };
	auto segsOf = [](SlopeTri const& t) {
		std::vector<Seg> out;
		float vx[3] = {t.px, t.ax, t.bx}, vy[3] = {t.py, t.ay, t.by};
		int pairs[3][3] = {{0, 1, 2}, {0, 2, 1}, {1, 2, 0}}; // segment endpoints + opposite vertex
		for (auto& pr : pairs) {
			Seg sg{vx[pr[0]], vy[pr[0]], vx[pr[1]], vy[pr[1]], 0.f, 0.f};
			float dx = sg.x1 - sg.x0, dy = sg.y1 - sg.y0;
			float len = std::sqrt(dx * dx + dy * dy);
			if (len < 1e-3f) continue;
			float nx = -dy / len, ny = dx / len;
			if (nx * (vx[pr[2]] - sg.x0) + ny * (vy[pr[2]] - sg.y0) > 0.f) { nx = -nx; ny = -ny; } // point away from interior
			sg.nx = nx; sg.ny = ny;
			out.push_back(sg);
		}
		return out;
	};
	// lay rotated strips along a segment, only where the outside is open
	auto stripsAlong = [&](Seg const& sg, float thick, bool varyDepth, std::string const& color, std::vector<DraftSpec>& out) {
		float dx = sg.x1 - sg.x0, dy = sg.y1 - sg.y0;
		float len = std::sqrt(dx * dx + dy * dy);
		int cnt = std::max(1, (int)std::ceil(len / 0.5f));
		float ang = -std::atan2(dy, dx) * 57.29578f; // clockwise degrees
		if (ang < -90.f) ang += 180.f;
		if (ang > 90.f) ang -= 180.f;
		for (int q = 0; q < cnt; q++) {
			float tpos = (q + 0.5f) / cnt;
			float mx = sg.x0 + dx * tpos, my = sg.y0 + dy * tpos;
			if (occ(mx + sg.nx * 0.12f, my + sg.ny * 0.12f)) continue; // covered by something else
			float th = thick;
			if (varyDepth) th = thick * (0.45f + 0.65f * vnoise(mx * 1.3f, my * 1.3f + 11.f, seed + 5));
			float cxp = mx - sg.nx * (th / 2 - 0.04f), cyp = my - sg.ny * (th / 2 - 0.04f);
			out.push_back(mk(cxp, cyp, ang, len / cnt * 1.3f + 0.02f, th, color));
		}
	};

	// ---- 3) edge crust along exposed upward-facing surfaces (blocks and slope diagonals)
	auto genEdge = [&]() {
		std::vector<DraftSpec> out;
		if (edgeStyle == "none") return out;
		std::mt19937 g(seed + 1);
		bool drips = edgeStyle == "moss" || edgeStyle == "slime" || edgeStyle == "lava" || edgeStyle == "frost" || edgeStyle == "blood";
		for (int j = 0; j < m_rows; j++) for (int i = 0; i < m_cols; i++) {
			if (rows[j][i] != '#' || occ(i + 0.5f, j + 1.08f)) continue;
			float n = vnoise(i * 0.8f, 7.f, seed + 5);
			float depth = edgeDepth * (0.6f + 0.9f * n);
			if (glowy) glowLayer.push_back(mk(i + 0.5f, j + 1.f - depth * 0.9f, 0.f, 1.02f, depth * 2.6f + 0.12f, glowEdge));
			out.push_back(mk(i + 0.5f, j + 1.f - depth / 2 + 0.04f, 0.f, 1.03f, depth, edgeColor));
			if (drips && frand(g, 0.f, 1.f) < 0.3f) {
				float len = frand(g, 0.25f, 0.8f);
				out.push_back(mk(i + frand(g, 0.2f, 0.8f), j + 1.f - depth - len / 2 + 0.04f, 0.f,
					frand(g, 0.14f, 0.26f), len, edgeColor));
			}
		}
		for (auto const& t : m_tris)
			for (auto const& sg : segsOf(t))
				if (sg.ny > 0.3f) {
					if (glowy) stripsAlong(sg, edgeDepth * 2.4f, false, glowEdge, glowLayer);
					stripsAlong(sg, edgeDepth, true, edgeColor, out);
				}
		return out;
	};

	// ---- 4) scattered accents (speckles or cracks)
	auto genAccent = [&]() {
		std::vector<DraftSpec> out;
		if (accDens <= 0.f) return out;
		std::mt19937 g(seed + 2);
		float occupied = 0.f;
		for (int j = 0; j < m_rows; j++) for (int i = 0; i < m_cols; i++) if (rows[j][i] == '#') occupied += 1.f;
		for (auto const& t : m_tris) occupied += 0.5f * std::abs(t.ax - t.px) * std::abs(t.by - t.py);
		int n = (int)(occupied * accDens);
		for (int tries = 0; tries < n * 8 && (int)out.size() < n; tries++) {
			float x = frand(g, 0.f, (float)m_cols), y = frand(g, 0.f, (float)m_rows);
			if (!occ(x, y)) continue;
			if (accType == "cracks") {
				float len = frand(g, 0.4f, 1.1f);
				if (!occ(x - 0.3f, y) || !occ(x + 0.3f, y) || !occ(x, y - 0.3f) || !occ(x, y + 0.3f)) continue;
				float rotc = frand(g, 0.f, 180.f);
				if (accGlowy) accGlowLayer.push_back(mk(x, y, rotc, 0.26f, len + 0.15f, glowAcc));
				out.push_back(mk(x, y, rotc, 0.07f, len, accColor));
			} else {
				float sz = frand(g, 0.12f, 0.3f);
				out.push_back(mk(x, y, frand(g, 0.f, 45.f), sz, sz, accColor));
			}
		}
		return out;
	};

	// ---- 4b) vertical shading: darker toward the bottom (transparent black overlay on whole-cell rows)
	auto genShade = [&]() {
		std::vector<DraftSpec> out;
		if (!fx || m_rows < 3) return out;
		for (int j = 0; j < m_rows; j++) {
			float f = (float)j / (float)(m_rows - 1);
			std::string col;
			if (f < 0.34f) col = "#000000|a40";
			else if (f < 0.67f) col = "#000000|a20";
			else continue;
			int i = 0;
			while (i < m_cols) {
				if (rows[j][i] != '#') { i++; continue; }
				int st = i;
				while (i < m_cols && rows[j][i] == '#') i++;
				out.push_back(mk(st + (i - st) / 2.f, j + 0.5f, 0.f, (float)(i - st), 1.f, col));
			}
		}
		return out;
	};

	// ---- 4c) trim: horizontal band every N rows (cornice / stripe / beam), only where solid blocks sit above and below
	auto genTrim = [&]() {
		std::vector<DraftSpec> out;
		if (trimEvery < 2) return out;
		for (int b = trimEvery; b < m_rows; b += trimEvery) {
			int i = 0;
			while (i < m_cols) {
				if (!(rows[b - 1][i] == '#' && rows[b][i] == '#')) { i++; continue; }
				int st = i;
				while (i < m_cols && rows[b - 1][i] == '#' && rows[b][i] == '#') i++;
				out.push_back(mk(st + (i - st) / 2.f, (float)b, 0.f, (float)(i - st), trimTh, trimColor));
			}
		}
		return out;
	};

	// ---- 4d) soft light rising from the bottom (additive, two opacity steps)
	auto genBottomGlow = [&]() {
		std::vector<DraftSpec> out;
		if (!fx || bgStrength <= 0.f || bgColor.empty()) return out;
		for (int j = 0; j < m_rows; j++) {
			float f = (j + 0.5f) / (float)m_rows;
			if (f >= bgHeight) break;
			float t = 1.f - f / bgHeight;
			int op = (int)std::lround(bgStrength * (t > 0.5f ? 45.f : 22.f));
			if (op < 4) continue;
			std::string col = fmt::format("{}|a{}|add", bgColor, op);
			int i = 0;
			while (i < m_cols) {
				if (rows[j][i] != '#') { i++; continue; }
				int st = i;
				while (i < m_cols && rows[j][i] == '#') i++;
				out.push_back(mk(st + (i - st) / 2.f, j + 0.5f, 0.f, (float)(i - st), 1.f, col));
			}
		}
		return out;
	};

	// ---- 5) crisp outline around the exposed silhouette (including slope diagonals)
	auto genOutline = [&]() {
		std::vector<DraftSpec> out;
		float th = 0.1f;
		for (int j = 0; j < m_rows; j++) for (int i = 0; i < m_cols; i++) {
			if (rows[j][i] != '#') continue;
			if (!occ(i - 0.1f, j + 0.5f)) out.push_back(mk(i + th / 2, j + 0.5f, 0.f, th, 1.f, mortar));
			if (!occ(i + 1.1f, j + 0.5f)) out.push_back(mk(i + 1.f - th / 2, j + 0.5f, 0.f, th, 1.f, mortar));
			if (!occ(i + 0.5f, j - 0.1f)) out.push_back(mk(i + 0.5f, j + th / 2, 0.f, 1.f, th, mortar));
			if (edgeStyle == "none" && !occ(i + 0.5f, j + 1.1f))
				out.push_back(mk(i + 0.5f, j + 1.f - th / 2, 0.f, 1.f, th, mortar));
		}
		for (auto const& t : m_tris)
			for (auto const& sg : segsOf(t)) {
				if (edgeStyle != "none" && sg.ny > 0.3f) continue; // crust already covers upward-facing edges
				stripsAlong(sg, th, false, mortar, out);
			}
		return out;
	};

	int L = limit();
	auto backing = genBacking();
	auto shade = genShade();
	auto trimBands = genTrim();
	auto bottomGlow = genBottomGlow();
	auto edge = genEdge();   // also fills glowLayer
	auto acc = genAccent();  // also fills accGlowLayer
	auto outline = genOutline();
	std::vector<DraftSpec> glow = glowLayer;
	glow.insert(glow.end(), accGlowLayer.begin(), accGlowLayer.end());

	std::mt19937 trim(seed + 3);
	size_t edgeCap = (size_t)std::max(1, (int)(L * 0.25f)), accCap = (size_t)std::max(1, (int)(L * 0.12f));
	size_t glowCap = (size_t)std::max(1, (int)(L * 0.15f));
	if (edge.size() > edgeCap) { std::shuffle(edge.begin(), edge.end(), trim); edge.resize(edgeCap); }
	if (acc.size() > accCap) { std::shuffle(acc.begin(), acc.end(), trim); acc.resize(accCap); }
	if (glow.size() > glowCap) { std::shuffle(glow.begin(), glow.end(), trim); glow.resize(glowCap); }
	if ((int)outline.size() > (int)(L * 0.2f)) outline.clear();
	if ((int)trimBands.size() > (int)(L * 0.08f)) trimBands.clear();
	if ((int)bottomGlow.size() > (int)(L * 0.1f)) bottomGlow.clear();
	auto reserveOf = [&]() {
		return (int)(backing.size() + shade.size() + trimBands.size() + bottomGlow.size()
			+ glow.size() + edge.size() + acc.size() + outline.size());
	};
	if (reserveOf() > (int)(L * 0.6f)) { outline.clear(); acc.clear(); glow.clear(); shade.clear(); trimBands.clear(); bottomGlow.clear(); }
	if (reserveOf() > (int)(L * 0.6f)) backing.clear();
	int reserve = reserveOf();

	// Prefer richer bevels (level 2 = 5 objects per brick, level 1 = 3); fall back to plain pieces if too big.
	std::vector<DraftSpec> bricks;
	bool fitted = false;
	for (int lvl = 2; lvl >= 1 && !fitted; lvl--) {
		float kmax = lvl == 2 ? 1.35f : 1.7f;
		for (float k = 1.f; k <= kmax && !fitted; k *= 1.08f) {
			bricks = genBricks(k, lvl);
			fitted = (int)bricks.size() + reserve <= L;
		}
	}
	for (float k = 1.f; !fitted && k <= 20.f; k *= 1.12f) {
		bricks = genBricks(k, 0);
		fitted = (int)bricks.size() + reserve <= L;
	}
	if ((int)bricks.size() + reserve > L) bricks.resize(std::max(0, L - reserve));

	std::vector<DraftSpec> all;
	all.reserve(bricks.size() + reserve);
	// order = draw order: mortar, bricks (+ glow in damaged holes), shading, trim, bottom glow, glow, accents, outline, crust on top
	for (auto& s2 : backing) all.push_back(s2);
	for (auto& s2 : bricks) all.push_back(s2);
	for (auto& s2 : shade) all.push_back(s2);
	for (auto& s2 : trimBands) all.push_back(s2);
	for (auto& s2 : bottomGlow) all.push_back(s2);
	for (auto& s2 : glow) all.push_back(s2);
	for (auto& s2 : acc) all.push_back(s2);
	for (auto& s2 : outline) all.push_back(s2);
	for (auto& s2 : edge) all.push_back(s2);
	return all;
}

void ArtAgentPopup::clearDraft() {
	auto lel = m_ui->m_editorLayer;
	for (auto& o : m_draft) lel->removeObject(o, true);
	m_draft.clear();
	// give the original slopes their previous color back
	if (!m_recolored.empty()) {
		auto arr = CCArray::create();
		for (auto& [so, oldId] : m_recolored) {
			GameObject* o = so;
			if (o->m_baseColor) { o->m_baseColor->m_colorID = oldId; arr->addObject(o); }
		}
		lel->updateObjectColors(arr);
		m_recolored.clear();
	}
}

void ArtAgentPopup::spawnDraft(std::vector<DraftSpec> const& specs) {
	auto lel = m_ui->m_editorLayer;
	std::vector<GJEffectManager*> mgrs;
	if (lel->m_levelSettings && lel->m_levelSettings->m_effectManager)
		mgrs.push_back(lel->m_levelSettings->m_effectManager);
	if (lel->m_effectManager && (mgrs.empty() || mgrs[0] != lel->m_effectManager))
		mgrs.push_back(lel->m_effectManager);
	int base = (int)Mod::get()->getSettingValue<int64_t>("color-channel-start");
	int layer = (int)Mod::get()->getSettingValue<int64_t>("editor-layer");
	std::map<std::string, int> channels;
	std::map<int, ccColor3B> chanColor;
	std::map<int, float> chanOp;
	std::vector<std::pair<GameObject*, float>> translucent;
	auto fresh = CCArray::create();

	// one color channel per distinct color (max 16); returns -1 if none could be made
	auto chanFor = [&](std::string const& color) -> int {
		auto it = channels.find(color);
		if (it != channels.end()) return it->second;
		if (channels.size() >= 16) return channels.empty() ? -1 : channels.begin()->second; // reuse one
		int ch = base + (int)channels.size();
		// "#RRGGBB|a30|add": optional 30% opacity and additive blending
		std::string hexPart = color;
		float op = 1.f;
		bool add = false;
		if (auto bar = color.find('|'); bar != std::string::npos) {
			hexPart = color.substr(0, bar);
			std::stringstream ss(color.substr(bar + 1));
			std::string tok;
			while (std::getline(ss, tok, '|')) {
				if (tok == "add") add = true;
				else if (tok.size() > 1 && tok[0] == 'a') op = std::clamp((float)std::atoi(tok.c_str() + 1) / 100.f, 0.f, 1.f);
			}
		}
		auto c = cc3bFromHexString(hexPart).unwrapOr(ccc3(255, 255, 255));
		// Write the channel to BOTH managers: the level settings one is what gets saved and
		// what playtest loads; the live one is what the editor is drawing right now.
		for (auto m : mgrs) {
			auto action = m->getColorAction(ch);
			if (!action) continue;
			action->m_colorID = ch;
			action->m_color = c;
			action->m_fromColor = c;
			action->m_toColor = c;
			action->m_duration = 0.f;
			action->m_deltaTime = 0.f;
			action->m_blending = add;
			action->m_fromOpacity = op;
			action->m_toOpacity = op;
			action->m_currentOpacity = op;
			action->m_playerColor = -1; // no player color
			action->m_copyID = 0;
			m->colorActionChanged(action);
		}
		channels[color] = ch;
		chanColor[ch] = c;
		chanOp[ch] = op;
		return ch;
	};
	auto tint = [&](GameObject* obj, int ch) {
		if (ch < 0) return;
		if (obj->m_baseColor) {
			obj->m_baseColor->m_colorID = ch;
			obj->m_baseColor->m_usesHSV = false;
		}
		// direct tint too, so the preview is coloured even before the channel refresh lands
		obj->setObjectColor(chanColor[ch]);
		if (chanOp[ch] < 0.99f) {
			obj->setOpacity((GLubyte)(chanOp[ch] * 255.f));
			translucent.push_back({obj, chanOp[ch]});
		}
	};

	for (auto const& s : specs) {
		auto pos = ccp(m_origin.x + s.x * B, m_origin.y + s.y * B);
		auto obj = lel->createObject(s.id, pos, false);
		if (!obj) continue;
		obj->setRotation(s.rot);
		obj->updateCustomScaleX(s.sx);
		obj->updateCustomScaleY(s.sy);
		if (s.front) obj->m_zLayer = ZLayer::T1;
		obj->m_editorLayer = (short)layer;
		tint(obj, chanFor(s.color));
		fresh->addObject(obj);
		m_draft.push_back(obj);
	}

	// Original slopes: tint them with the mortar color so they act as the mortar backing under the bricks.
	if (Mod::get()->getSettingValue<bool>("recolor-slopes") && !m_slopeObjs.empty() && !m_mortarHex.empty()) {
		int mch = chanFor(m_mortarHex);
		if (mch >= 0) {
			for (auto& so : m_slopeObjs) {
				GameObject* o = so;
				if (!o->m_baseColor) continue;
				bool known = false;
				for (auto& r : m_recolored) if (r.first == so) known = true;
				if (!known) m_recolored.push_back({so, o->m_baseColor->m_colorID});
				tint(o, mch);
				fresh->addObject(o);
			}
		}
	}

	// make the editor recompute channel colors and redraw the new objects
	for (auto m : mgrs) m->calculateBaseActiveColors();
	lel->updateObjectColors(fresh);
	// make sure the editor preview shows the transparency even if the refresh reset it
	for (auto& [o, op] : translucent) o->setOpacity((GLubyte)(op * 255.f));
}
