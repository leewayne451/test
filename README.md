# Art Agent

Select blocks in the editor, press the pink **AI** button, describe the look (any theme), and Gemini drafts decoration for it.

## Reference screenshots (optional)
Show the AI levels whose look you like:
1. Open a level in the editor and press the green **Ref** button. It saves a screenshot of the view as `snap-<number>.png` in the mod's `references` folder.
   You can also drop any PNG/JPG there yourself (the **Dir** button in the Art Agent panel opens the folder).
2. In the Art Agent panel, type the file names in the **Refs** box, comma separated (extension optional), e.g. `snap-1760611200, castle`.
3. Press **Go**. Up to 4 images (6 MB each) are sent with your first request and the AI matches their palette and feel.

Your last Refs list is remembered. Screenshots go to Google's Gemini API like the rest of your request.

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
