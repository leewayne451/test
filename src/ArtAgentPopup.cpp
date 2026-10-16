#include <cmath>
#include <cctype>
#include <Geode/utils/file.hpp>
#include "ArtAgentPopup.hpp"
#include "Palette.hpp"
#include <algorithm>
#include <map>
#include <sstream>
#include <random>

static constexpr float B = 30.f; // 1 block in GD units

// Copies a JSON array into a vector (empty if the value isn't an array).
template <class V>
static std::vector<matjson::Value> arrOf(V&& v) {
	std::vector<matjson::Value> out;
	if (auto r = v.asArray(); r.isOk()) out = r.unwrap();
	return out;
}

ArtAgentPopup* ArtAgentPopup::create(EditorUI* ui, std::vector<CCRect> const& rects) {
	auto ret = new ArtAgentPopup();
	if (ret->init(ui, rects)) {
		ret->autorelease();
		return ret;
	}
	delete ret;
	return nullptr;
}

static ButtonSprite* btnSpr(char const* text, int width, char const* tex = "GJ_button_01.png") {
	return ButtonSprite::create(text, width, true, "bigFont.fnt", tex, 24.f, 0.5f);
}

bool ArtAgentPopup::init(EditorUI* ui, std::vector<CCRect> const& rects) {
	if (!Popup::init(230.f, 290.f)) return false;
	m_ui = ui;
	m_noElasticity = true;
	this->setTitle("Art Agent");
	this->setOpacity(0); // don't dim the editor: the draft is previewed live behind this panel

	// dock the panel on the right so the level stays visible
	auto win = CCDirector::get()->getWinSize();
	m_mainLayer->setPositionX(m_mainLayer->getPositionX() + (win.width / 2 - m_size.width / 2 - 8));

	this->buildGrid(rects);

	m_agentText = SimpleTextArea::create("", "chatFont.fnt", 0.6f, 205.f);
	m_agentText->setAnchorPoint({0.5f, 1.f});
	m_mainLayer->addChildAtPosition(m_agentText, Anchor::Top, {0, -34});
	this->say(m_tooBig
		? "That selection is too large (max 100x80 blocks). Select a smaller part."
		: "Tell me what to draw on the " + std::to_string(rects.size()) + " selected blocks.");

	m_countLabel = CCLabelBMFont::create("", "goldFont.fnt");
	m_countLabel->setScale(0.45f);
	m_mainLayer->addChildAtPosition(m_countLabel, Anchor::Bottom, {0, 112});

	m_prompt = TextInput::create(165.f, "Describe the design...", "chatFont.fnt");
	m_prompt->setMaxCharCount(300);
	m_mainLayer->addChildAtPosition(m_prompt, Anchor::Bottom, {-24, 90});

	auto send = CCMenuItemExt::createSpriteExtra(btnSpr("Go", 30), [this](CCObject* s) { onSend(s); });
	m_buttonMenu->addChildAtPosition(send, Anchor::Bottom, {88, 90});

	auto limitLbl = CCLabelBMFont::create("Limit", "bigFont.fnt");
	limitLbl->setScale(0.35f);
	m_mainLayer->addChildAtPosition(limitLbl, Anchor::Bottom, {-88, 58});
	m_limitInput = TextInput::create(50.f, "500", "chatFont.fnt");
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

	auto variant = CCMenuItemExt::createSpriteExtra(btnSpr("Another Variant", 100, "GJ_button_04.png"),
		[this](CCObject* s) { onVariant(s); });
	m_buttonMenu->addChildAtPosition(variant, Anchor::Bottom, {-52, 28});
	auto undo = CCMenuItemExt::createSpriteExtra(btnSpr("Undo", 40, "GJ_button_06.png"),
		[this](CCObject* s) { onDiscard(s); });
	m_buttonMenu->addChildAtPosition(undo, Anchor::Bottom, {20, 28});
	auto keep = CCMenuItemExt::createSpriteExtra(btnSpr("Keep", 50),
		[this](CCObject* s) { onKeep(s); });
	m_buttonMenu->addChildAtPosition(keep, Anchor::Bottom, {78, 28});

	this->setCount();
	return true;
}

// Rasterise the selection into a block grid so the model can "see" the shape.
void ArtAgentPopup::buildGrid(std::vector<CCRect> const& rects) {
	float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
	for (auto const& r : rects) {
		minX = std::min(minX, r.getMinX()); minY = std::min(minY, r.getMinY());
		maxX = std::max(maxX, r.getMaxX()); maxY = std::max(maxY, r.getMaxY());
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
	std::ostringstream os;
	for (int j = m_rows - 1; j >= 0; j--) os << rows[j] << "\n"; // top row first
	m_grid = os.str();
}

int ArtAgentPopup::limit() {
	int v = numFromString<int>(m_limitInput->getString()).unwrapOr(500);
	return std::clamp(v, 1, 2000);
}

void ArtAgentPopup::say(std::string const& text) { m_agentText->setText(text); }

void ArtAgentPopup::setCount() {
	m_countLabel->setString(fmt::format("{} / {} objects", m_draft.size(), limit()).c_str());
}

static std::string systemPrompt() {
	return
		"You are the art director inside Geometry Dash's level editor. The player selected a structure made of blocks "
		"and describes how it should look (stone wall, wood, ice, hell...). You do NOT place objects. You choose a "
		"visual STYLE, and the mod itself tiles it precisely over the structure's silhouette, adds a mortar backing, "
		"bevel highlights/shadows and an outline.\n\n"
		"Reply with ONLY one JSON object, no prose, no code fences:\n"
		"{\"summary\":\"1-2 short sentences\","
		"\"pattern\":\"bricks|stones|tiles|planks\","
		"\"brick_w\":1.0,\"brick_h\":0.5,\"gap\":0.05,"
		"\"rotation_jitter\":1,\"size_jitter\":0.1,"
		"\"colors\":[\"#2B1414\",\"#3A1A1A\",\"#4A2222\",\"#5C2A26\"],"
		"\"mortar\":\"#0D0606\","
		"\"edge\":{\"style\":\"none|moss|grass|snow|slime|lava\",\"color\":\"#FF5A14\",\"depth\":0.25},"
		"\"accent\":{\"type\":\"spots|cracks\",\"color\":\"#FF7A1A\",\"density\":0.05}}\n\n"
		"Field meanings (sizes are in blocks, 1 block = 1 grid square):\n"
		"- pattern: bricks = running-bond rows; stones = irregular rows of uneven stones; tiles = aligned grid; planks = long horizontal boards.\n"
		"- brick_w / brick_h: size of one piece (0.25 to 4). gap: seam between pieces (0 to 0.3) showing the mortar.\n"
		"- rotation_jitter: max random tilt in degrees; keep it SMALL (0 to 2), large values look sloppy. size_jitter: 0 to 0.3.\n"
		"- colors: 3 or 4 shades of the MAIN material ordered DARKEST to LIGHTEST, close to each other. The mod darkens low areas "
		"and lightens high areas, and clusters shades into natural patches. Make them clearly distinct, but not wildly different.\n"
		"- mortar: very dark color for seams, backing and outline (darker than the darkest shade).\n"
		"- edge: decoration on TOP surfaces. moss/grass = green, snow = white, slime/lava = dripping. depth 0.1 to 0.5. "
		"For hell use lava with a bright orange color.\n"
		"- accent: scattered detail. spots = small speckles, cracks = thin slashes. density 0 to 0.3. A glowing accent color "
		"(orange for hell, cyan for ice) makes the design pop.\n\n"
		"Style hints: stone castle: stones 1x0.5, greys, moss edge, dark cracks. Wood: planks 3x0.5, gap 0.03, rotation_jitter 0, browns. "
		"Ice: tiles 1x1, pale blues, white spots, snow edge. Hell: bricks 1x0.5, very dark reds/blacks, lava edge, orange cracks. "
		"Use at most 4 colors in 'colors'.\n"
		"If the user asks for changes, return the COMPLETE updated style object.";
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
		msg = fmt::format("Structure grid, {} columns x {} rows, top row first ('#' = block, '.' = empty):\n{}\n{}\nRequest: {}",
			m_cols, m_rows, m_grid, stateNote(), text);
	} else {
		msg = fmt::format("Change request: {}\n{}", text, stateNote());
	}
	m_prompt->setString("");
	this->request(msg);
}

void ArtAgentPopup::onVariant(CCObject*) {
	if (m_busy || m_history.empty()) { this->say("Describe a design first."); return; }
	m_variant++;
	this->request(fmt::format("Give me another variant (#{}) of the same design. Take a noticeably different "
		"approach to layout, detail and color.\n{}", m_variant + 1, stateNote()));
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
	auto contents = matjson::Value::array();
	for (auto& [role, content] : m_history) {
		contents.push(matjson::makeObject({
			{"role", role == "assistant" ? "model" : "user"},
			{"parts", textParts(content)},
		}));
	}

	auto body = matjson::makeObject({
		{"systemInstruction", matjson::makeObject({{"parts", textParts(systemPrompt())}})},
		{"contents", contents},
		{"generationConfig", matjson::makeObject({
			{"responseMimeType", "application/json"}, // forces valid JSON output
			{"maxOutputTokens", 4000},
			{"thinkingConfig", matjson::makeObject({{"thinkingBudget", 0}})}, // skip slow "thinking" phase (flash models)
		})},
	});

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
		if (model.empty()) model = "gemini-2.5-flash";
	}
	auto url = fmt::format("https://generativelanguage.googleapis.com/v1beta/models/{}:generateContent", model);

	m_busy = true;
	this->say("Thinking...");

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
		this->say("I couldn't read my own answer. Try again or press Another Variant.");
		return;
	}
	auto parsed = matjson::parse(text.substr(a, b - a + 1));
	if (parsed.isErr()) {
		m_history.pop_back();
		this->say("My answer got cut off or malformed. Try a lower object limit or press Another Variant.");
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
		if (i < 0 || j < 0 || i >= m_cols || j >= m_rows) return false;
		return rows[j][i] == '#';
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
	float edgeDepth = (float)std::clamp(dbl(edgeV, "depth", 0.25), 0.1, 0.5);
	std::string accType = str(accV, "type", "spots");
	std::string accColor = hex(str(accV, "color", "#3E3E3E"), "#3E3E3E");
	float accDens = (float)std::clamp(dbl(accV, "density", 0.0), 0.0, 0.3);

	unsigned seed = 1337u + 7919u * (unsigned)m_variant + 104729u * (unsigned)m_history.size()
		+ (unsigned)(m_cols * 31 + m_rows);

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
		return out;
	};

	// ---- 2) main surface pieces with bevel
	auto genBricks = [&](float k, bool bevel) {
		std::vector<DraftSpec> out;
		std::mt19937 g(seed);
		float w = bw * k, h = bh * k;
		bool stagger = pattern != "tiles";
		float irr = pattern == "stones" ? 0.3f : 0.f;
		float e = 0.05f;
		float y = 0.f;
		int r = 0;
		int N = (int)base.size();
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

				auto cornersIn = [&](float L, float R, float B2, float T) {
					return occ(L + e, B2 + e) && occ(R - e, B2 + e) && occ(L + e, T - e) && occ(R - e, T - e)
						&& occ((L + R) / 2, (B2 + T) / 2);
				};
				auto place = [&](float px, float py, float pwid, float phei) {
					// pieces only ever shrink, so neighbours never overlap
					float fw = std::max(0.12f, (pwid - gap) * (1.f - frand(g, 0.f, sizeJ) * 0.5f));
					float fh = std::max(0.12f, (phei - gap) * (1.f - frand(g, 0.f, sizeJ) * 0.5f));
					float rot = frand(g, -rotJ, rotJ);
					// color: smooth patches + lighter toward the top + a little randomness
					float n = vnoise(px / 2.5f, py / 2.5f, seed);
					n = std::clamp(0.5f + (n - 0.5f) * 1.5f, 0.f, 1.f);
					float yf = m_rows > 0 ? py / (float)m_rows : 0.5f;
					float tt = n * 0.55f + yf * 0.2f + frand(g, 0.f, 0.25f);
					int ci = std::clamp((int)(tt * N), 0, N - 1);
					out.push_back(mk(px, py, rot, fw, fh, base[ci]));
					if (bevel && fw > 0.45f && fh > 0.28f) {
						float sr = std::sin(rot * 0.0174533f), cr = std::cos(rot * 0.0174533f);
						float d = fh / 2 - 0.045f;
						out.push_back(mk(px + d * sr, py + d * cr, rot, fw * 0.94f, 0.07f, hl));
						out.push_back(mk(px - d * sr, py - d * cr, rot, fw * 0.94f, 0.07f, mortar));
					}
				};

				if (m_overhang) {
					if (occ(cx, cy)) place(cx, cy, ww, hh);
				} else if (cornersIn(l, rg, b, t)) {
					place(cx, cy, ww, hh);
				} else {
					// partial piece along an irregular edge: fill with quarter pieces that fit
					for (int qx = 0; qx < 2; qx++) for (int qy = 0; qy < 2; qy++) {
						float ql = l + qx * ww / 2, qr = ql + ww / 2;
						float qb = b + qy * hh / 2, qt = qb + hh / 2;
						if (cornersIn(ql, qr, qb, qt)) place((ql + qr) / 2, (qb + qt) / 2, ww / 2, hh / 2);
					}
				}
			}
			y += rh;
			r++;
		}
		return out;
	};

	// ---- 3) edge crust along exposed top surfaces (continuous, follows the silhouette)
	auto genEdge = [&]() {
		std::vector<DraftSpec> out;
		if (edgeStyle == "none") return out;
		std::mt19937 g(seed + 1);
		bool drips = edgeStyle == "moss" || edgeStyle == "slime" || edgeStyle == "lava";
		for (int j = 0; j < m_rows; j++) for (int i = 0; i < m_cols; i++) {
			if (!occ(i + 0.5f, j + 0.5f) || occ(i + 0.5f, j + 1.5f)) continue;
			float n = vnoise(i * 0.8f, 7.f, seed + 5);
			float depth = edgeDepth * (0.6f + 0.9f * n);
			out.push_back(mk(i + 0.5f, j + 1.f - depth / 2 + 0.04f, 0.f, 1.03f, depth, edgeColor));
			if (drips && frand(g, 0.f, 1.f) < 0.3f) {
				float len = frand(g, 0.25f, 0.8f);
				out.push_back(mk(i + frand(g, 0.2f, 0.8f), j + 1.f - depth - len / 2 + 0.04f, 0.f,
					frand(g, 0.14f, 0.26f), len, edgeColor));
			}
		}
		return out;
	};

	// ---- 4) scattered accents (speckles or cracks)
	auto genAccent = [&]() {
		std::vector<DraftSpec> out;
		if (accDens <= 0.f) return out;
		std::mt19937 g(seed + 2);
		int occupied = 0;
		for (int j = 0; j < m_rows; j++) for (int i = 0; i < m_cols; i++) if (rows[j][i] == '#') occupied++;
		int n = (int)(occupied * accDens);
		for (int tries = 0; tries < n * 8 && (int)out.size() < n; tries++) {
			float x = frand(g, 0.f, (float)m_cols), y = frand(g, 0.f, (float)m_rows);
			if (!occ(x, y)) continue;
			if (accType == "cracks") {
				float len = frand(g, 0.4f, 1.1f);
				if (!occ(x - 0.3f, y) || !occ(x + 0.3f, y) || !occ(x, y - 0.3f) || !occ(x, y + 0.3f)) continue;
				out.push_back(mk(x, y, frand(g, 0.f, 180.f), 0.07f, len, accColor));
			} else {
				float sz = frand(g, 0.12f, 0.3f);
				out.push_back(mk(x, y, frand(g, 0.f, 45.f), sz, sz, accColor));
			}
		}
		return out;
	};

	// ---- 5) crisp outline around the exposed silhouette
	auto genOutline = [&]() {
		std::vector<DraftSpec> out;
		float th = 0.1f;
		for (int j = 0; j < m_rows; j++) for (int i = 0; i < m_cols; i++) {
			if (rows[j][i] != '#') continue;
			if (!occ(i - 0.5f, j + 0.5f)) out.push_back(mk(i + th / 2, j + 0.5f, 0.f, th, 1.f, mortar));
			if (!occ(i + 1.5f, j + 0.5f)) out.push_back(mk(i + 1.f - th / 2, j + 0.5f, 0.f, th, 1.f, mortar));
			if (!occ(i + 0.5f, j - 0.5f)) out.push_back(mk(i + 0.5f, j + th / 2, 0.f, 1.f, th, mortar));
			if (edgeStyle == "none" && !occ(i + 0.5f, j + 1.5f))
				out.push_back(mk(i + 0.5f, j + 1.f - th / 2, 0.f, 1.f, th, mortar));
		}
		return out;
	};

	int L = limit();
	auto backing = genBacking();
	auto edge = genEdge();
	auto acc = genAccent();
	auto outline = genOutline();
	std::mt19937 trim(seed + 3);
	size_t edgeCap = (size_t)std::max(1, (int)(L * 0.25f)), accCap = (size_t)std::max(1, (int)(L * 0.12f));
	if (edge.size() > edgeCap) { std::shuffle(edge.begin(), edge.end(), trim); edge.resize(edgeCap); }
	if (acc.size() > accCap) { std::shuffle(acc.begin(), acc.end(), trim); acc.resize(accCap); }
	if ((int)outline.size() > (int)(L * 0.2f)) outline.clear();
	auto reserveOf = [&]() { return (int)(backing.size() + edge.size() + acc.size() + outline.size()); };
	if (reserveOf() > (int)(L * 0.6f)) { outline.clear(); acc.clear(); }
	if (reserveOf() > (int)(L * 0.6f)) backing.clear();
	int reserve = reserveOf();

	// Prefer bevelled pieces (3 objects each); if that needs oversized pieces, fall back to plain pieces.
	std::vector<DraftSpec> bricks;
	bool fitted = false;
	for (float k = 1.f; k <= 1.6f && !fitted; k *= 1.08f) {
		bricks = genBricks(k, true);
		fitted = (int)bricks.size() + reserve <= L;
	}
	for (float k = 1.f; !fitted && k <= 20.f; k *= 1.12f) {
		bricks = genBricks(k, false);
		fitted = (int)bricks.size() + reserve <= L;
	}
	if ((int)bricks.size() + reserve > L) bricks.resize(std::max(0, L - reserve));

	std::vector<DraftSpec> all;
	all.reserve(bricks.size() + reserve);
	for (auto& s : backing) all.push_back(s); // order = draw order: backing first, edge last
	for (auto& s : bricks) all.push_back(s);
	for (auto& s : acc) all.push_back(s);
	for (auto& s : outline) all.push_back(s);
	for (auto& s : edge) all.push_back(s);
	return all;
}

void ArtAgentPopup::clearDraft() {
	auto lel = m_ui->m_editorLayer;
	for (auto& o : m_draft) lel->removeObject(o, true);
	m_draft.clear();
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
	std::map<std::string, ccColor3B> colorOf;
	auto fresh = CCArray::create();

	for (auto const& s : specs) {
		auto pos = ccp(m_origin.x + s.x * B, m_origin.y + s.y * B);
		auto obj = lel->createObject(s.id, pos, false);
		if (!obj) continue;
		obj->setRotation(s.rot);
		obj->updateCustomScaleX(s.sx);
		obj->updateCustomScaleY(s.sy);
		if (s.front) obj->m_zLayer = ZLayer::T1;
		obj->m_editorLayer = (short)layer;

		// one color channel per distinct color (max 8)
		auto it = channels.find(s.color);
		if (it == channels.end() && channels.size() < 8) {
			int ch = base + (int)channels.size();
			auto c = cc3bFromHexString(s.color).unwrapOr(ccc3(255, 255, 255));
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
				action->m_blending = false;
				action->m_playerColor = -1; // no player color
				action->m_copyID = 0;
				m->colorActionChanged(action);
			}
			it = channels.emplace(s.color, ch).first;
			colorOf[s.color] = c;
		}
		if (it == channels.end() && !channels.empty()) it = channels.begin(); // out of channels: reuse one
		if (it != channels.end()) {
			if (obj->m_baseColor) {
				obj->m_baseColor->m_colorID = it->second;
				obj->m_baseColor->m_usesHSV = false;
			}
			// direct tint too, so the preview is coloured even before the channel refresh lands
			obj->setObjectColor(colorOf[it->first]);
		}

		fresh->addObject(obj);
		m_draft.push_back(obj);
	}

	// make the editor recompute channel colors and redraw the new objects
	for (auto m : mgrs) m->calculateBaseActiveColors();
	lel->updateObjectColors(fresh);
}
