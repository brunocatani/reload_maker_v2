---
name: reload-maker-portable
description: Author and inspect physical Fallout 4 reloads using the installed Reload Maker application's MCP tools, or build its finished magazine-object packs. Use for source selection, reload motion, hands, operating points, sound and collision authoring in Maker. Use the repository development workflow when changing native game plugins.
---

# Reload Maker

Use the user's installed Maker and selected workspace. An external AI client drives the same controller as the manual UI. The application owns project state and validation; this skill does not supply another implementation of its file formats. Durable rules and technical references belong in the skill packages (owner, 2026-10-06). Prior notes intentionally moved to F:/no are historical input; do not recreate the old Docs tree. No hash-based input verification, invalidation or skip gates are permitted.

## Connect and establish authority

Start the optional stdio adapter with `ReloadMakerMcp.cmd --workspace "<workspace>"`. The bundled runtime is sufficient. Native format adapters use the same launcher with `--adapter nif`, `--adapter archive` or `--adapter plugin` when that capability is required. Do not locate a developer's personal tools or install separate MCP servers.

Read `get_state` and `get_installation`. Obtain the current revision before a mutation. The endpoint belongs to one workspace and one process session; reconnecting MO2 changes the session. After a stale-session error, read state and re-evaluate the selection before editing. Never replay an old edit blindly.

MO2 metadata alone does not prove access to its effective files. `connect_profile` saves the open project and requests a verified MO2 launch. Wait for the replacement session and completed loading. Package mode and an attached MO2 library are different source contexts. Do not claim that an offline package contains every winning mod override.

## Author the source behavior

Inspect the selected weapon, attachment choices, graph actions and native scene before assigning roles. The weapon label is not evidence that its bolt, handle, barrel, sights or cartridges share an animation owner. Use the actual rendered part and its reference transform.

Keep three things distinct:

- Physical controller input: a finite straight pull/push, possibly angled in weapon space. A gentle curve is the normal maximum; more complex gestures need a specific interaction requirement and recorded justification.
- Visible mechanism: the full required source/authored motion, including handle folding, rotations, barrel tilt, firing pin, follower parts and feeding cartridges.
- Visible hand: keep the holding hand coherently attached/posed on the animated part. Its rendered motion can be curved while the physical controller moves straight; retain the existing magazine-fit and operating-finger assignment policies.

**Owner clarification, 2026-10-06:** the restriction is on the controller's trajectory, not the visible hand or weapon animation. Controller displacement drives mechanism progress; it must not require tracing a sampled HKX hand/contact curve. Preserve the full visual movement at that progress. The G36 image is a rejected controller-input path, not permission to remove its folding/rotating animation. Ordinary pulls must not require sharp hooks, zigzags, reversals or exact source wrist rotations. A special gesture is an exception only when the interaction actually requires it, not merely because the source animation contains it. Check the editor's input guide and the game's actual mapping separately; neither a valid curve nor a successful preview proves comfortable VR control.

Use source animation for visual movement when it contains what is required. A static firing bolt or handle needs an explicit measured derivation or authored motion. Do not quietly reuse a generic travel distance or simplify the complete visible mechanism into one translated mesh.

### AR-15 charging handles: required M16-style adaptation

For every AR-15-type weapon being authored, provide an independently operable charging handle wherever the geometry permits it. Retain an already-correct independent handle. When movement is missing or the handle is grouped into the receiver/bolt, use the M16-style repair: inspect the real handle geometry, split/reparent existing separable components without changing their detail/materials, and drive the independent part from that weapon's measured mechanical stroke. Put required model changes in a local override and keep source mods unchanged. Empty helper bones, a shared NIF and missing animation do not exempt the weapon. Genuine exceptions are absent handle geometry or geometry that cannot be separated into a usable part without destroying it or inventing a replacement; record the inspected limitation. An unavailable authoring/export operation is a tool deficiency to resolve/report, not physical impossibility.

Keep AR nonreciprocating handle motion independent of firing-bolt cycling and empty catch/release. Pick the actual handle surface, give controller input a straight or gently curved guide, and retain full visual motion and contact for the part and visible hand. Carry the source binding, model override, movement and grab points together when publishing. Reuse the M16 repair method, never its particular node names, mesh-component indices or travel distance. This rule applies to existing AR configurations when revisited as well as new ones; recording it does not mean their packs have already been repaired.

For magazines, select the actual variant and physical hand, then use `configure_magazine` and a recognizable library pose. Move/rotate the hand around the authoritative magazine. Save the selected magazine's fit by replacement; use the weapon-default scope only when requested. Confirm all fifteen joints and the actual wrist-to-carrier placement. Do not confuse an attachment root with the animated magazine anchor.

For operating parts, authored grabs control fingers only on either permitted hand. Preserve the live wrist position and approach angle. Grab locations are picked surface points in the moving part's local frame; they follow the part at closed, intermediate and open positions. A saved wrist or mesh center never replaces a picked point.

For controller-input inspection, read `get_simulation_state().controllerInput.strokeInWeapon` and use `simulate_weapon(operation="controller_pull", position=[x,y,z])` with weapon-local displacement since acquisition. Release before starting a new acquisition. The old `pull(progress=...)` directly sets normalized visual progress and does not verify controller usability. Compare real rendered part motion and full endpoints separately.

Check full, empty and pre-feed magazine presentations independently. Exclude animation-only spare copies. Validate the magazine both in free carry and along the insertion rail, from multiple camera angles. Fingers should contact the surface; a small intersection is preferable to visible hovering.

Sound cues belong to physical events and directional crossings, not a continuously playing reload clip. Exercise holds, reversals, catch/release, dry fire and cancellation so a cue does not repeat every frame.

## Build and inspect

Save projects and authored records before switching sources. Use the tools' actual returned IDs, paths and revisions. Poll long operations only until they complete or fail, and preserve the returned job identity when checking a pack build.

The collision workshop's convex hulls are editable preparation data. A distributable world NIF needs validated Fallout 4 `bhkNPCollisionObject` / `bhkPhysicsSystem` output from the configured Creation Kit cooker. Missing Elric does not block pose work or building source-plugin ESLs from already cooked assets.

Complete export now creates one ESL-flagged ESP per source plugin, with matching magazine-mesh folders. The two manual actions are **Export to MO2** and **Export ZIP / 7z**. Keep the creator allocation ledger so each group's compact IDs remain stable. Updating one source group retains its previously published sibling weapons; another source group's files and MO2 activation remain unchanged.

`pack_job(operation="build", packs=[saved-project JSON files or finished-pack folders], installation=..., initialize=..., archive=... or mo2=..., expected_revision=...)` uses the same builder. `operation="export"` accepts one `project` with `archive` or `mo2`. `open`/`close` show or close the export window. Poll the returned job ID with `state`. CLI: `packs export --project <project> --archive <file.zip>` or `packs build --pack <input> --installation <ledger> --archive <file.zip>`; replace `--archive` with `--mo2 <named-mod-folder>` for direct installation. Internal `freeze` emits authoring interchange only.

Keep ESL-flagged ESPs, their per-group object bindings, behavior and required models together. Magazine models are under `Meshes/<ESL basename>/`. Source weapon overrides retain required native paths and are declared explicitly in pack-project `modelOverrides`. Do not copy original source mods or change MO2 activation/load order. VR needs an ESL loader such as Daytripper.

## Current compatibility boundary

Complete exports require a DIAMOND runtime and bundled validator supporting source-plugin ESLs (`sourcePluginESL` capability). Groups activate from the game's loaded-plugin list at GameDataReady. An enabled source ESL supersedes legacy monolithic content for the same weapon. Legacy spawned magazine references are not remapped into new ESL identities. Keep the creator's saved export identity workspace for subsequent updates.

Report separately what was source-derived, mathematically checked, visually inspected, built, and actually tested in the headset. An offline preview or successful plugin readback does not establish in-game acceptance.

## Complete authored-pack publication

For a prepared `reload-maker-pack-project`, inspect coverage with `pack_job(operation="inspect-project", project=..., expected_revision=...)`, then export the installable bundle using `operation="export", project=..., archive=... or mo2=...`. Manual: MENU > Export > Export current reload, then Export to MO2 or Export ZIP / 7z. The exporter freezes authored inputs internally, groups them by source plugin and builds the ESLs; the user does not perform a second ESP-generation step.

The publisher freezes complete runtime behavior, hands, moving grab points, sounds, logical magazine objects and validated cooked models. It validates them through DIAMOND's native runtime parser. Current testbed projects are organized by source plugin and weapon. Live configured reloads are saved into these projects through the shared configuration save operation below. Broader source compatibility and release qualification remain in progress.

The complete delivered mod contains all selected ESL-flagged ESPs, bindings, authored behavior and finished assets together. Do not require a second companion installation. Updating one group preserves sibling weapons and other installed groups; the ledger remembers each group's latest generation even after intervening exports of other source plugins. A successful export is not a headset test.

## Source review and collision cooking

Use `checklist_state` and the loaded source identities to inspect the current configuration. The earlier twelve-step authoring_roadmap/fingerprint workflow is retired. Inspect actual missing parts, changed behavior and source availability; never invalidate saved work or demand reconfirmation because a hash differs.

The collision workshop exports enabled hulls in the selected magazine root with `configure_collisions(changes={"operation":"export-hulls", "path":...}, expected_revision=...)`. The profile must have a current source root. Existing saved source-local hulls are not final Havok data. `cook_world_object(job, output, expected_revision)` and `ReloadMakerTools.cmd --workspace <workspace> cook-object --job <job.json> --output <new.nif>` run the same native cooker. The frozen job contains `geometry` and `collision` path/SHA256 pairs and format `reload-maker-world-object-job`, version 1. Geometry must already be a prepared rigid full/empty magazine in the same root frame, without existing collision; do not select a whole receiver or source animated spare as a shortcut. The cooker preserves source inputs, validates finished physics and compares render geometry/transforms/material bindings. Inspect its receipt and test actual physics in game. Whole live-model extraction into that prepared input remains a distinct authoring step.

## Prepare geometry inside Maker

Reload builder > Magazines (workspace 7) lists only the captured loose magazine world object, the only capture a pack accepts; the whole weapon and installed parts are in the separate Weapon collision tool (MENU, workspace 9), which nothing consumes yet. In the collision workshop select the actual magazine/part reference and the intended paused source frame. `capture_prepared_part(output, expected_revision)` freezes the native viewport's rigid surface attributes, material paths and part-local transforms, then creates a prepared NIF through the bundled native importer. The visible source must have complete UVs/normals/tangents, supported vertex counts and full detail. Unsupported deforming/incomplete geometry is rejected instead of flattened silently. Native readback checks counts, frame matrices, bounds and surface attributes. Continue with the same root-space hulls and the cooker.

Use the WORLD MODEL PIECES list, or `configure_collisions(changes={"operation":"piece", "index":..., "included":false}, expected_revision=...)`, to select the intended body and payload. Re-generate hulls after re-including pieces. Selections persist in collision profiles and apply to geometry capture and collision generation/export. Inspect names, source paths and material names from get_collision_state.sources. Timberwolf/L118 source containers include alternative projectiles and spare magazines: capturing every source piece is not proof of the correct full/empty presentation. Do not replace the existing accepted runtime meshes until the selected pieces and root frame are verified for that magazine. No automatic weapon-specific name filter is implied by the capture tool.


## Capture and save a live configured reload

Use `checklist(operation="preset", id=...)` with an ID from `checklist_state.presets`. The bundled presets may share a mechanism; retain the exact selected ID. Prepare and reset Operate. `export_authored_record(kind="runtime_configuration", ...)` captures the selected receiver/magazine/operating source, complete curves, both permitted magazine hand fits, finger-only operating poses, sounds, picked grab points and current source digests. Missing rifle grab points or hand assignments reject the capture; Glock may retain its unassigned slide acquisition. It does not invent a pickup location.

The manual FULL WORLD MODEL and EMPTY WORLD MODEL buttons call `capture_magazine_world(empty=..., expected_revision=...)`. This freezes the same pre-feed/empty state shown by Operate in the actual magazine carrier frame, excluding hands, receiver and parked spares. The collision workshop remains on that presentation. Review WORLD MODEL PIECES, especially overlapping projectile alternatives; select the intended materials before preparing and cooking. For the current Timberwolf/L118 FMJ source, the alternative 338copperprojectileinc.bgsm, 338brassprojectile.bgsm and 338armorprojectile.bgsm pieces are excluded. Do not generalize this into a filter for unrelated weapons or ammunition.

Then use `capture_prepared_part`, export enabled hulls, and `cook_world_object` as above. Keep each finished NIF beside its `.receipt.json`. Carrier-space cooking requires a matching preparation receipt; geometry and collision must share the exact weapon, magazine state, reference frame and selected piece list. A mismatch is an error, not something to repair by editing a digest.

`save_configured_reload(project, expected_revision, pack_id="", full_model="", empty_model="")` is the same operation as Export > Save selected configuration to pack. A new project requires a namespace (`pack_id`) and both `full_model` and `empty_model`. For existing projects, omit both model paths to retain their assignments. No expected hash is required. Saving replaces the configuration identified by its receiver/magazine/operating records and replaces the shared magazine fit by magazine record. It preserves other configurations and explicit source/collection/blocked-mode rules. Poll `pack_job` until complete. Invalid hands, stale writers or mismatched assets preserve the previous project.

The standalone equivalent is `ReloadMakerTools.cmd --workspace <workspace> packs save-configuration --project <project.json> --capture <runtime-configuration.json>`; for a new project use `--id <namespace> --full <full.nif> --empty <empty.nif>` when creating a new project. This performs the same native runtime validation before the atomic project save. Publish the saved project with the complete-pack exporter, using the complete MO2 or ZIP / 7z action.


## Verify pack inputs before publishing

Inspect-project reports the prepared weapon/configuration coverage. Validate actual source identities, complete curves, hands, point sets, native profiles and full/empty model assignments. Update a changed configuration in its existing pack project; do not rebuild unrelated configurations or assets. Models/poses for different source editions remain separate. New logical world-object identities distinguish different weapon/fit owners, even when they reuse an OMOD. Keep the installation ledger for stable IDs. Source hashes or recipe IDs are not validity gates.


## Operating roles and source movements

Use get_operating_setup before configure_operating_setup. Its settings are scoped to the current recipeId. Select the actual operating, bolt and magazine carrier tracks and complete follower list. Source modes are preset (verified current behavior), reload (current capture), clip (explicit native animation and baseline), and manual (existing named part-mechanism records). Open/closed samples may use either source direction; automatic detection uses -1 for both. Manual spring movement is a closed-to-open path; a manual bolt needs the complete opening-and-closing sequence. Linked rigid followers and independent movements must not control the same part. For a manual bolt, ejectSeconds=-1 selects full opening. Do not create fake source annotations.

Save and preview, then test the stroke, release/catch, firing and ejection. Check source evidence, save/reopen, and export runtime_configuration. Exports preserve native hierarchy separately from model-relative source paths. A changed recipe requires explicit review rather than silently applying old track indices. Restore source defaults removes only that assembly's operating override.

## Magazine input direction

Use get_magazine_guide and configure_magazine_guide to inspect/save/reset the selected assembly's straight input path. Start from returned settings, retaining recipeId. outwardInConnection is a unit vector pointing away from the seated magazine in its connection-local axes. travel, captureRadius and releaseMargin use game units. A zero direction or stale recipe is rejected. The manual equivalent is Operate > Configure insertion guide, also available from roadmap step seven.

Changing the guide retains the whole visible magazine movement, including an original straight path. It does not rotate or translate the authored insertion animation. Test entry at intermediate positions, pull-out and complete seating. A guide edit must not change either magazine hand fit or the weapon's independent operating curves. Restore source guide removes only this assembly's override.

## Magazine body and payload authoring

Use get_magazine_presentation and configure_magazine_presentation from the loaded Operate configuration. The full carrier is selected in Operating parts; this panel chooses the empty native body (or the same carrier for a shared body), source movement mode and payload samples. Native insertion keeps rotation from entrySeconds through the seated frame. Sampled payload uses explicit pre-feed full and empty follower frames. This is separate from editing the magazine hand and from configuring the straight input guide.

Review every returned native piece path. Roles are body, cartridge (hide when empty), and spare (exclude from physical presentation). Native paths must remain unique; parts outside the full/empty carriers must remain spares. Separate carriers cannot be parent/child branches. Save rolls back if it leaves no body or an invalid source presentation. Preview both full and empty states, recapture and cook both world NIFs, then save the configured reload. Cook receipts include the presentation settings; export refuses reuse of models prepared with a different setting. Inside-carrier exclusions are also exported to DIAMOND's native extraction preview, with source visibility restored when it releases control.

## Authored workspace coverage

Reload library groups loaded weapons and authored packs by originating ESP, showing the current winning weapon record and saved configuration/magazine counts. get_pack_coverage(refresh=True) starts the shared inventory job; poll pack_job, then call get_pack_coverage() for the result and source weapon identities. The standalone equivalent is ReloadMakerTools.cmd --workspace <workspace> packs inventory, with repeated --project arguments for projects outside workspace/packs.

Coverage scans default pack.project.json files and remembered custom project paths. Damaged inputs remain visible as needs-repair drafts where their weapon identity can still be read. Different projects claiming the same weapon remain independent; never add their counts as proof that all variants are covered. These are authoring counts. Current source compatibility and game enablement require their own checks. Refresh after saves or source/project changes; the library does not poll folders in the background.

## Review and resume saved authoring

`checklist_state` exposes detected/confirmed/needs-you/not-applicable items. Confirmations remain until explicitly removed; save the project to retain them. `presetAssigned` distinguishes a chosen preset from a proposal. New weapons need an assigned supported preset before Operate; do not choose a convenient wrong family to bypass unsupported behavior. There were ten bundled preset IDs on 6 October 2026; read the current catalog instead of hardcoding that count.

Editor Save preserves the author's project. Save selected configuration updates the pack project and shared point/fit records. Complete pack export produces deployable files. For a grab-point-only change, save the points/project, enter Operate, save the selected configuration to the existing pack with model fields blank, then export the complete pack. An authorized install replaces that pack's F4SE files; no collision recook or DLL rebuild is needed. Point sets apply across configurations sharing the same operating-part model/path. Changing the weapon's template requires all its existing configurations to be saved together.

The browser and MCP reopen authored per-weapon/source-context projects. Save As no longer redirects a default testbed; failed loads retain the prior Save target; blank drafts cannot overwrite an authored preset. A stale header label alone is not evidence that a preset was lost: inspect the actual assignment. A zero-success configuration save must report failure.

## Explicit magazine references and authored movement

`get_magazine_reference` lists owner-qualified eligible reference frames. Save through `configure_magazine_reference` with the returned settings and anchorKey. The selected frame is magazine authority for wrist/finger fits; the native mount, full carrier pivot, or exact custom carrier-local connection determines the guide origin/orientation. Never select a wrist as the grab/connection authority. Changed references preserve saved fits but reject mismatched frames until reviewed. Check both physical hands and the detached carrier in native renders.

`get_magazine_presentation` lists saved movements authored for the exact full carrier. Set presentation motion to `manual` and movementId to a saved Move/Rotate definition. Its staged extraction is the visible path; insertion traverses it in reverse. The user's straight/angled guide remains independent. The complete body must clear the weapon at the endpoint; do not bypass the clearance error by removing geometry. Runtime capture freezes the sampled path and original movement definition, so DIAMOND does not need the creator's library to play it. When a source has no usable extraction, get_magazine_guide exposes an explicit manual draft after the operating roles are resolved. Choose and save its outward direction before configuring the visible magazine movement; geometry still determines complete clearance. For a pistol whose slide and magazine share one source model, select their independent native branches explicitly in Operating parts. The magazine subtree then owns visibility and its carrier provides the starting connection; do not select a receiver/operating ancestor as a magazine. Configure payload and guide separately, and verify detached geometry before authoring the hands.


Finished-pack builds now run the complete DIAMOND runtime catalog parser over an isolated Data view before selecting the output generation. Respect the bundled validator capacity; per-source ESL grouping does not bypass runtime capacity. An object-only manifest is incomplete. `packs validate` checks frozen inputs; full catalog/ESL/bindings acceptance belongs to `packs build`. An unchanged or refreshed hash never substitutes for valid behavior, readable source identities and matching full/empty object assignments.


Editor state exposes suggestedProject under the originating ESP/weapon/source-context folder. Save projects there unless the user supplies another workspace destination. Source contexts separate MO2 profiles and ordered mod/override combinations; changing source bytes still requires review. The reload library reports editor drafts separately from authored pack coverage. Switching weapons saves and resumes their independent projects. Do not combine base/overhaul authoring merely because both originate in the same ESP record.


## Source compatibility and separate editions

Use get_source_compatibility/configure_source_compatibility for the current weapon/source-context key. The rules cover an add-on marker that must be present or absent, blocked conversions and nonphysical magazine collection records. Bundled defaults distinguish MK18 base/Empire and nonphysical magazine collections. Auxiliary SREP M203/M26 selectors do not block the host reload; regular reload authoring ignores their secondary ammunition and animation overrides. Custom edits are explicit and apply to every configuration of this weapon when saved into its pack; they do not change curves or hands. Reset restores the current source defaults.

Default pack projects now live under ESP/weapon/source-context folders, independently of editor projects. Their default namespaces also distinguish contexts. A default compatibility policy must match an existing pack: do not silently mix base and overhaul editions. A deliberate change requires reviewing and saving the compatibility rules explicitly. A policy-only change invalidates publication review without invalidating unrelated hand/collision work.

## Output and activation

Finished-pack builds return the generated output folder and native catalog counts. Keep one allocation ledger per installation so IDs remain stable; failed or cancelled builds do not replace the last successful output. Authored packs and the source ESL-flagged ESPs and their bindings are ordinary mod files. The user controls MO2 activation and load order. Do not add cutover/rollback plans or edit profiles as part of building or deploying folders.


For repository integration, the installed build-diamond-reloads skill maintains the owner roster, observed future-family evidence, M16 geometry-adaptation method, project/pack workflow and runtime ownership. Keep detailed implementation conclusions there and portable authoring commands here. Do not use the archived F:/no notes as a second active instruction set.

## Owned testbed completion requirement (owner, 2026-10-06)
Every owned testbed weapon must be fully authored, saved, functional in Reload Maker, and functional in DIAMOND in game before it is marked completed. This applies to all owned testbeds and the authorized A and B roster. Do not substitute a pack export, parser acceptance, build, deployment or editor-only test for runtime acceptance. Record each unverified behavior honestly and continue repairs; require correlated runtime evidence for game claims. Consumer-authored weapons opened through ordinary MO2 are outside this owned-testbed completion requirement. Do not change their authoring or activation to satisfy testbed coverage. Validate weapons individually, never through the owner's prohibited all-weapons/golden suites.

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
The behaviour preset is the only required choice; confirm it with the user. Every other setting is an optional refinement with a fallback, so do not stop a weapon because a refinement is missing.

- Author parts, not combinations. Magazine settings (fit, guide, reference, presentation, models, collision) belong to the magazine; operating setup, grab points and fingers belong to the receiver/operating assembly. A magazine works with every receiver.
- The first authored example of each kind is the anchor: the first magazine fit for a hand serves every magazine (the other hand receives its mirror), and the first configured receiver/operating part serves same-model siblings. `checklist_state.planSources`, `operating_setup_state.source` and `grab_locations_state.own` say whether a value is the part's own, a legacy combination, the anchor or a sibling. Author a part individually only when its reused anchor looks wrong.
- Fastest path for a new weapon: assign the preset, author one magazine fit, configure one receiver/operating part, save. Refine other parts only where the result is wrong.
- Allowed refusals are only: no magazine, a genuinely different magazine or moving-part model, and memory/node/file safety. Ammunition or fire-mode uncertainty, missing fire clips, changed magazine pieces, missing fingers or grab points, and one weapon's export problem are warnings with fallbacks. Never add a blocked-mod policy for joke or comical parts; they use the anchors too.

## AKs and AR15s pack expansion (owner, 2026-10-06)
The entire AKs and AR15s FO4 Edition pack is now authorized for reload authoring in Maker and DIAMOND, except PKM (`AK_AR15FO4.esp:005c93`). This supersedes the earlier three-weapon-only scope. Preserve the already authored AKMS, AS VAL and M16A2E3. Main and legitimate unique records are in scope, including All American and AK-104 Vojvoda. All essential magazines and receivers must work; physical variations require individual source review. M231's open-bolt source behavior requires its own actual implementation, not an AR15 closed-bolt substitution. Headset acceptance stays pending while the owner is away.

## Magazine rail rollback (owner, 2026-10-06)
Restore the earlier default magazine insertion behavior. Do not automatically move capture to the magazine tip/socket, halve controller travel, or tighten its capture radius. Keep the default Maker body-centre display/native connection and DIAMOND carrier-origin capture, while preserving explicit custom reference frames and authored guides. Preserve hand fits, complete visible motion, collision lifecycle, ammunition persistence and object IDs. See the reload skill reference `references/magazine-rails-and-admission.md` for existing-export handling and validation limits. This supersedes the earlier same-day tip/socket rail instruction.

Current detailed developer/export authority: `F:/fo4dev/misc/reload-maker-diamond-skill/build-diamond-reloads/references/per-source-esl-export.md` (owner, 7 October 2026).


## ESL-flagged ESP correction (owner, 2026-10-07)
Generated source groups use .esp filenames with only the Small (ESL) flag, never the Master flag. This supersedes the earlier .esl/light-master format to preserve normal ESP load-order placement. Keep per-source grouping, compact local IDs, mesh basenames and both export destinations. Convert existing creator ledgers without reallocating local IDs; rebuild plugin names in bindings and manifests. MO2 delivery removes only the receipt-owned replaced .esl for each updated group. Never change profile enablement/load order or imply that changing a plugin filename remaps existing save references. The runtime continues reading existing .esl groups, while new exports require the sourcePluginLightESP validator capability.
