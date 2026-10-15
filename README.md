# Art Agent

Select blocks in the editor, press the pink **AI** button, describe the look, and Gemini drafts decoration for it.

## Installing

A source folder does nothing inside `geode/mods`. Geode only loads a built `.geode` file.

### Easiest: build online (no setup)
1. Make a free GitHub repo and upload everything in this folder.
2. Open the **Actions** tab, then **Build Mod**, then **Run workflow**.
3. When it finishes, download **Build Output** and unzip it. You get `yourname.art-agent.geode`.
4. Put that file in `%localappdata%/GeometryDash/geode/mods` (or use Geode's "Install from file").

### Build locally
Install the Geode SDK + CLI (docs.geode-sdk.org), then:
```
geode build
```

## Setup
Mod settings, paste a free Gemini key from aistudio.google.com.

Before publishing, change `yourname` in `mod.json` (id and developer).
