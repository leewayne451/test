#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>
#include "ArtAgentPopup.hpp"

using namespace geode::prelude;

class $modify(ArtAgentEditorUI, EditorUI) {
	bool init(LevelEditorLayer* lel) {
		if (!EditorUI::init(lel)) return false;

		auto label = CCLabelBMFont::create("AI", "bigFont.fnt");
		auto spr = CircleButtonSprite::create(label, CircleBaseColor::Pink, CircleBaseSize::Small);
		auto btn = CCMenuItemExt::createSpriteExtra(spr, [this](CCObject*) { this->openArtAgent(); });
		btn->setID("art-agent-button"_spr);

		if (auto undoMenu = this->getChildByID("undo-menu")) {
			undoMenu->addChild(btn);
			undoMenu->updateLayout();
		} else {
			auto fallback = CCMenu::create();
			fallback->addChild(btn);
			fallback->setPosition(CCDirector::get()->getWinSize() - ccp(30, 80));
			this->addChild(fallback, 10);
		}
		return true;
	}

	void openArtAgent() {
		CCArray* sel = this->getSelectedObjects();
		if (!sel || sel->count() == 0) {
			FLAlertLayer::create("Art Agent", "Select the blocks you want to decorate first.", "OK")->show();
			return;
		}
		std::vector<CCRect> rects, hazards;
		std::vector<Ref<GameObject>> slopes;
		for (auto obj : CCArrayExt<GameObject*>(sel)) {
			// spikes / saws are left untouched; slopes are tiled as triangles; everything else as full blocks
			if (obj->m_objectType == GameObjectType::Hazard) hazards.push_back(obj->getObjectRect());
			else if (obj->m_objectType == GameObjectType::Slope) slopes.push_back(obj);
			else rects.push_back(obj->getObjectRect());
		}
		if (rects.empty() && slopes.empty()) {
			FLAlertLayer::create("Art Agent", "Select at least one solid block or slope to decorate (hazards alone can't be tiled).", "OK")->show();
			return;
		}
		ArtAgentPopup::create(this, rects, hazards, slopes)->show();
	}
};
