// Source bar widgets. They render the browser state kept by
// areas/sources/SourceBrowser.cpp and send the user's choices back to it.
#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "LoadoutView.h"
#include "ReloadTemplates.h"
#include "design/DesignWidgets.h"
#include "design/Icons.h"
#include "design/Theme.h"
#include <QFrame>
#include <QPushButton>
#include <QHBoxLayout>
#include <QScreen>
#include <QStyle>
#include <QBrush>
#include <QColor>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPainter>
#include <QSettings>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>

namespace reloadmaker {
// The weapon pill's popover: where the weapon comes from, then the plugin
// and the weapon. A popup, so it closes when the weapon is chosen or on any
// click outside.
void ReloadMakerController::createSourcePicker() {
    using namespace design;
    _mo2Instance=QSettings().value("paths/mo2Instance").toString();
    _sourcePicker=new QFrame(_window,Qt::Popup);
    _sourcePicker->setObjectName("Popover");
    _sourcePicker->setMinimumWidth(440);
    auto* layout=new QVBoxLayout(_sourcePicker);layout->setContentsMargins(16,14,16,16);layout->setSpacing(6);
    layout->addWidget(sectionLabel("Source",_sourcePicker));
    auto* sourceRow=new QHBoxLayout;sourceRow->setSpacing(8);
    _sourceLabel=new QLabel("No source",_sourcePicker);_sourceLabel->setObjectName("SourceLabel");_sourceLabel->setWordWrap(true);
    sourceRow->addWidget(_sourceLabel,1);
    auto* change=new QToolButton(_sourcePicker);change->setText("Change source");change->setPopupMode(QToolButton::InstantPopup);change->setMenu(_sourceMenu);
    sourceRow->addWidget(change);layout->addLayout(sourceRow);
    layout->addSpacing(6);
    layout->addWidget(sectionLabel("Plugin",_sourcePicker));
    auto* pluginRow=new QHBoxLayout;pluginRow->setSpacing(6);
    _pluginFilter=new QComboBox(_sourcePicker);_pluginFilter->setObjectName("SourcePluginPicker");_pluginFilter->setAccessibleName("Plugin");
    _pluginFilter->setMinimumContentsLength(24);_pluginFilter->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    _pluginFilter->setMaxVisibleItems(30);_pluginFilter->setPlaceholderText("Select a plugin…");pluginRow->addWidget(_pluginFilter,1);
    _pluginSearch=new QToolButton(_sourcePicker);_pluginSearch->setObjectName("SourcePluginSearch");
    _pluginSearch->setAccessibleName("Search plugins");_pluginSearch->setToolTip("Search plugins by name");
    _pluginSearch->setIcon(icon(Icon::Search,color::textSecondary));_pluginSearch->setIconSize(QSize(16,16));
    _pluginSearch->setPopupMode(QToolButton::InstantPopup);pluginRow->addWidget(_pluginSearch);
    layout->addLayout(pluginRow);
    auto* searchMenu=new QMenu(_pluginSearch);_pluginSearch->setMenu(searchMenu);
    auto* panel=new QWidget(searchMenu);panel->setMinimumWidth(400);auto* searchLayout=new QVBoxLayout(panel);
    auto* search=new QLineEdit(panel);search->setObjectName("PluginSearchText");search->setPlaceholderText("Search plugins…");search->setClearButtonEnabled(true);searchLayout->addWidget(search);
    auto* matches=new QSortFilterProxyModel(panel);matches->setSourceModel(_pluginFilter->model());
    matches->setFilterRole(Qt::DisplayRole);matches->setFilterCaseSensitivity(Qt::CaseInsensitive);
    auto* results=new QListView(panel);results->setObjectName("PluginSearchResults");results->setModel(matches);
    results->setMinimumHeight(250);searchLayout->addWidget(results);
    auto* searchAction=new QWidgetAction(searchMenu);searchAction->setDefaultWidget(panel);searchMenu->addAction(searchAction);
    connect(search,&QLineEdit::textChanged,panel,[matches,results](const QString& text){matches->setFilterFixedString(text);results->setCurrentIndex(matches->index(0,0));});
    const auto choose=[this,searchMenu](const QModelIndex& index){if(!index.isValid())return;const auto name=index.data(Qt::UserRole).toString();searchMenu->hide();(void)selectSourcePlugin(name);};
    connect(results,&QListView::clicked,panel,choose);connect(results,&QListView::activated,panel,choose);
    connect(search,&QLineEdit::returnPressed,panel,[results,choose]{choose(results->currentIndex());});
    connect(searchMenu,&QMenu::aboutToShow,panel,[this,search,results,matches]{search->clear();results->setCurrentIndex(matches->index(_pluginFilter->currentIndex(),0));search->setFocus();});
    layout->addSpacing(6);
    layout->addWidget(sectionLabel("Weapon",_sourcePicker));
    _weaponPicker=new QComboBox(_sourcePicker);_weaponPicker->setObjectName("SourceWeaponPicker");_weaponPicker->setAccessibleName("Weapon");
    _weaponPicker->setMinimumContentsLength(24);_weaponPicker->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    _weaponPicker->setMaxVisibleItems(30);_weaponPicker->setPlaceholderText("Choose a plugin first…");layout->addWidget(_weaponPicker);
    connect(_pluginFilter,qOverload<int>(&QComboBox::activated),this,[this](int i){(void)selectSourcePlugin(_pluginFilter->itemData(i).toString());});
    connect(_weaponPicker,qOverload<int>(&QComboBox::activated),this,[this](int i){if(i<0)return;_sourcePicker->hide();(void)selectSourceWeapon(_weaponPicker->itemData(i).toInt());});
}

void ReloadMakerController::showSourcePicker() {
    _sourcePicker->adjustSize();
    QPoint at=_weaponPill->mapToGlobal(QPoint(0,_weaponPill->height()+6));
    if(const auto* screen=_weaponPill->screen()){const auto area=screen->availableGeometry();at.setX(std::min(at.x(),area.right()-_sourcePicker->width()));}
    _sourcePicker->move(at);
    _sourcePicker->show();
    (_weaponPicker->isEnabled()?static_cast<QWidget*>(_weaponPicker):static_cast<QWidget*>(_pluginFilter))->setFocus();
}

// The weapon pill names the weapon and its plugin; the summary names the
// receiver and magazine; the chip names the behavior preset.
void ReloadMakerController::refreshHeader() {
    if(!_weaponPill)return;
    const auto* weapon=currentWeapon();
    const auto* plugin=currentPlugin();
    const auto* config=configuration();
    QString pill=QStringLiteral("Choose a weapon");
    if(weapon){
        const QString name=weapon->name.isEmpty()?weapon->editorId:weapon->name;
        const QString origin=plugin?(_libraryMode?plugin->origins.value(weapon->formId).plugin:plugin->fileName):QString();
        pill=origin.isEmpty()?name:name+QStringLiteral("  ·  ")+origin;
    }
    _weaponPill->setText(pill);
    QStringList summary;
    if(weapon&&config){
        // Slot labels are presentation only; they name what the header shows.
        for(const char* role:{"Receiver","Magazine"}){
            for(const auto& slot:config->entries()){
                if(slot.label.compare(QLatin1String(role),Qt::CaseInsensitive)!=0)continue;
                if(const auto* mod=config->mod(config->selected(slot.key)))summary<<QStringLiteral("%1 %2").arg(cleanPartName(mod->name.isEmpty()?mod->editorId:mod->name),QString(role).toLower());
                break;
            }
        }
    }
    _headerSummary->setText(summary.join(QStringLiteral(" · ")));
    _headerSummary->setVisible(!summary.isEmpty());
    // A preset the author chose is confirmed; otherwise Maker's own guess
    // shows as detected, the checklist's two states.
    const QString assigned=weapon?assignedTemplate():QString();
    const QString current=assigned.isEmpty()&&weapon?templateIdentity(simulationMechanism()):assigned;
    QString preset;
    for(const auto& item:reloadPresets())if(item.id==current){preset=item.label;break;}
    const bool detected=assigned.isEmpty();
    _presetChip->setText(preset.isEmpty()?QStringLiteral("No behavior preset"):QStringLiteral("Preset: ")+preset+(detected?QStringLiteral(" · detected"):QString()));
    _presetChip->setProperty("tone",preset.isEmpty()?QStringLiteral("warning"):detected?QStringLiteral("detected"):QString());
    _presetChip->setToolTip(preset.isEmpty()?QStringLiteral("Behavior presets are unavailable: ")+reloadPresetsError()
        :detected?QStringLiteral("Maker detected this preset. Confirm or change it in Menu › Author reload."):QStringLiteral("Behavior preset chosen for this weapon."));
    _presetChip->style()->unpolish(_presetChip);_presetChip->style()->polish(_presetChip);
    _presetChip->setVisible(weapon!=nullptr);
}

void ReloadMakerController::refreshSourceBar()
{
    if (!_sourceLabel) {
        return;
    }
    if (!_libraryMode && !_package.sourcePath.isEmpty() && !_testbedSourcesLoaded) {
        refreshDevelopmentSources();
    }
    if (_libraryMode) {
        _sourceLabel->setText(QStringLiteral("MO2 profile · ") + _launchSnapshot.profileName);
        _sourceLabel->setToolTip(_launchSnapshot.instancePath + "\nProfile: " + _launchSnapshot.profileName +
                                 "\nWinning records follow this profile's load order.");
    } else {
        _sourceLabel->setText(_package.sourcePath.isEmpty() ? "No source" : "Development source");
        _sourceLabel->setToolTip(_package.sourcePath);
    }
    resolveBrowserPlugin();
    renderPluginPicker();
    refreshHeader();
    _pluginFilter->setEnabled(!_loadThread && _pluginFilter->count() > 0);
    _pluginSearch->setEnabled(_pluginFilter->isEnabled());
    refreshWeaponPicker();
}

void ReloadMakerController::renderPluginPicker()
{
    if (!_pluginFilter) {
        return;
    }
    const QSignalBlocker block(_pluginFilter);
    _pluginFilter->clear();
    for (const SourcePluginRow& row : sourcePluginRows()) {
        const bool selected = row.role == SourcePluginRole::Selected;
        _pluginFilter->addItem(row.label, row.name);
        const int item = _pluginFilter->count() - 1;
        const QString tip = row.name == QLatin1String(kAllWeaponsSource)
            ? QString("All %1 editable weapons in this MO2 profile. Each weapon shows its originating plugin; records still follow the winning load order.").arg(row.weaponCount)
            : QString("%1\n%2 editable weapon record(s) in the loaded source.\nAmber: selected plugin · teal: required master.").arg(row.name).arg(row.weaponCount);
        _pluginFilter->setItemData(item, tip, Qt::ToolTipRole);
        const QColor text = selected ? QColor("#e8c17c") : row.role == SourcePluginRole::Master ? QColor("#80d2c7") : QColor("#dbe7e9");
        _pluginFilter->setItemData(item, QBrush(text), Qt::ForegroundRole);
        if (selected) {
            _pluginFilter->setItemData(item, QBrush(QColor("#334946")), Qt::BackgroundRole);
        }
        QFont font = _pluginFilter->font();
        font.setBold(selected);
        _pluginFilter->setItemData(item, font, Qt::FontRole);
    }
    _pluginFilter->setCurrentIndex(_pluginFilter->findData(_browserPlugin));
}

// With several plugins to choose from, opens the weapon pill on the plugin.
void ReloadMakerController::offerPluginChoice()
{
    if (_pluginFilter->count() > 1) {
        showSourcePicker();
        _pluginFilter->showPopup();
    }
}

void ReloadMakerController::refreshWeaponPicker()
{
    if (!_weaponPicker) {
        return;
    }
    const QSignalBlocker block(_weaponPicker);
    _weaponPicker->clear();
    for (const SourceWeaponRow& row : sourceWeaponRows()) {
        _weaponPicker->addItem(row.label, row.index);
        _weaponPicker->setItemData(_weaponPicker->count() - 1,
                                   QString("%1\n%2\nWinning record: %3").arg(row.editorId, row.record, row.winner), Qt::ToolTipRole);
    }
    _weaponPicker->setCurrentIndex(_weaponPicker->findData(_selectedWeapon));
    if (_browserPlugin.isEmpty()) {
        _weaponPicker->setPlaceholderText("Choose a plugin first…");
    } else {
        _weaponPicker->setPlaceholderText(_weaponPicker->count() ? "Select a weapon…" : "No editable weapons in this plugin");
    }
    _weaponPicker->setEnabled(!_loadThread && _weaponPicker->count() > 0);
}
}
