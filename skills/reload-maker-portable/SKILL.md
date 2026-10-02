---
name: reload-maker-portable
description: Author and inspect physical Fallout 4 reloads using the installed Reload Maker application's MCP tools, or build its finished magazine-object packs. Use for source selection, reload motion, hands, operating points, sound and collision authoring in Maker. Use the repository development workflow when changing native game plugins.
---

# Reload Maker

Use the user's installed Maker and selected workspace. An external AI client drives the same controller as the manual UI. The application owns project state and validation; this skill does not supply another implementation of its file formats.

## Connect and establish authority

Start the optional stdio adapter with `ReloadMakerMcp.cmd --workspace "<workspace>"`. The bundled runtime is sufficient. Native format adapters use the same launcher with `--adapter nif`, `--adapter archive` or `--adapter plugin` when that capability is required. Do not locate a developer's personal tools or install separate MCP servers.

Read `get_state` and `get_installation`. Obtain the current revision before a mutation. The endpoint belongs to one workspace and one process session; reconnecting MO2 changes the session. After a stale-session error, read state and re-evaluate the selection before editing. Never replay an old edit blindly.

MO2 metadata alone does not prove access to its effective files. `connect_profile` saves the open project and requests a verified MO2 launch. Wait for the replacement session and completed loading. Package mode and an attached MO2 library are different source contexts. Do not claim that an offline package contains every winning mod override.

## Author the source behavior

Inspect the selected weapon, attachment choices, graph actions and native scene before assigning roles. The weapon label is not evidence that its bolt, handle, barrel, sights or cartridges share an animation owner. Use the actual rendered part and its reference transform.

Keep three things distinct:

- Player input: finite straight or angled magazine guide, or the permitted bolt-pull path.
- Visible mechanism: the full source motion, including rotations, barrel tilt, firing pin, follower parts and feeding cartridges.
- Hand presentation: magazine wrist placement plus fingers, or operating fingers with a live wrist.

Use source animation when it contains the required movement. A static firing bolt or handle requires an explicitly recorded derivation from another verified source clip. Do not quietly reuse a generic travel distance or simplify a full mechanism to one translated mesh.

For magazines, select the actual variant and physical hand, then use `configure_magazine` and a recognizable library pose. Move/rotate the hand around the authoritative magazine. Save the selected magazine's fit by replacement; use the weapon-default scope only when requested. Confirm all fifteen joints and the actual wrist-to-carrier placement. Do not confuse an attachment root with the animated magazine anchor.

For operating parts, authored grabs control fingers only on either permitted hand. Preserve the live wrist position and approach angle. Grab locations are picked surface points in the moving part's local frame; they follow the part at closed, intermediate and open positions. A saved wrist or mesh center never replaces a picked point.

Check full, empty and pre-feed magazine presentations independently. Exclude animation-only spare copies. Validate the magazine both in free carry and along the insertion rail, from multiple camera angles. Fingers should contact the surface; a small intersection is preferable to visible hovering.

Sound cues belong to physical events and directional crossings, not a continuously playing reload clip. Exercise holds, reversals, catch/release, dry fire and cancellation so a cue does not repeat every frame.

## Build and inspect

Save projects and authored records before switching sources. Use the tools' actual returned IDs, paths and revisions. Poll long operations only until they complete or fail, and preserve the returned job identity when checking a pack build.

The collision workshop's convex hulls are editable preparation data. A distributable world NIF needs validated Fallout 4 `bhkNPCollisionObject` / `bhkPhysicsSystem` output from the configured Creation Kit cooker. Missing Elric does not block pose work or building an object ESP from already cooked assets.

`pack_job` exposes `validate`, `build`, `state` and `cancel` through the same builder as the manual window. `BuildDiamondPacks.bat --workspace "<workspace>" --pack "<finished-pack>" --installation "<installation-folder>"` provides the standalone path. Use `--initialize` only for a new installation. Keep the allocation ledger so rebuilds have stable object IDs. Emit only the selected packs' records and assets. This development system makes a clean transition to generated packs; do not add old-save migration, old-magazine aliases or retired-world-object support.

Install the generated ESP and bindings as a pair when installation is authorized. Building does not enable MO2 mods. Do not replace original weapon ESPs or publish source assets as an implicit part of authoring.

## Current compatibility boundary

This skill accompanies an implementation in progress. Setup, the existing authoring commands and the object builder are available. The complete shareable reload-pack export/runtime migration must pass its own capability and integration checks before claiming that a newly built object ESP supplies playable reloads. Consult the installed application's tool responses and the requested project's current compatibility report.

Report separately what was source-derived, mathematically checked, visually inspected, built, and actually tested in the headset. An offline preview or successful ESP readback does not establish in-game acceptance.

## Complete authored-pack publication

For a prepared `reload-maker-pack-project`, use `pack_job(operation="inspect-project", project=..., expected_revision=...)` to inspect its weapon/configuration coverage, then `operation="export"` with a new local `output` folder. The manual equivalent is Export > Complete DIAMOND reload pack. The standalone equivalent is `ReloadMakerTools.cmd --workspace <workspace> packs export --project <pack.project.json> --output <new-folder>`.

The publisher freezes complete runtime behavior, hands, moving grab points, sounds, logical magazine objects and validated cooked models. It runs the bundled DIAMOND parser, not merely JSON/hash checks. Changed project inputs require review and refreshed references; never bypass this by silently updating hashes. Current testbed projects are organized by source ESP and weapon. Live configured reloads can now be saved into these projects through the shared configuration save operation below. Broader source compatibility and release qualification remain in progress.

The authored pack owns its finished NIFs. The generated companion contains the local ESP and paired bindings only, avoiding stale copied NIFs masking a later pack update in MO2. Install both the authored pack(s) and their matching generated companion when installation is authorized. A successful export is not a headset test.

## Source review and collision cooking

`authoring_roadmap` exposes the twelve-step guide, saved step selection, record-sources and check-sources. Source comparison uses relevant animation/model bytes, not only filenames or ESP lists. A changed provider label with identical content is acceptable. Changed or missing bytes require review; never erase working hand fits or refresh expected hashes merely to turn the report green.

The collision workshop exports enabled hulls in the selected magazine root with `configure_collisions(changes={"operation":"export-hulls", "path":...}, expected_revision=...)`. The profile must have a current source root. Existing saved source-local hulls are not final Havok data. `cook_world_object(job, output, expected_revision)` and `ReloadMakerTools.cmd --workspace <workspace> cook-object --job <job.json> --output <new.nif>` run the same native cooker. The frozen job contains `geometry` and `collision` path/SHA256 pairs and format `reload-maker-world-object-job`, version 1. Geometry must already be a prepared rigid full/empty magazine in the same root frame, without existing collision; do not select a whole receiver or source animated spare as a shortcut. The cooker preserves source inputs, validates finished physics and compares render geometry/transforms/material bindings. Inspect its receipt and test actual physics in game. Whole live-model extraction into that prepared input remains a distinct authoring step.

## Prepare geometry inside Maker

In the collision workshop select the actual magazine/part reference and the intended paused source frame. `capture_prepared_part(output, expected_revision)` freezes the native viewport's rigid surface attributes, material paths and part-local transforms, then creates a prepared NIF through the bundled native importer. The visible source must have complete UVs/normals/tangents, supported vertex counts and full detail. Unsupported deforming/incomplete geometry is rejected instead of flattened silently. Native readback checks counts, frame matrices, bounds and surface attributes. Continue with the same root-space hulls and the cooker.

Use the WORLD MODEL PIECES list, or `configure_collisions(changes={"operation":"piece", "index":..., "included":false}, expected_revision=...)`, to select the intended body and payload. Re-generate hulls after re-including pieces. Selections persist in collision profiles and apply to geometry capture and collision generation/export. Inspect names, source paths and material names from get_collision_state.sources. Timberwolf/L118 source containers include alternative projectiles and spare magazines: capturing every source piece is not proof of the correct full/empty presentation. Do not replace the existing accepted runtime meshes until the selected pieces and root frame are verified for that magazine. No automatic weapon-specific name filter is implied by the capture tool.


## Capture and save a live configured reload

Use `authoring_roadmap(operation="template", id=...)` to choose one of the five supported mechanisms for the weapon. Prepare and reset Operate. `export_authored_record(kind="runtime_configuration", ...)` captures the selected receiver/magazine/operating source, complete curves, both permitted magazine hand fits, finger-only operating poses, sounds, picked grab points and current source digests. Missing rifle grab points or hand assignments reject the capture; Glock may retain its unassigned slide acquisition. It does not invent a pickup location.

The manual FULL WORLD MODEL and EMPTY WORLD MODEL buttons call `capture_magazine_world(empty=..., expected_revision=...)`. This freezes the same pre-feed/empty state shown by Operate in the actual magazine carrier frame, excluding hands, receiver and parked spares. The collision workshop remains on that presentation. Review WORLD MODEL PIECES, especially overlapping projectile alternatives; select the intended materials before preparing and cooking. For the current Timberwolf/L118 FMJ source, the alternative 338copperprojectileinc.bgsm, 338brassprojectile.bgsm and 338armorprojectile.bgsm pieces are excluded. Do not generalize this into a filter for unrelated weapons or ammunition.

Then use `capture_prepared_part`, export enabled hulls, and `cook_world_object` as above. Keep each finished NIF beside its `.receipt.json`. Carrier-space cooking requires a matching preparation receipt; geometry and collision must share the exact weapon, magazine state, reference frame and selected piece list. A mismatch is an error, not something to repair by editing a digest.

`save_configured_reload(project, expected_revision, expected_project_sha256, ...)` is the same operation as Export > Save selected configuration to pack. A new project requires a namespace (`pack_id`) and both `full_model` and `empty_model`. Existing projects require their current SHA256; omit both models to retain the current assignment. Saving replaces the configuration identified by its receiver/magazine/operating records and replaces the shared magazine fit by magazine record. It preserves other configurations and explicit source/collection/blocked-mode rules. Poll `pack_job` until complete. Invalid hands, stale writers or mismatched assets preserve the previous project.

The standalone equivalent is `ReloadMakerTools.cmd --workspace <workspace> packs save-configuration --project <project.json> --capture <runtime-configuration.json> --expected <existing-project-sha256>`; for a new project use `--id <namespace> --full <full.nif> --empty <empty.nif>` instead of `--expected`. This performs the same native runtime validation before the atomic project save. Publish the saved project with the complete-pack exporter, then build the local object ESP from the chosen finished packs.


## Verify pack inputs before publishing

Inspect-project returns sourceRequirements for each weapon/configuration and sourceConflicts for unfinished mixed-version edits. Choose the loaded configuration, convert its resources to `{version: 1, resources: [{kind, path, sha256}]}` (kind is animation for HKX, model for NIF), and call `inspect_sources`. The equivalent manual action is Check loaded configuration sources in the complete-pack export window. Checks reopen the effective source view, detect newly added loose overrides and retain authored work. Package-mode success proves that package view only; a MO2 profile check requires the verified active VFS session.

Runtime requirements belong to individual configurations so a missing optional clip/model does not invalidate the other configured variants. Changed source bytes require review. Update draft configurations incrementally, but do not publish a weapon with contradictory versions of the same source. New logical world-object identities distinguish weapons even when they use the same magazine OMOD, because their hand fits/carrier frames can differ. Do not manually collapse those identities; the publisher deduplicates identical NIF bytes separately.

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

## Review and resume the authoring roadmap

Each roadmap step reports available, canReview, status, message and a fingerprint of its relevant authored inputs. Statuses are blocked, draft, ready, reviewed and needs-review. Use authoring_roadmap(operation="review", index=...) only after inspecting that step; unreview returns it to a draft review state. These marks do not bypass any source, native profile, cooked-model or publication validation.

Relevant changes invalidate dependent reviews. A guide edit leaves magazine hand fitting and collision reviews alone, while requiring sound/Operate review. Operating curve/progress changes require moving-point and sound review. Source or assembly changes affect dependent work. Camera, overlay and hull selection do not invalidate review. Collision generation, imports, piece/hull enablement and physical settings update the specific full/empty draft; both drafts must match the current source/presentation before the collision step can be reviewed. These are draft checks, not a claim of Havok cooking or headset acceptance. Save the project to retain progress, and explicitly check source changes before export.

New weapons require an explicit supported template before Operate; the twelve established testbeds retain their verified source defaults. Do not assign a pistol/slide template merely to suppress an unsupported family error.


## Explicit magazine references and authored movement

`get_magazine_reference` lists owner-qualified eligible reference frames. Save through `configure_magazine_reference` with the returned settings and anchorKey. The selected frame is magazine authority for wrist/finger fits; the native mount, full carrier pivot, or exact custom carrier-local connection determines the guide origin/orientation. Never select a wrist as the grab/connection authority. Changed references preserve saved fits but reject mismatched frames until reviewed. Check both physical hands and the detached carrier in native renders.

`get_magazine_presentation` lists saved movements authored for the exact full carrier. Set presentation motion to `manual` and movementId to a saved Move/Rotate definition. Its staged extraction is the visible path; insertion traverses it in reverse. The user's straight/angled guide remains independent. The complete body must clear the weapon at the endpoint; do not bypass the clearance error by removing geometry. Runtime capture freezes the sampled path and original movement definition, so DIAMOND does not need the creator's library to play it. When a source has no usable extraction, get_magazine_guide exposes an explicit manual draft after the operating roles are resolved. Choose and save its outward direction before configuring the visible magazine movement; geometry still determines complete clearance. For a pistol whose slide and magazine share one source model, select their independent native branches explicitly in Operating parts. The magazine subtree then owns visibility and its carrier provides the starting connection; do not select a receiver/operating ancestor as a magazine. Configure payload and guide separately, and verify detached geometry before authoring the hands.


Finished-pack builds now run the complete DIAMOND runtime catalog parser over an isolated Data view before selecting the output generation. Respect the bundled validator capacity; do not split one build into incompatible generated ESPs to bypass it. An object-only manifest is incomplete. `packs validate` checks frozen inputs; full catalog/ESP/bindings acceptance belongs to `packs build`. An unchanged or refreshed hash never substitutes for valid behavior, required source fingerprints and matching full/empty object assignments.


Editor state exposes suggestedProject under the originating ESP/weapon/source-context folder. Save projects there unless the user supplies another workspace destination. Source contexts separate MO2 profiles and ordered mod/override combinations; changing source bytes still requires review. The reload library reports editor drafts separately from authored pack coverage. Switching weapons saves and resumes their independent projects. Do not combine base/overhaul authoring merely because both originate in the same ESP record.


## Source compatibility and separate editions

Use get_source_compatibility/configure_source_compatibility for the current weapon/source-context key. The rules cover an add-on marker that must be present or absent, blocked conversions and nonphysical magazine collection records. Bundled defaults preserve MK18 base/Empire, SREP unsupported conversions and Thanatos collection behavior. Custom edits are explicit and apply to every configuration of this weapon when saved into its pack; they do not change curves or hands. Reset restores the current source defaults.

Default pack projects now live under ESP/weapon/source-context folders, independently of editor projects. Their default namespaces also distinguish contexts. A default compatibility policy must match an existing pack: do not silently mix base and overhaul editions. A deliberate change requires reviewing and saving the compatibility rules explicitly. A policy-only change invalidates publication review without invalidating unrelated hand/collision work.

## Local installation and rollback

A successful pack_job build returns installation.report, installation.instructions and installation.snapshot, also available through the builder's Open installation and rollback instructions button. The snapshot is an independently copied, hash-checked effective Data view of just the selected authored content and its generated ESP/bindings. Original weapon mods and DIAMOND.dll remain separate prerequisites. Use the selected authored folders plus generated companion for independent MO2 mods, or the complete snapshot for a manual local installation. Installation and enablement remain explicit user actions.

Keep the same installation ledger when rebuilding. Restore exact prior pack versions and their companion together to roll back installed files; do not rewind/delete the current allocation ledger. A previous snapshot is offered only when its ESP, bindings, manifests and content hashes verify. Updating a pack must not mutate that snapshot. No old DIAMOND.esp saved-object aliases are provided.
