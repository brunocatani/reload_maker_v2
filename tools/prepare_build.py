"""Stage the native engine without the NifSkope application or generic editor."""
import json
import hashlib
import re
import shutil
import sys
from pathlib import Path

source, stage, project = map(Path, sys.argv[1:])
if not stage.exists():
    shutil.copytree(source, stage, ignore=shutil.ignore_patterns('.git', '.github', 'bin', 'debug', 'release', 'GeneratedFiles', '*.o'))

def write(relative, value):
    path = stage / relative
    if not path.exists() or path.read_text(encoding='utf-8') != value:
        path.write_text(value, encoding='utf-8')

def patch(relative, replacements, finish=None):
    value = (source / relative).read_text(encoding='utf-8')
    for old, new in replacements:
        if value.count(old) != 1:
            raise RuntimeError(f'Native adapter marker changed: {relative}: {old[:90]}')
        value = value.replace(old, new)
    if finish is not None:
        value = finish(value)
    write(relative, value)

def replace_exactly(text, old, new, where, count=1):
    if text.count(old) != count:
        raise RuntimeError(f'Native adapter marker changed: {where}: {old[:90]}')
    return text.replace(old, new)

def remove_method(text, signature):
    start = text.index(signature)
    begin = text.index('{', start)
    depth = 0
    # Ignore braces in strings and comments while finding a complete definition.
    for token in re.finditer(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/|[{}]', text[begin:]):
        if token.group() == '{': depth += 1
        elif token.group() == '}':
            depth -= 1
            if depth == 0: return text[:start] + text[begin + token.end():]
    raise RuntimeError(f'Unclosed native method: {signature}')

patch('src/glview.h', [
    ('\tQModelIndex indexAt( const QPointF & p, bool shiftModifier = false );',
     '\tQModelIndex indexAt( const QPointF & p, bool shiftModifier = false );\n\tQMatrix4x4 reloadMakerWorldToClip() const;\n\tQImage studioCaptureFrame();\n\tQJsonObject studioDiagnostics() const;\n\tQVector<QVector3D> studioGeometryBounds(const QVector<int>& blocks);\n\tbool studioFitBounds(const QVector<QVector3D>& points);\n\tQJsonObject studioCameraState() const;\n\tbool studioRestoreCamera(const QJsonObject& value);\n\tbool reloadMakerSurfaceAt(const QPointF& point, const QVector<int>& allowedBlocks, int& block, Vector3& world);\n\t// Part outlines drawn after the scene (ReloadMakerHighlight.cpp).\n\tstruct ReloadMakerHighlight { QVector<int> blocks; QColor color; float width = 3.0f; };\n\tvoid reloadMakerSetHighlights(QVector<ReloadMakerHighlight> highlights);\n\tvoid reloadMakerDrawHighlights();\n\tQVector<ReloadMakerHighlight> reloadMakerHighlights;\n\tunsigned int reloadMakerHighlightQuery = 0;\n\tbool reloadMakerHighlightQueryPending = false;\n\tdouble reloadMakerHighlightMs = -1.0;'),
    ('#include <QOpenGLWindow> // Inherited', '#include <QOpenGLWindow> // Inherited\n#include <QColor>\n#include <QMatrix4x4>\n#include <QJsonObject>\n#include <QVector3D>'),
    ('\tstatic bool selectPBRCubeMapForGame( quint32 bsVersion );', ''),
    ('\tvoid selectPBRCubeMap();', '')])
patch('src/gl/glshape.h', [
    ('\tvirtual void updateLodLevel() { lodTriangleCount = triangles.size(); }',
     '\tvirtual void updateLodLevel() { lodTriangleCount = triangles.size(); }\n\tvoid reloadMakerDrawSurface();\n\tvoid reloadMakerDrawHighlight(const FloatVector4& color);\n\tQVector<int> reloadMakerSkinBones() const;\n\tQVector<Vector3> reloadMakerCollisionVertices() const;\n\tqsizetype reloadMakerCollisionTriangleCount() const;\n\tQJsonObject reloadMakerRigidMeshJson() const;'),
    ('#include <QString>', '#include <QString>\n#include <QJsonObject>')])
patch('src/glview.cpp', [
    ('#include "nifskope.h"', ''),
    ('#include "ui/settingsdialog.h"', ''),
    ('settings.value( "General/Camera/Field Of View" )', 'settings.value( "General/Camera/Field Of View", 60 )'),
    ('settings.value( "General/Camera/Movement Speed" )', 'settings.value( "General/Camera/Movement Speed", 350 )'),
    ('settings.value( "General/Camera/Rotation Speed" )', 'settings.value( "General/Camera/Rotation Speed", 45 )'),
    ('\t\taxis = (scene->bounds().radius <= 0) ? 1024.0 * scale() : scene->bounds().radius;',
     '\t\t// Studio assembly supplies weapon-only navigation bounds; retain them on recompilation.'),
    ('\tconnect( NifSkope::getOptions(), &SettingsDialog::flush3D, textures, &TexCache::flush );', ''),
    ('\tconnect( NifSkope::getOptions(), &SettingsDialog::update3D, this, &GLView::update3D );', ''),
    ('\tif ( NifSkope::getOptions() )\n\t\tNifSkope::getOptions()->apply();', ''),
    ('\tif ( NifSkope::getOptions() )\n\t\temit NifSkope::getOptions()->loadSettings();', '\tGame::GameManager::close_resources();'),
    ('NifSkope::fileExtensions()', 'QStringList{QStringLiteral("nif")}'),
    ('\t\tauto ns = qobject_cast<NifSkope *>( graphicsView->parent() );\n\t\tif ( ns )\n\t\t\tns->openFiles( draggedNifs );',
     '\t\t// Source selection belongs to the studio, never a generic NIF window.'),
    ('\temit paintUpdate();', '\tglProjection();\n\temit paintUpdate();'),
    # Selection and hover outlines, over the model and under the axes overlay.
    ('\tglDisable( GL_BLEND );\n\tscene->draw();\n', '\tglDisable( GL_BLEND );\n\tscene->draw();\n\treloadMakerDrawHighlights();\n'),
    # An unattended (automated) editor never waits for the display between
    # commands; the interactive editor keeps V-Sync.
    ('\tfmt.setSwapInterval( DEBUG_FRAME_TIME ? 0 : 1 );',
     '\tfmt.setSwapInterval( ( DEBUG_FRAME_TIME || qApp->property( "unattended" ).toBool() ) ? 0 : 1 );'),
    # setNifFolder flushes every texture, so each recompile (every part swap)
    # loaded all textures again on the next frame (about 0.5 s on the Glock,
    # 2026-10-04). The studio flushes textures itself when its source changes
    # (clearAssetCaches -> GLView::flush); a recompile of the same source
    # keeps them.
    ('\t\ttextures->setNifFolder( model->getFolder() );\n',
     '\t\t// Textures are flushed by the studio when its source changes.\n')],
    finish=lambda value: remove_method(remove_method(value,
        'bool GLView::selectPBRCubeMapForGame('), 'void GLView::selectPBRCubeMap()').replace('#include "ui/widgets/filebrowser.h"', ''))
# NifSkope gives every billboard the "always face camera" rotation. Laser sights
# (NiBillboardNode "LaserSightBeam", mode ROTATE_ABOUT_UP) author the beam along
# the node's local +Y with the spot at +Y 1000, ahead of the barrel; drawn facing
# the camera, the beam stood vertically on screen (2026-10-04). ROTATE_ABOUT_UP
# keeps the node's own up axis (local Y) and only turns local Z toward the viewer
# about it. Other modes keep NifSkope's behaviour.
patch('src/gl/glnode.h', [
    ('\tBillboardNode( Scene * scene, const QModelIndex & block );\n\n\tconst Transform & viewTrans() const override;\n};',
     '\tBillboardNode( Scene * scene, const QModelIndex & block );\n\n\tconst Transform & viewTrans() const override;\n\n'
     'protected:\n\tvoid updateImpl( const NifModel * nif, const QModelIndex & block ) override;\n\n'
     '\t//! NiBillboardNode "Billboard Mode"; 1 is ROTATE_ABOUT_UP.\n\tint billboardMode = 0;\n};')])
patch('src/gl/glnode.cpp', [
    ('BillboardNode::BillboardNode( Scene * scene, const QModelIndex & iBlock )\n\t: Node( scene, iBlock )\n{\n}\n',
     'BillboardNode::BillboardNode( Scene * scene, const QModelIndex & iBlock )\n\t: Node( scene, iBlock )\n{\n}\n\n'
     'void BillboardNode::updateImpl( const NifModel * nif, const QModelIndex & index )\n{\n'
     '\tNode::updateImpl( nif, index );\n\tif ( iBlock == index )\n\t\tbillboardMode = nif->get<int>( iBlock, "Billboard Mode" );\n}\n'),
    ('\tt.rotation = Matrix();\n',
     '\tif ( billboardMode == 1 ) {\n'
     '\t\t// The columns of a view rotation are the node\'s local axes in view space;\n'
     '\t\t// view +Z points at the viewer.\n'
     '\t\tVector3 up( t.rotation( 0, 1 ), t.rotation( 1, 1 ), t.rotation( 2, 1 ) );\n'
     '\t\tup.normalize();\n'
     '\t\tVector3 toward = Vector3( 0, 0, 1 ) - up * up[2];\n'
     '\t\tif ( toward.length() > 1e-4f ) {\n'
     '\t\t\ttoward.normalize();\n'
     '\t\t\tconst Vector3 side = Vector3::crossproduct( up, toward );\n'
     '\t\t\tfor ( int row = 0; row < 3; ++row ) {\n'
     '\t\t\t\tt.rotation( row, 0 ) = side[row];\n'
     '\t\t\t\tt.rotation( row, 1 ) = up[row];\n'
     '\t\t\t\tt.rotation( row, 2 ) = toward[row];\n'
     '\t\t\t}\n'
     '\t\t}\n'
     '\t\t// Seen straight along its up axis the node keeps its own orientation.\n'
     '\t\t*p = t;\n'
     '\t\treturn *p;\n'
     '\t}\n'
     '\tt.rotation = Matrix();\n')])
patch('src/gl/renderer.cpp', [
    ('#include "nifskope.h"', ''), ('#include "ui/settingsdialog.h"', ''),
    ('\tconnect( NifSkope::getOptions(), &SettingsDialog::saveSettings, this, &Renderer::updateSettings );', '')])
patch('src/gl/glscene.cpp', [
    ('#include "nifskope.h"', '#include "StudioWindow.h"'),
    ('NifSkope *\tw;', 'StudioWindow *\tw;'),
    ('dynamic_cast< NifSkope * >', 'dynamic_cast< StudioWindow * >'),
    ('\t//if ( flushTextures )\n\ttextures->flush();', '\tif ( flushTextures )\n\t\ttextures->flush();')])
patch('src/gamemanager.cpp', [
    ('#include "gamemanager.h"', '#include "gamemanager.h"\n#include "LibraryResources.h"'),
    ('QString GameManager::GameResources::find_file( const std::string_view & fullPath )\n{',
     'QString GameManager::GameResources::find_file( const std::string_view & fullPath )\n{\n\tif (game == FALLOUT_4 && reloadmaker::libraryResourcesActive()) return reloadmaker::findLibraryResource(fullPath);'),
    ('bool GameManager::GameResources::get_file( QByteArray & data, const std::string_view & fullPath )\n{',
     'bool GameManager::GameResources::get_file( QByteArray & data, const std::string_view & fullPath )\n{\n\tif (game == FALLOUT_4 && reloadmaker::libraryResourcesActive()) return reloadmaker::readLibraryResource(data, fullPath);')])
# Generic block-type conversion and Starfield mesh conversion are not part of
# FO4 reload assembly. Keep native branch copy/link without those editor paths.
patch('src/spells/blocks.h', [
    ('#include "spellbook.h"', '#include "spellbook.h"\nclass QMimeData;'),
    ('\tQString name() const override final { return Spell::tr( "Copy Branch" ); }',
     '\tQModelIndex copyData(NifModel* nif, const QModelIndex& index, QMimeData& payload);\n\tQString name() const override final { return Spell::tr( "Copy Branch" ); }'),
    ('\tQString name() const override final { return Spell::tr( "Paste Branch" ); }',
     '\tQModelIndex pasteData(NifModel* nif, const QModelIndex& index, const QMimeData& payload);\n\tQString name() const override final { return Spell::tr( "Paste Branch" ); }')])
blocks = (source/'src/spells/blocks.cpp').read_text(encoding='utf-8')
# Reuse native branch serialization through a caller-owned payload. Interactive
# spell wrappers retain their original clipboard behavior; studio assembly does
# not depend on clipboard ownership, external contents, or a stale prior copy.
blocks=replace_exactly(blocks, '#include <QMimeData>', '#include <QMimeData>\n#include <memory>', 'src/spells/blocks.cpp')
for spell,method,parameter,old,new,wrapper in [
    ('spCopyBranch','copyData','QMimeData& payload',
     '\t\tQMimeData * mime = new QMimeData;\n\t\tmime->setData( QString( STR_BR ).arg( nif->getVersion() ), data );\n\t\tQApplication::clipboard()->setMimeData( mime );',
     '\t\tpayload.setData( QString( STR_BR ).arg( nif->getVersion() ), data );',
     '\tauto payload=std::make_unique<QMimeData>();\n\tconst auto result=copyData(nif,index,*payload);\n\tif(!payload->formats().isEmpty())QApplication::clipboard()->setMimeData(payload.release());\n\treturn result;'),
    ('spPasteBranch','pasteData','const QMimeData& payload',
     '\tconst QMimeData * mime = QApplication::clipboard()->mimeData();',
     '\tconst QMimeData * mime = &payload;',
     '\tconst auto* payload=QApplication::clipboard()->mimeData();\n\treturn payload?pasteData(nif,index,*payload):QModelIndex{};')]:
    signature=f'QModelIndex {spell}::cast( NifModel * nif, const QModelIndex & index )'
    begin=blocks.index(signature);end=blocks.index(f'REGISTER_SPELL( {spell} )',begin)
    original=blocks[begin:end]
    if original.count(old)!=1:raise RuntimeError(f'Native branch transfer marker changed: {spell}')
    direct=original.replace(signature,f'QModelIndex {spell}::{method}(NifModel* nif, const QModelIndex& index, {parameter})',1).replace(old,new,1)
    blocks=blocks[:begin]+signature+'\n{\n'+wrapper+'\n}\n\n'+direct+blocks[end:]
begin = blocks.index('class spConvertBlock final')
end = blocks.index('REGISTER_SPELL( spConvertBlock )', begin)+len('REGISTER_SPELL( spConvertBlock )')
write('src/spells/blocks.cpp', blocks[:begin]+blocks[end:])
model = (source/'src/model/nifmodel.cpp').read_text(encoding='utf-8')
model = replace_exactly(model, 'spMeshFileImport::processAllItems( this );', 'throw NifSkopeError("Starfield mesh conversion is outside the Fallout 4 reload workflow");', 'src/model/nifmodel.cpp', count=2)
# moveAllNiBlocks appends before the footer, at row getBlockCount() + 1 (row 0
# is the header), like insertNiBlock. It announced the insertion one row early,
# so Qt shifted every persistent index on the last existing block by the number
# of blocks moved in: a part surface recorded on it later named another part's
# shape (Glock Mag:0 read as the sights' Glow:0, 2026-10-05).
model = replace_exactly(model,
    'targetnif->beginInsertRows( QModelIndex(), targetnif->getBlockCount(), targetnif->getBlockCount() + bcnt - 1 );',
    'targetnif->beginInsertRows( QModelIndex(), targetnif->getBlockCount() + 1, targetnif->getBlockCount() + bcnt );',
    'src/model/nifmodel.cpp moveAllNiBlocks')
write('src/model/nifmodel.cpp', model)

pro = (source / 'NifSkope.pro').read_text(encoding='utf-8')
native = []
for folder in ('src/data', 'src/gl', 'src/io'):
    native += [p.relative_to(source).as_posix() for p in (source / folder).glob('*.cpp')]
native += ['src/model/basemodel.cpp', 'src/model/nifmodel.cpp', 'src/model/nifextfiles.cpp',
           'src/model/kfmmodel.cpp', 'src/model/undocommands.cpp', 'src/spells/blocks.cpp',
           'src/spellbook.cpp', 'src/gamemanager.cpp', 'src/glview.cpp', 'src/message.cpp',
           'src/xml/nifexpr.cpp', 'src/xml/nifxml.cpp', 'src/xml/kfmxml.cpp',
           'src/ui/checkablemessagebox.cpp', 'src/ui/widgets/fileselect.cpp',
           'src/ui/widgets/floatslider.cpp', 'src/ui/widgets/floatedit.cpp',
           'src/lib/nvtristripwrapper.cpp']
original_sources = re.findall(r'^\s*(src/\S+\.cpp)\s*\\?$', pro, re.M)
removed = [p for p in original_sources if p not in native]
headers = [p[:-4]+'.h' for p in native if (source/(p[:-4]+'.h')).exists()]
headers += ['src/xml/xmlconfig.h', 'src/qt5compat.hpp', 'src/gl/icontrollable.h', 'src/io/material.h']
rock = project.parents[1]/'PROJECT_ROCK_V2/main_projects/ROCK/src'
policies = ['physics-interaction/weapon/WeaponGapDecomposition.h', 'physics-interaction/weapon/WeaponGeometry.h', 'physics-interaction/weapon/WeaponGeometryTask.h', 'physics-interaction/weapon/WeaponEffectGeometryPolicy.h', 'physics-interaction/VectorMath.h', 'physics-interaction/weapon/WeaponCollisionInternal.h', 'physics-interaction/weapon/collision/GeneratedWeaponSources.cpp']
# Changing upstream production defaults requires reviewing the offline adapter.
constants = (rock/'physics-interaction/weapon/WeaponCollisionInternal.h').read_text(encoding='utf-8')
for name, expected in {'MAX_CONVEX_HULL_POINTS':252, 'WEAPON_COLLISION_CONVEX_RADIUS_HAVOK':.01, 'WEAPON_COLLISION_POINT_DEDUP_GRID_HAVOK':.002, 'WEAPON_COLLISION_SUPPORT_FIT_TARGET_POINTS':96, 'WEAPON_COLLISION_SUPPORT_FIT_MAX_ERROR_GAME_UNITS':.5}.items():
    token = re.search(r'\b'+name+r'\s*=\s*([^;]+);', constants)
    if not token: raise RuntimeError(f'ROCK collision default missing: {name}')
    value = token.group(1).strip().removesuffix('f')
    actual = int(value,16) if value.startswith('0x') else float(value)
    if actual != expected: raise RuntimeError(f'ROCK collision default changed: {name}; review CollisionAuthoring before building')
digest = hashlib.sha256()
for name in policies:
    digest.update(name.encode()); digest.update((rock/name).read_bytes())
write('RockCollisionProvenance.h', '#pragma once\ninline constexpr const char* kRockCollisionSourceHash="'+digest.hexdigest()+'";\n')
app = project.as_posix()
pro += '\n# Reload Maker V2: native engine only; independent application window.\n'
pro += 'TARGET = ReloadMakerV2\nQMAKE_PRE_LINK =\n'
pro += 'SOURCES -= ' + ' '.join(removed) + '\n'
pro += 'HEADERS = ' + ' '.join(headers) + '\n'
pro += 'FORMS = src/ui/checkablemessagebox.ui\n'
app_sources = sorted((project/'src').rglob('*.cpp'))
# qmake writes every object file into one folder, so basenames must be unique
# across the application and the staged native sources.
object_names = [Path(p).stem.lower() for p in native] + [p.stem.lower() for p in app_sources]
duplicates = sorted({name for name in object_names if object_names.count(name) > 1})
if duplicates:
    raise SystemExit('Source basenames must be unique for qmake objects: ' + ', '.join(duplicates))
pro += 'SOURCES += ' + ' '.join(p.as_posix() for p in app_sources) + '\n'
pro += 'HEADERS += ' + ' '.join(p.as_posix() for p in sorted((project/'src').rglob('*.h'))) + '\n'
pro += f'INCLUDEPATH += {app}/src\nDEFINES += RELOAD_MAKER_V2\n'
pro += f'INCLUDEPATH += {rock.as_posix()} {stage.as_posix()}\nCONFIG += c++20\nQT += concurrent\n'
pro += 'SOURCES += $$files($$PWD/lib/qhull/src/libqhull_r/*.c)\n'
pro += 'win32:LIBS += -lxaudio2_8 -lole32\n'
write('NifSkope.pro', pro)
(project/'local/native-build-manifest.json').write_text(json.dumps({'nativeSources':native,'excludedApplicationSources':removed},indent=2))
