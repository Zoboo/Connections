# App and puzzle updates

The app checks automatically once per launch when connected to the internet:

1. Check the NRO in the latest GitHub Release first, using small HTTP range requests to read
   its embedded app name, version, and NRO metadata.
2. If its numeric version is newer, show INSTALL / LATER. INSTALL downloads,
   validates, backs up and replaces the running NRO, then restarts through the
   homebrew loader. LATER continues to the puzzle check.
3. If the app is current (or its check is unavailable), check the puzzle archive.
   Stage and validate the JSON, then prompt if its contents differ and it is not
   older or shorter. This also detects corrections to existing puzzles. INSTALL
   promotes the staged file; LATER discards it.

Checks run in the background. Prompts appear on the main menu, including when
returning from a game. Offline or unchanged automatic checks do not open a
dialog. Select the download icon to check manually or inspect an ongoing check.
The prompts support touch and the D-pad/A/B; LATER is selected by default.
B cancels a network transfer. Selecting QUIT closes the app after cleanup.

## App releases

Publish releases at https://github.com/Zoboo/Connections/releases.
Keep release asset names `Connections.nro` and `Connections.nsp`: the updater
uses `/releases/latest/download/` to discover the latest published assets.

For each new release, increase `CONNECTIONS_APP_VERSION` in
`source/core/version.h`, rebuild, and attach the verified files to a published
GitHub Release. Increase `CONNECTIONS_PACKAGE_REVISION` for native packaging
changes. Use three numeric version components, e.g. `1.2.0`.

Self-update requires launch from an SD-card NRO with a valid `argv[0]` and a
homebrew loader supporting `envSetNextLoad` (such as hbmenu/hbloader). The updater
uses that exact launch path, checks its identity/version, and never guesses an
installation location. Unsupported launch modes leave the executable untouched.

NRO downloads use `<launch path>.update.tmp` (maximum 64 MiB). Validation checks
NRO/ASET headers, segment and asset bounds, the Connections app name, and the
embedded numeric version. The full file must match the version, build ID and
size checked before the prompt. HTML/error pages, changed downloads,
invalid files, and downgrades are rejected. HTTPS certificate verification is
enabled. The old executable remains at `<launch path>.bak`; promotion failures
attempt rollback. ROMFS is closed before replacing its backing NRO. If a power
loss occurs between renames, restore the `.bak` file to the original `.nro` name
on the SD card. Keep this single backup until the new release is verified.

## Puzzle source

https://raw.githubusercontent.com/Eyefyre/NYT-Connections-Answers/main/connections.json

## Storage and fallback

- The target is `sdmc:/switch/Connections/connections.json`.
- Downloads first go to `connections.json.tmp`, with an 8 MiB limit, HTTPS-only
  requests/redirects, certificate verification, connection/transfer timeouts,
  and checked writes, flush, sync, and close.
- A failed request, HTTP error, malformed or incompatible archive, or older/
  shorter archive does not replace the current database.
- Before installing, the current valid database is renamed to
  `connections.json.bak`. The validated temporary file is then promoted. If
  promotion fails, restoration is attempted; a failed restoration leaves the
  valid backup available to the loader.
- Loading checks the whole SD database, then the backup, then uses the bundled
  `romfs:/connections.json`. An incomplete temporary file is never loaded.
- The built-in archive and game progress/custom-puzzle save file are untouched.

The former fallback only looked for a date string and could select an unusable
file. Validation and gameplay now share cJSON and the same schema checks: an
array of uniquely identified, dated puzzles, each with four answer groups and
four string members per group. Object key order, whitespace, Unicode escapes,
and added metadata are supported. Required fields, types, bounds, duplicate
IDs, nesting, and the entire input are checked before installation. A changed
schema is rejected safely rather than guessed.

The archive's existing level `-1` is supported. Group names retain the existing
display-length limit without splitting UTF-8 characters. Records with empty or
duplicate words (including the archive's symbol-only April Fools puzzle) are
excluded from play; an archive must contain playable puzzles. Words longer
than the game's capacity are rejected rather than silently truncated.

## Build and checks

Requires devkitPro's `switch-curl` alongside the project's existing Switch/SDL
dependencies (`pacman -S switch-curl` in a configured devkitPro environment).
cJSON 1.7.19 is vendored under `source/libs/cjson` with its MIT license.
Build with `make`; `CURL_ROOT` can point to a separate Switch curl prefix.

The portable tests compile the production parser and installer with a host C
compiler. For example:

```sh
mkdir -p build
cc -Isource -DCJSON_NESTING_LIMIT=64 tests/puzzle_update_driver.c \
  source/game/connections_puzzle.c source/libs/cjson/cJSON.c \
  -lm -o build/puzzle_update_tests
python tests/test_puzzle_update.py build/puzzle_update_tests -v
cc -Isource tests/app_update_driver.c -o build/app_update_tests
python tests/test_app_update.py build/app_update_tests -v
cc -Itests/stubs -Isource tests/update_flow_tests.c source/core/app_update.c \
  -o build/update_flow_tests
build/update_flow_tests
```

Tests cover the bundled archive (and a downloaded archive if present at
`build/dependencies/upstream-connections.json`), reordered keys, extra metadata,
Unicode, incompatible schemas, invalid/partial payloads, size limits, unplayable
records, backups, downgrade rejection, and injected rename/rollback failures.

The NRO tests cover numeric versions, malformed/truncated binaries, launch-path
bounds, and replacement/rollback failures. Update-flow tests replace Switch,
SDL and network services with host stubs to verify app-first sequencing,
confirmation before installation, skip/cancel/offline behavior, and restart
requests. These stubs are used only by tests, never by the Switch build.

Hardware checks: launch online/offline, use both prompt choices, quit during a
check, install a newer NRO through hbmenu, and check the backup/bundled puzzle
fallback. Actual console network services, the visual dialog and loader restart
cannot be exercised by the host tests.
