"""Local installation instructions and hash-qualified rollback snapshots."""
from product_packs import PLUGIN, digest, read, require, within, write

BINDINGS = 'F4SE/Plugins/DIAMOND/Generated/objects.json'


def check_snapshot(stage):
    """A previous output is usable only with the exact complete authored content."""
    view = stage/'validation-Data'
    try:
        receipt = read(stage/'receipt.json')
        require(digest(view/PLUGIN) == receipt['pluginSha256'], 'Snapshot plugin changed')
        require(digest(view/BINDINGS) == receipt['bindingsSha256'], 'Snapshot bindings changed')
        bindings = read(view/BINDINGS)
        for pack, expected in bindings['packs'].items():
            manifest = within(view, f'F4SE/Plugins/DIAMOND/Packs/{pack}/manifest.json')
            require(digest(manifest) == expected, f'Snapshot manifest changed: {pack}')
            for name, checksum in read(manifest)['files'].items():
                require(digest(within(view, name)) == checksum, f'Snapshot content changed: {name}')
        return dict(verified=True, data=str(view), packs=sorted(bindings['packs']),
                    generation=receipt['generation'])
    except (OSError, ValueError, KeyError, TypeError) as error:
        return dict(verified=False, data=str(view), reason=str(error))


def installation_report(stage, packs, receipt, previous):
    snapshot = check_snapshot(stage)
    require(snapshot['verified'], 'Installation snapshot is incomplete: '+snapshot.get('reason', ''))
    selected = []
    for pack in sorted(packs, key=lambda p: p['manifest']['id']):
        manifest = pack['manifest']
        selected.append(dict(id=manifest['id'], name=manifest.get('name', manifest['id']),
            revision=manifest['revision'], source=str(pack['root']), manifestSha256=pack['manifestSha256'],
            requirements=manifest.get('requirements', {})))
    rollback = check_snapshot(previous) if previous else None
    old_ids = set(rollback.get('packs', [])) if rollback and rollback['verified'] else set()
    report = dict(version=1, generation=receipt['generation'], generatedPlugin=PLUGIN,
        companion=str(stage/'output'), snapshot=snapshot, packs=selected, rollback=rollback,
        removedPacks=sorted(old_ids-set(snapshot['packs'])), gameFilesChanged=False)
    write(stage/'installation-report.json', report)
    lines = ['# Install this reload build', '',
        'This build has not changed your game, MO2 profile or original weapon plugins.', '',
        '## Mod Organizer 2', '',
        '1. Keep DIAMOND and its regular ROCK/PAPER providers installed. Keep the source weapon and animation mods required by each pack.',
        '2. Install each authored pack below as its own mod. Place its F4SE and Meshes folders at the mod root; update an existing pack by replacing its previous version.',
        '3. Install this build\'s `output` folder as one generated-companion mod. Replace the previous companion completely, keeping its ESP and bindings together.',
        f'4. Enable the authored packs, the companion and `{PLUGIN}` in the intended profile. Disable packs removed from this selection. Keep source weapon plugins enabled in their normal order.',
        '5. Restart the game. Changed or missing source files can disable the affected reload configuration; the runtime log identifies it.', '',
        '## Selected authored packs', '']
    for pack in selected:
        lines += [f'- **{pack["name"]}** (`{pack["id"]}`, revision {pack["revision"]}): `{pack["source"]}`']
        plugins = pack['requirements'].get('plugins', [])
        if plugins:
            lines += ['  Source plugins: '+', '.join('`'+str(p)+'`' for p in plugins)+'.']
    if report['removedPacks']:
        lines += ['', 'Remove these previous authored pack versions from the active profile: '+', '.join(report['removedPacks'])+'.']
    lines += ['', '## Manual installation', '',
        'The `validation-Data` folder beside this report is a complete, independently copied and validated snapshot of the selected reload packs plus generated ESP/bindings. Its contents belong directly inside game Data; it does not include DIAMOND.dll or the source weapon mods.',
        'Before replacing a manual installation, back up the exact files it will replace and keep its prior installation report. Remove only files owned by the previous reload build that are absent from this one; never erase a whole game folder. Enable the generated ESP through the game\'s normal plugin configuration.', '',
        '## Rebuild and rollback', '',
        'Keep the installation ledger folder and its latest current.json. Reuse that folder when adding, updating or removing packs. Numeric ID reservations are retained; removed packs emit no objects.',
        'To revert an installed build, restore its complete prior pack versions, generated ESP and bindings together. Do not mix generations. Do not rewind or delete the current allocation ledger to roll back game files.',
        'A verified local snapshot is evidence at build time; retain it unchanged. No support for objects from the old development DIAMOND.esp is included.']
    if rollback and rollback['verified']:
        lines += ['', 'Previous complete snapshot verified for rollback: `'+rollback['data']+'`.']
    elif rollback:
        lines += ['', 'The previous snapshot could not be verified. Restore the prior authored packs and companion from your own backup before reverting. Reason: '+rollback['reason']]
    else:
        lines += ['', 'This is the first generation in this ledger; there is no previous generated build to restore.']
    lines += ['', 'The JSON report includes exact generation, manifest checksums and source dependency declarations. These local reports are not part of a shareable authored pack.', '']
    (stage/'INSTALLATION.md').write_text('\n'.join(lines), encoding='utf-8')
    return dict(report=str(stage/'installation-report.json'), instructions=str(stage/'INSTALLATION.md'),
                snapshot=snapshot['data'], previousSnapshot=rollback)
