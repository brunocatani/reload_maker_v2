"""Stage the native engine without the NifSkope application or generic editor."""
import json
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
     '\tQModelIndex indexAt( const QPointF & p, bool shiftModifier = false );\n\tQMatrix4x4 reloadMakerWorldToClip() const;\n\tQImage studioCaptureFrame();\n\tQJsonObject studioDiagnostics() const;\n\tQVector<QVector3D> studioGeometryBounds(const QVector<int>& blocks);\n\tbool studioFitBounds(const QVector<QVector3D>& points);\n\tQJsonObject studioCameraState() const;\n\tbool studioRestoreCamera(const QJsonObject& value);\n\tbool reloadMakerSurfaceAt(const QPointF& point, const QVector<int>& allowedBlocks, int& block, Vector3& world);'),
    ('#include <QOpenGLWindow> // Inherited', '#include <QOpenGLWindow> // Inherited\n#include <QMatrix4x4>\n#include <QJsonObject>\n#include <QVector3D>'),
    ('\tstatic bool selectPBRCubeMapForGame( quint32 bsVersion );', ''),
    ('\tvoid selectPBRCubeMap();', '')])
patch('src/gl/glshape.h', [
    ('\tvirtual void updateLodLevel() { lodTriangleCount = triangles.size(); }',
     '\tvirtual void updateLodLevel() { lodTriangleCount = triangles.size(); }\n\tvoid reloadMakerDrawSurface();\n\tQVector<int> reloadMakerSkinBones() const;')])
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
    ('\temit paintUpdate();', '\tglProjection();\n\temit paintUpdate();')],
    finish=lambda value: remove_method(remove_method(value,
        'bool GLView::selectPBRCubeMapForGame('), 'void GLView::selectPBRCubeMap()').replace('#include "ui/widgets/filebrowser.h"', ''))
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
blocks=blocks.replace('#include <QMimeData>', '#include <QMimeData>\n#include <memory>',1)
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
model = model.replace('spMeshFileImport::processAllItems( this );', 'throw NifSkopeError("Starfield mesh conversion is outside the Fallout 4 reload workflow");')
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
app = project.as_posix()
pro += '\n# Reload Maker V2: native engine only; independent application window.\n'
pro += 'TARGET = ReloadMakerV2\nQMAKE_PRE_LINK =\n'
pro += 'SOURCES -= ' + ' '.join(removed) + '\n'
pro += 'HEADERS = ' + ' '.join(headers) + '\n'
pro += 'FORMS = src/ui/checkablemessagebox.ui\n'
pro += 'SOURCES += ' + ' '.join(p.as_posix() for p in (project/'src').glob('*.cpp')) + '\n'
pro += 'HEADERS += ' + ' '.join(p.as_posix() for p in (project/'src').glob('*.h')) + '\n'
pro += f'INCLUDEPATH += {app}/src\nDEFINES += RELOAD_MAKER_V2\n'
pro += 'win32:LIBS += -lxaudio2_8 -lole32\n'
write('NifSkope.pro', pro)
(project/'local/native-build-manifest.json').write_text(json.dumps({'nativeSources':native,'excludedApplicationSources':removed},indent=2))
