#pragma once
#include <Geode/Geode.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>

using namespace geode::prelude;

struct DraftSpec {
	int id = 1;
	float x = 0, y = 0, rot = 0, sx = 1, sy = 1;
	std::string color = "#FFFFFF";
	bool front = true;
};

// A slope as a right triangle in grid units: right-angle vertex P, horizontal leg end A, vertical leg end B.
struct SlopeTri {
	float px, py, ax, ay, bx, by;
	bool rising; // hypotenuse rises to the right
};

class ArtAgentPopup : public Popup {
protected:
	EditorUI* m_ui = nullptr;
	CCPoint m_origin{0, 0};
	int m_cols = 0, m_rows = 0;
	std::string m_grid;
	bool m_tooBig = false;
	std::vector<SlopeTri> m_tris;
	std::vector<Ref<GameObject>> m_slopeObjs;
	std::vector<std::pair<Ref<GameObject>, int>> m_recolored; // original slope + its previous color channel
	std::string m_mortarHex;
	bool m_busy = false;
	bool m_overhang = false;
	unsigned m_seed = 0; // layout seed: fixed for the life of the panel, so follow-up requests keep the same brick layout

	TextInput* m_prompt = nullptr;
	TextInput* m_limitInput = nullptr;
	CCLabelBMFont* m_imgLabel = nullptr;
	// the single optional reference image (read and base64-encoded when picked)
	std::string m_imgName, m_imgMime, m_imgB64;
	async::TaskHolder<geode::utils::file::PickResult> m_pickTask;
	SimpleTextArea* m_agentText = nullptr;
	CCLabelBMFont* m_countLabel = nullptr;

	std::vector<std::pair<std::string, std::string>> m_history; // role, content
	std::vector<Ref<GameObject>> m_draft;
	async::TaskHolder<web::WebResponse> m_task;

	bool init(EditorUI* ui, std::vector<CCRect> const& rects, std::vector<CCRect> const& hazards, std::vector<Ref<GameObject>> const& slopes);
	void buildGrid(std::vector<CCRect> const& rects, std::vector<CCRect> const& hazards, std::vector<Ref<GameObject>> const& slopes);
	int limit();
	void say(std::string const& text);
	void setCount();

	void onSend(CCObject*);
	void onPickImage(CCObject*);
	void onClearImage(CCObject*);
	void setImage(std::filesystem::path const& path);
	void onDiscard(CCObject*);
	void onKeep(CCObject*);
	void onClose(CCObject*) override;

	std::string stateNote();
	void request(std::string userMsg);
	void handleReply(std::string text);
	std::string styleReadout(matjson::Value v);
	std::vector<DraftSpec> generateDraft(matjson::Value v);
	void clearDraft();
	void spawnDraft(std::vector<DraftSpec> const& specs);

public:
	static ArtAgentPopup* create(EditorUI* ui, std::vector<CCRect> const& rects, std::vector<CCRect> const& hazards, std::vector<Ref<GameObject>> const& slopes);
};
