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

class ArtAgentPopup : public Popup {
protected:
	EditorUI* m_ui = nullptr;
	CCPoint m_origin{0, 0};
	int m_cols = 0, m_rows = 0;
	std::string m_grid;
	bool m_tooBig = false;
	bool m_busy = false;
	bool m_overhang = false;
	int m_variant = 0;

	TextInput* m_prompt = nullptr;
	TextInput* m_limitInput = nullptr;
	SimpleTextArea* m_agentText = nullptr;
	CCLabelBMFont* m_countLabel = nullptr;

	std::vector<std::pair<std::string, std::string>> m_history; // role, content
	std::vector<Ref<GameObject>> m_draft;
	async::TaskHolder<web::WebResponse> m_task;

	bool init(EditorUI* ui, std::vector<CCRect> const& rects);
	void buildGrid(std::vector<CCRect> const& rects);
	int limit();
	void say(std::string const& text);
	void setCount();

	void onSend(CCObject*);
	void onVariant(CCObject*);
	void onDiscard(CCObject*);
	void onKeep(CCObject*);
	void onClose(CCObject*) override;

	std::string stateNote();
	void request(std::string userMsg);
	void handleReply(std::string text);
	void clearDraft();
	void spawnDraft(std::vector<DraftSpec> const& specs);

public:
	static ArtAgentPopup* create(EditorUI* ui, std::vector<CCRect> const& rects);
};
