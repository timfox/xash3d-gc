# Bring your own game data

Use `bash scripts/build-gamecube-byod.sh /path/to/your/valve` to build a
private GameCube disc. For other mods, use the disc builder's explicit `--data`
and `--gamedir` arguments. It never grants redistribution rights to that data.

The BYOD wrapper runs the engine build and then converts the supplied game's
assets locally. It disables automatic inclusion of sibling game directories.
The direct disc builder requires `--data`; no developer's installation is a
fallback. Conversion needs the dependencies documented by the build tools,
including Pillow/numpy and FFmpeg for the supported media conversions.

Keep source data outside the checkout or in ignored `assets-local/`. Outputs,
texture packs, converted movies/audio, menu art, model exports, screenshots,
frame dumps and generated reports stay local. The material lab's exported
textures must also be generated locally with its documented asset tools.
`gitignore` does not protect files already tracked or deliberately force-added;
run `python3 scripts/check-byod-repository.py` against the proposed Git tree.
The BYOD CI workflow runs that same check with synthetic tests and never requests
a game installation.

This cleanup removes tracked runtime/export folders and captured retail imagery
from the current index while preserving local files. It preserves upstream
source copyrights, license files and platform engine icons. Source names such
as `cursor` used for mouse pointers and functional tool-provider identifiers
are not authorship credits and remain functional.

## Existing history

Older commits contain gameplay/menu captures and assistant co-author trailers.
Removing files from the index and cleaning patch text does not erase those Git
objects or historical commit messages. A complete historical cleanup requires a
separately approved rewrite, coordinated with all clones and remotes; no rewrite
or publication is performed by this change. Submodule histories need their own
audit. Do not call this repository fully cleared of third-party content based
only on the current-tree check.
