// Source bar widgets. They render the browser state kept by
// areas/sources/SourceBrowser.cpp and send the user's choices back to it.
#include "ReloadMakerController.h"
#include "StudioWindow.h"
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
void ReloadMakerController::createSourceBar(QToolBar* bar) {
    _mo2Instance=QSettings().value("paths/mo2Instance").toString();
    _sourceLabel=new QLabel("No source",bar);_sourceLabel->setObjectName("SourceLabel");bar->addWidget(_sourceLabel);
    _pluginFilter=new QComboBox(bar);_pluginFilter->setObjectName("SourcePluginPicker");_pluginFilter->setAccessibleName("Plugin");
    _pluginFilter->setMinimumContentsLength(24);_pluginFilter->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    _pluginFilter->setMaxVisibleItems(30);_pluginFilter->setPlaceholderText("Select a plugin…");bar->addWidget(_pluginFilter);
    _pluginSearch=new QToolButton(bar);_pluginSearch->setObjectName("SourcePluginSearch");
    _pluginSearch->setAccessibleName("Search plugins");_pluginSearch->setToolTip("Search plugins by name");
    QPixmap icon(32,32);icon.fill(Qt::transparent);
    {QPainter painter(&icon);painter.setRenderHint(QPainter::Antialiasing);painter.setPen(QPen(QColor("#dbe7e9"),2.8));
        painter.drawEllipse(QRectF(5,5,15,15));painter.drawLine(QPointF(18,18),QPointF(27,27));}
    _pluginSearch->setIcon(QIcon(icon));_pluginSearch->setIconSize(QSize(20,20));
    _pluginSearch->setPopupMode(QToolButton::InstantPopup);bar->addWidget(_pluginSearch);
    auto* searchMenu=new QMenu(_pluginSearch);_pluginSearch->setMenu(searchMenu);
    auto* panel=new QWidget(searchMenu);panel->setMinimumWidth(400);auto* layout=new QVBoxLayout(panel);
    auto* search=new QLineEdit(panel);search->setObjectName("PluginSearchText");search->setPlaceholderText("Search plugins…");search->setClearButtonEnabled(true);layout->addWidget(search);
    auto* matches=new QSortFilterProxyModel(panel);matches->setSourceModel(_pluginFilter->model());
    matches->setFilterRole(Qt::DisplayRole);matches->setFilterCaseSensitivity(Qt::CaseInsensitive);
    auto* results=new QListView(panel);results->setObjectName("PluginSearchResults");results->setModel(matches);
    results->setMinimumHeight(250);layout->addWidget(results);
    auto* searchAction=new QWidgetAction(searchMenu);searchAction->setDefaultWidget(panel);searchMenu->addAction(searchAction);
    connect(search,&QLineEdit::textChanged,panel,[matches,results](const QString& text){matches->setFilterFixedString(text);results->setCurrentIndex(matches->index(0,0));});
    const auto choose=[this,searchMenu](const QModelIndex& index){if(!index.isValid())return;const auto name=index.data(Qt::UserRole).toString();searchMenu->hide();(void)selectSourcePlugin(name);};
    connect(results,&QListView::clicked,panel,choose);connect(results,&QListView::activated,panel,choose);
    connect(search,&QLineEdit::returnPressed,panel,[results,choose]{choose(results->currentIndex());});
    connect(searchMenu,&QMenu::aboutToShow,panel,[this,search,results,matches]{search->clear();results->setCurrentIndex(matches->index(_pluginFilter->currentIndex(),0));search->setFocus();});
    _weaponPicker=new QComboBox(bar);_weaponPicker->setObjectName("SourceWeaponPicker");_weaponPicker->setAccessibleName("Weapon");
    _weaponPicker->setMinimumContentsLength(24);_weaponPicker->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    _weaponPicker->setMaxVisibleItems(30);_weaponPicker->setPlaceholderText("Choose a plugin first…");bar->addWidget(_weaponPicker);
    connect(_pluginFilter,qOverload<int>(&QComboBox::activated),this,[this](int i){(void)selectSourcePlugin(_pluginFilter->itemData(i).toString());});
    connect(_weaponPicker,qOverload<int>(&QComboBox::activated),this,[this](int i){if(i>=0)(void)selectSourceWeapon(_weaponPicker->itemData(i).toInt());});
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
        _sourceLabel->setText("MO2");
        _sourceLabel->setToolTip(_launchSnapshot.instancePath + "\nProfile: " + _launchSnapshot.profileName +
                                 "\nWinning records follow this profile's load order.");
    } else {
        _sourceLabel->setText(_package.sourcePath.isEmpty() ? "No source" : "Development source");
        _sourceLabel->setToolTip(_package.sourcePath);
    }
    resolveBrowserPlugin();
    renderPluginPicker();
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

// With several plugins to choose from, opens the plugin picker.
void ReloadMakerController::offerPluginChoice()
{
    if (_pluginFilter->count() > 1) {
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
