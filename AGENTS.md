# Reload Maker V2

Read `F:/fo4dev/PROJECT_ROCK_V2/AGENTS.md` and the current plan at `F:/fo4dev/PROJECT_ROCK_V2/Docs/Reload_Maker_V2/docs/IMPLEMENTATION_PLAN.md` before modifying this application. Do not use subagents or question widgets.

This is the independent Reload Maker V2 application. `StudioWindow` owns the native viewport/model directly. Keep the NifSkope application window, entry point, generic CLI and unrelated editor tools out of the build. The required native model, material, skinning, rendering and branch-transfer code remains a read-only external dependency; patch only its isolated build staging directory. Preserve dependency licenses and provenance.

The original Reload Maker, Offset Maker, source mod packages, game Data, MO2 profiles, ROCK/PAPER/FRIK and SDK checkouts are references or inputs. Do not modify or deploy them under ordinary V2 editor work. Follow the workspace's approved BSA/NIF tooling for asset preparation and inspection.

Build with `cmake --preset custom-fast` and `cmake --build --preset custom-fast`, capped at two compiler processes. Launch `ReloadMakerV2.cmd`. All application settings, caches, temporary output, authoring libraries, screenshots and staged exports belong under this repository on F:. Do not reintroduce AppData or registry storage.

GUI and MCP share one controller. Preserve independent physical hands and distinguish source/native pose, finger preview, part-grip presentation, mechanism simulation and reload-driven grips. Keep drafts while changing workspaces; merely having a saved draft does not make it the active pose authority. Failed or cancelled source replacements must retain completed authoring data.

Hand-pose, part-grip, mechanism and reload-recipe JSON retain the existing domain contracts. Their saved `revision` fields are separate from MCP `documentRevision`. Validate imports, preserve original transform values, and reject conflicting IDs. Exports stay in the local exports folder; they do not deploy themselves to the game.

Use the existing behavioral suites and the real MCP integration test. The integration fixture is the original Reload Maker's read-only Glock package. Put generated test records/evidence under ignored workspace/evidence folders. Inspect the actual native image and desktop workflow before claiming visual success. Do not claim full Havok behavior emulation, universal weapon support or runtime-game verification from offline tests.

Keep the current Git branch, commit validated task-owned code, and leave Markdown uncommitted unless its commit is explicitly requested. Update the central implementation plan with concrete evidence and unresolved limitations.
