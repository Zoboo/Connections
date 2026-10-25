# Connections for Nintendo Switch

An independent homebrew word-grouping game with daily puzzles, custom puzzles, history, and multiplayer. Not affiliated with The New York Times.

Copy `Connections.nro` from GitHub Releases to `sdmc:/switch/Connections/` and launch it with the Homebrew Menu. Assets are embedded in the NRO.

Saves and downloaded puzzles are stored in `sdmc:/switch/Connections/`. Keep this directory when updating. NRO updates retain a `.bak` copy beside the launched executable.

Build with devkitPro/devkitA64, libnx, the SDL2 Switch portlibs, and curl: `make nro`. On Windows, run `./packaging/build.ps1`. See `UPDATE.md` for updating and portable tests.

The release source is configured in `source/platform/platform_updates.c`. Native package installation requires a compatible homebrew environment. Third-party library and font notices remain in their respective directories.

Public builds include two original sample puzzles and licensed Inter fonts. Unverified artwork, sounds, and the original puzzle archive are excluded; menus use text fallbacks. Your own compatible puzzle database can be placed at `sdmc:/switch/Connections/connections.json`.

Downloads and updates: [GitHub Releases](https://github.com/Zoboo/Connections/releases/latest).

See [third-party notices](THIRD_PARTY_NOTICES.txt) for bundled library, font and content licenses.
