#include <cctype>
#include "ArtAgentPopup.hpp"
#include "Palette.hpp"
#include <algorithm>
#include <map>
#include <sstream>

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
	std::ostringstream os;
	os << "You are the art agent inside Geometry Dash's level editor. The player built a structure from "
	      "blocks; you decorate it so it looks like what they describe (stone bricks, moss, wood, ice...).\n\n"
	      "Reply with ONLY one JSON object, no prose, no code fences:\n"
	      "{\"summary\":\"1-2 short sentences about what you made\","
	      "\"objects\":[{\"id\":1,\"x\":0.5,\"y\":0.5,\"rot\":0,\"sx\":1,\"sy\":1,\"color\":\"#8A8A8A\",\"front\":true}]}\n\n"
	      "Coordinates: block units, origin at the bottom-left corner of the structure's bounding box, y points up. "
	      "x,y is an object's CENTER; the center of the bottom-left cell is (0.5, 0.5). rot is degrees clockwise. "
	      "sx/sy scale the object (0.2 to 4). front=true draws it in front of the original blocks.\n\n"
	      "Allowed objects (size at scale 1, in blocks):\n";
	for (auto const& p : palette())
		os << "- id " << p.id << ": " << p.name << " (" << p.w << " x " << p.h << ")\n";
	os << "\nRules: use only these ids; at most 8 distinct colors; never exceed the object limit; "
	      "cover the structure's surface with bricks/detail that follow its silhouette; "
	      "add variety (slight color shifts, small rotations, size changes) so it does not look tiled. "
	      "If the user asks for changes, return the COMPLETE updated object list, not a diff.";
	return os.str();
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
			{"maxOutputTokens", 60000},
		})},
	});

	auto url = fmt::format("https://generativelanguage.googleapis.com/v1beta/models/{}:generateContent",
		Mod::get()->getSettingValue<std::string>("model"));

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

void ArtAgentPopup::clearDraft() {
	auto lel = m_ui->m_editorLayer;
	for (auto& o : m_draft) lel->removeObject(o, true);
	m_draft.clear();
}

void ArtAgentPopup::spawnDraft(std::vector<DraftSpec> const& specs) {
	auto lel = m_ui->m_editorLayer;
	int base = (int)Mod::get()->getSettingValue<int64_t>("color-channel-start");
	std::map<std::string, int> channels;

	for (auto const& s : specs) {
		auto pos = ccp(m_origin.x + s.x * B, m_origin.y + s.y * B);
		auto obj = lel->createObject(s.id, pos, false);
		if (!obj) continue;
		obj->setRotation(s.rot);
		obj->updateCustomScaleX(s.sx);
		obj->updateCustomScaleY(s.sy);
		if (s.front) obj->m_zLayer = ZLayer::T1;

		// one color channel per distinct color (max 8)
		auto it = channels.find(s.color);
		if (it == channels.end() && channels.size() < 8) {
			int ch = base + (int)channels.size();
			auto c = cc3bFromHexString(s.color).unwrapOr(ccc3(255, 255, 255));
			lel->m_effectManager->getColorAction(ch)->m_color = c; // may need extra refresh calls
			it = channels.emplace(s.color, ch).first;
		}
		if (it != channels.end() && obj->m_baseColor) obj->m_baseColor->m_colorID = it->second;

		m_draft.push_back(obj);
	}
}
