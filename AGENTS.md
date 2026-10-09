# Reload Maker V2

Read `F:/fo4dev/PROJECT_ROCK_V2/AGENTS.md` and the current reload skill instructions before modifying this application: `skills/reload-maker-portable/SKILL.md` and `F:/fo4dev/misc/reload-maker-diamond-skill/build-diamond-reloads/SKILL.md`. Owner 2026-10-06: durable reload knowledge belongs in these skills; former project notes moved to `F:/no` are historical input, not a missing required plan. Do not use subagents or question widgets.

This is the independent Reload Maker V2 application. `StudioWindow` owns the native viewport/model directly. Keep the NifSkope application window, entry point, generic CLI and unrelated editor tools out of the build. The required native model, material, skinning, rendering and branch-transfer code remains a read-only external dependency; patch only its isolated build staging directory. Preserve dependency licenses and provenance.

The original Reload Maker, Offset Maker, source mod packages, game Data, MO2 profiles, ROCK/PAPER/FRIK and SDK checkouts are references or inputs. Do not modify or deploy them under ordinary V2 editor work. Follow the workspace's approved BSA/NIF tooling for asset preparation and inspection.

Build with `cmake --preset custom-fast` and `cmake --build --preset custom-fast`, capped at two compiler processes. Launch `ReloadMakerV2.cmd`. All application settings, caches, temporary output, authoring libraries, screenshots and staged exports belong under this repository on F:. Do not reintroduce AppData or registry storage.

GUI and MCP share one controller. Preserve independent physical hands and distinguish source/native pose, finger preview, part-grip presentation, mechanism simulation and reload-driven grips. Keep drafts while changing workspaces; merely having a saved draft does not make it the active pose authority. Failed or cancelled source replacements must retain completed authoring data.

Hand-pose, part-grip, mechanism and reload-recipe JSON retain the existing domain contracts. Their saved `revision` fields are separate from MCP `documentRevision`. Validate imports, preserve original transform values, and reject conflicting IDs. Archive exports use the selected local output; Export to MO2 delivers complete source-plugin groups to the selected named mod folder without changing activation or profile settings.

## Working with the owner (2026-10-05)

- **Questions.** Finish the approved work that does not depend on an answer, then stop and ask. Put each question in full, with a recommendation, in that final message. Questions posted in mid-work progress messages are not read or answered. Never start work that depends on an unanswered question.
- **Rules live here.** Standing rules and decisions go in this file or in `F:/fo4dev/PROJECT_ROCK_V2/Docs/Reload_Maker_V2/`, not in an agent's private memory.
- **Who authors weapons (owner, 2026-10-05).** Do not count on automatic detection to cover weapons well. A person authors mostly by hand, with an AI's help, or leaves it all to an AI; an AI works through MCP. Both the manual UI and MCP must be good and intuitive for their user. An AI should finish a weapon in a few minutes; a person takes much longer. MCP friction met while authoring (a missing command, an unclear refusal, a step that needs a workaround) is a defect to record and fix, not a cost to absorb.
- **Plans.** The rework plans and the backlog are in `F:/fo4dev/PROJECT_ROCK_V2/Docs/Reload_Maker_V2/docs/rework/` (`08_BACKLOG.md`). Lessons are in `Docs/Reload_Maker_V2/lessons/`.

## Game files and MO2 (owner, 2026-10-05)

- **Where masters come from.** A development source (package or testbed mode) resolves masters beside its plugin, then in the game's Data folder, then in `workspace/dependencies`.
- **MO2.** Its load order and profile apply only when Maker runs inside MO2. A package is one plugin, mod folder or source pack and may sit in `D:/FO4/mods`. Package mode reads only those files, Data and `workspace/dependencies`; it never searches MO2's other mods or its profile. Never write to MO2 or the game's Data folder.
- **Dev bench (owner, 2026-10-05).** Every weapon the owner gives for authoring goes on the dev bench: copy its mod folder from MO2 (read only) into `local/devbench/<mod folder>` and save the weapon's project from that copy; MO2 stays separate. The copies stay out of `workspace/`, which test workers mirror. The ten testbeds remain the automated corpus.
- **Data.** It holds the base game and DLCs. When a source needs another master, copy that master and its `<name> - *.ba2` archives from its MO2 mod into `workspace/dependencies`. That folder is git-ignored and mirrored to test workers. Ask before changing where Maker looks for game files.

## No hash verification (owner, 2026-10-05)

See the workspace rule in `F:/fo4dev/PROJECT_ROCK_V2/AGENTS.md`. Authoring records are keyed by weapon, receiver, operating part and magazine only; other parts (barrel, grip, sights, underbarrel, muzzle) never change which authoring applies.

## Local files

- **Scratch.** Scratch scripts, probes and captures go in `scratchbook/` in this repository (git-excluded). Never put downloads, caches or scratch files on C:.
- **Testbeds.** MK18 and MK18 Empire were removed from the testbeds (owner, 2026-10-05); the owner adds them back if wanted. Their projects stay on disk.
- **Saving elsewhere.** A suite that saves a project elsewhere re-points the testbed and last-project settings. It must reopen and re-save the original in `finally` (see the lessons).
- **The library is irreplaceable.** `workspace/library` holds every saved ESP project and is git-ignored, so nothing can bring a deleted file back. Never write code or run a tool that can delete or overwrite library files without first reading every delete in that path, and testing it on a temporary copy. A run must never remove library files that existed before it started. The editor harness (`tools/editor_session.py`) enforces this, and ctest `EditorHarnessRestore` checks it; never weaken either. Real saves go through the editor's Save; tests keep their own library changes out of it.

## Validation budget (owner, 2026-10-05)

Do not run the complete validation after every change. The complete validation is the golden capture/compare, every MCP suite and the SREP export diff.

- **Full gate:** only after a heavy logic change, such as export or pack output, the simulation and native-motion model, or source and master resolution. Also run it when the owner asks.
- **Any other change:** build with the capped preset, then run at most the one suite that covers it or a quick page capture for UI work. Then commit.
- **Track instead of retesting.** Each commit gets a row in `F:/fo4dev/PROJECT_ROCK_V2/Docs/Reload_Maker_V2/docs/rework/CHANGE_TRACKER.md`: what changed, which behavior changed (or "none"), the validation actually run, and what still needs testing. The next full gate clears the "needs testing" entries it covers.

Use the existing behavioral suites and the real MCP integration test. The integration fixture is the original Reload Maker's read-only Glock package. Put generated test records/evidence under ignored workspace/evidence folders. Inspect the actual native image and desktop workflow before claiming visual success. Do not claim full Havok behavior emulation, universal weapon support or runtime-game verification from offline tests.

Keep the current Git branch, commit validated task-owned code, and leave Markdown uncommitted unless its commit is explicitly requested. Update the central implementation plan with concrete evidence and unresolved limitations.

## Current roster validation override (owner, 2026-10-05)
For the requested groups 1 and 2 (roster A2 through A20), do not run any more all-weapons suites or golden captures. Check each weapon individually through the native Maker workflow and its actual pack. Golden comparisons are not evidence of a new weapon's behavior. This instruction supersedes the full-gate rule above for this task. Stop before Tier B. Keep outputs on F: and leave MO2 untouched.


## Testbed completion authority (owner, 2026-10-06)
Every weapon in the owned testbed roster must be fully authored, saved, functional in Reload Maker and functional in DIAMOND in game before it is treated as completed. A successful export, catalog parse, build, deployment or Maker-only demonstration does not complete a weapon. Track unverified game behavior explicitly until correlated runtime evidence establishes it. This applies to every owned testbed, including roster sections A and B, which are now both authorized. It does not impose testbed certification on consumer-authored weapons loaded through regular MO2. Preserve consumer projects and source activation. Validate owned weapons individually; do not run all-weapons behavior suites or golden captures.

## Weapon behavior and template authority (owner, 2026-10-06)
Treat each newly onboarded weapon as potentially requiring its own new template and behavior. Reuse an existing template only when this weapon's source, physical operation and individual Maker/DIAMOND checks prove that behavior is repeatable and appropriate. Prefer accurate variety over forcing a weapon into a convenient template. When the behavior differs, create the required template and implement its actual movement/state transitions in both Maker and DIAMOND; a renamed preset without the required behavior is insufficient. Ordered multi-step reloads, including belt-fed cover/box/belt/closing sequences, are explicitly permitted when the weapon requires them. Preserve full visual mechanism/hand motion while keeping controller gestures linear or gently curved unless the physical interaction requires a justified exception. Complete all steps, hand ownership, collision transitions, ammunition changes, sounds and persistence applicable to the implemented behavior before owned-testbed completion. Do not mark an unsupported mechanism as working by choosing the nearest available template. This authorizes necessary behavior work for the requested weapons; it does not silently expand the current A/B roster to unrelated weapons.

## Essential variations and shared receiver authoring (owner, 2026-10-06)
An owned testbed is not complete with only one working loadout: cover all essential magazine and receiver variations in scope. Receiver records using the same mesh/body must share compatible geometry authoring, operating/grab points, hand fits and movement configuration rather than failing because their record IDs differ. Configuring one such physical variation should make its same-model siblings compatible. Keep each receiver's automatic/semi-automatic firing, ammunition and genuinely distinct mechanism behavior correct; sharing mesh authoring does not erase those differences. Individually verify the sibling variations and essential magazine combinations before completion. If the same mesh has a proven behavior difference, implement that difference explicitly instead of rejecting it or substituting an incorrect default. Existing proven authoring takes precedence over creating duplicate poses/configurations for identical bodies.

The owner explicitly permits fully skipping the vanilla Submachine Gun if its inconsistent source/in-game behavior cannot be repaired reliably. Do not publish it partially or count it as completed; report the deliberate exclusion. Other A/B weapons remain in scope.

## Reload admission must follow moving parts (owner, 2026-10-06)
Apply this to both Maker and DIAMOND. A stock, sight, muzzle, skin or other nonessential accessory change must not invalidate a reload, remove its preset, invalidate saved poses, or require reauthoring. Compatibility follows the physical magazine and actual moving-part assembly. Different records using the same model and moving parts must be made compatible; preserve their ammunition and firing properties. Audit existing refusal gates instead of assuming they are justified. Never use source/part/configuration hashes, recipe fingerprints or whole-loadout identities as admission or stale-data gates. Content-addressed filenames/caches are allowed but never prove incompatibility or require confirmation. Keep actual live ownership, generation, missing/ambiguous node, incompatible physical mechanism and unsafe-pointer checks; explain their concrete failure rather than mislabeling all failures as an unsupported magazine. Missing optional blocklist/collection records must not disable a whole weapon.

## Ignore auxiliary launchers for regular reloads (owner, 2026-10-06)
Grenade launchers and auxiliary underbarrel weapons such as SREP's M203/M26 are excluded from required reload authoring. If present, preserve the host weapon's regular authored reload and grab points, treating it as the same regular weapon. Their presence or selector records must not block, discard or replace the regular reload. Do not require new launcher/underbarrel templates, captures or completion work. Do not modify other mods or MO2 activation to achieve this. Preserve the host's regular magazine/operating-part authority; auxiliary behavior is not the scope of this task. This supersedes the earlier SREP blocked-mod policy for these auxiliary attachments.

## Optional authoring, anchors and allowed gates
Make every weapon work with the least authoring. The behaviour preset is the only required choice; the user picks it because a wrong preset breaks the weapon. Everything else is an optional refinement with a fallback, and nothing may block a reload, capture, save or export because a refinement is absent.

- Authoring belongs to parts, never to receiver × magazine combinations. Magazine configuration (fit, guide, reference, presentation, models, collision) belongs to the magazine; moving-part configuration (operating setup, grab points, fingers) belongs to the receiver/operating assembly. A magazine used with another receiver keeps working. Captures stay expanded per combination internally, without user work.
- The first authored example of each kind is the weapon's anchor and fallback; there is no generic built-in pose. The first magazine fit for a hand serves every magazine (the other hand gets its mirror); the first configured receiver/operating part serves its siblings with the same or near-identical model, including grab points and fingers. The UI and MCP state name a reused anchor so the user can author that part individually if the result looks wrong.
- Allowed refusals are only: no magazine, a magazine or moving part with a genuinely different model, and memory/node/pointer/generation/file-integrity safety. Everything else warns and falls back: fire mode and ammunition uncertainty (the game's live weapon decides), missing fire clips (replay the reload stroke), changed magazine pieces or references (keep what still exists), cooked models shared by records using the same model, missing fingers or grab points (ROCK's hand pose, whole-part grab), one weapon's problem in a pack export (skip that weapon). Do not add a new gate outside the allowed list; propose a fallback instead.
- Joke and comical parts need no exclusion: they fall back to the anchors like any other part. Do not add blocked-mod policies for them.

The plan and status live in `F:/fo4dev/PROJECT_ROCK_V2/Docs/reload_maker_v2/docs/AnchorsAndFallbacksPlan.md`.

## AKs and AR15s pack expansion (owner, 2026-10-06)
The entire AKs and AR15s FO4 Edition pack is now authorized for reload authoring in Maker and DIAMOND, except PKM (`AK_AR15FO4.esp:005c93`). This supersedes the earlier three-weapon-only scope. Preserve the already authored AKMS, AS VAL and M16A2E3. Main and legitimate unique records are in scope, including All American and AK-104 Vojvoda. All essential magazines and receivers must work; physical variations require individual source review. M231's open-bolt source behavior requires its own actual implementation, not an AR15 closed-bolt substitution. Headset acceptance stays pending while the owner is away.

Owner stop point (2026-10-06): finish Colt Model 651, export/deploy the authored AK/AR checkpoint, then stop. Do not resume the remaining weapons without a new owner instruction. See the reload skill reference `references/aks-ar15-expansion.md`.

## Magazine rail rollback (owner, 2026-10-06)
Restore the earlier default magazine insertion behavior. Do not automatically move capture to the magazine tip/socket, halve controller travel, or tighten its capture radius. Keep the default Maker body-centre display/native connection and DIAMOND carrier-origin capture, while preserving explicit custom reference frames and authored guides. Preserve hand fits, complete visible motion, collision lifecycle, ammunition persistence and object IDs. See the reload skill reference `references/magazine-rails-and-admission.md` for existing-export handling and validation limits. This supersedes the earlier same-day tip/socket rail instruction.

## Source-plugin ESL export (owner, 2026-10-07)
New reload deliveries use one compact ESL-flagged ESP per originating weapon plugin, with generated magazine meshes under the ESL basename. One ZIP/7z or one configured MO2 mod contains the selected source groups. Manual UI exposes Export to MO2 and Export ZIP / 7z; CLI and MCP use the same complete builder. Do not deliver raw authoring packs as a finished mod or regenerate the monolithic DIAMOND_Generated.esp. Maintain independent stable IDs per source group, preserve previously published sibling weapons on incremental updates, and keep unrelated groups/profile activation untouched. Required weapon-model overrides retain their source-required paths and are declared in the pack project's modelOverrides; they must not be omitted from bundles. Existing monolithic content remains readable, without aliases or silent old-magazine migration. Follow the reload skill reference references/per-source-esl-export.md for schema, publishing, validation and compatibility details.


## ESL-flagged ESP correction (owner, 2026-10-07)
Generated source groups use .esp filenames with only the Small (ESL) flag, never the Master flag. This supersedes the earlier .esl/light-master format to preserve normal ESP load-order placement. Keep per-source grouping, compact local IDs, mesh basenames and both export destinations. Convert existing creator ledgers without reallocating local IDs; rebuild plugin names in bindings and manifests. MO2 delivery removes only the receipt-owned replaced .esl for each updated group. Never change profile enablement/load order or imply that changing a plugin filename remaps existing save references. The runtime continues reading existing .esl groups, while new exports require the sourcePluginLightESP validator capability.
