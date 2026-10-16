# Art Agent

Select blocks in the editor, press the pink **AI** button, describe the look (any theme), and Gemini drafts decoration for it.

## Reference image (optional)
1. In the editor, select blocks and press the pink **AI** button.
2. Describe the design. If you want it to follow a look you like, press **Image** and pick a PNG or JPG screenshot (one image only; press **X** to remove it).
3. Press **Go**. The image is sent with your first request and the AI matches its palette and feel.

The image is sent to Google's Gemini API along with your request.

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
Mod settings, paste a free Gemini key from aistudio.google.com. Defaults: model `gemini-3.8-flash`, object limit 800.

Before publishing, change `yourname` in `mod.json` (id and developer).
