#include "ReloadMakerController.h"
#include "InstallationSetup.h"
#include "StudioPaths.h"
#include "StudioWindow.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QDir>
#include <QCoreApplication>
#include <QTimer>

namespace reloadmaker {
void ReloadMakerController::showSetup() {
    QDialog dialog(_window);dialog.setWindowTitle("Setup & tools");dialog.resize(720,570);
    auto* layout=new QVBoxLayout(&dialog);
    auto* intro=new QLabel("Choose your game and tools. Creation Kit tools are needed only for new cooked collision assets. AI clients connect externally through MCP.",&dialog);intro->setWordWrap(true);layout->addWidget(intro);
    auto* form=new QFormLayout;layout->addLayout(form);QString settingsError;auto current=loadInstallationSetup(&settingsError);
    const auto browse=[&](const QString& title,const QString& text){
        auto* row=new QWidget(&dialog);auto* h=new QHBoxLayout(row);h->setContentsMargins(0,0,0,0);
        auto* edit=new QLineEdit(text,row);auto* button=new QPushButton("Browse…",row);h->addWidget(edit,1);h->addWidget(button);
        connect(button,&QPushButton::clicked,&dialog,[&,edit,title]{const auto selected=QFileDialog::getExistingDirectory(&dialog,title,edit->text());if(!selected.isEmpty()){edit->setText(selected);emit edit->editingFinished();}});
        form->addRow(title,row);return edit;
    };
    auto* game=browse("Game folder",current.gameRoot);
    auto* same=new QCheckBox("Creation Kit is in the game folder",&dialog);same->setChecked(current.sameToolsRoot);form->addRow(QString(),same);
    auto* kit=browse("Creation Kit folder",current.creationKitRoot);kit->parentWidget()->setEnabled(!same->isChecked());
    auto* mo2=browse("MO2 instance folder",current.mo2Instance);
    auto* profiles=new QComboBox(&dialog);form->addRow("MO2 profile",profiles);
    auto* workspace=new QLabel(QDir::toNativeSeparators(studioRoot()),&dialog);workspace->setTextInteractionFlags(Qt::TextSelectableByMouse);form->addRow("Workspace",workspace);
    auto* mcp=new QLineEdit('"'+QDir::toNativeSeparators(QDir(QCoreApplication::applicationDirPath()).filePath("ReloadMakerMcp.cmd"))+"\" --workspace \""+QDir::toNativeSeparators(studioRoot())+'"',&dialog);
    mcp->setReadOnly(true);form->addRow("External AI · MCP command",mcp);
    auto* skills=new QLabel(QDir::toNativeSeparators(QDir(QCoreApplication::applicationDirPath()).filePath("skills")),&dialog);skills->setTextInteractionFlags(Qt::TextSelectableByMouse);form->addRow("Bundled AI instructions",skills);
    auto* report=new QPlainTextEdit(&dialog);report->setReadOnly(true);layout->addWidget(report,1);
    const auto value=[&]{return InstallationSetup{game->text(),kit->text(),mo2->text(),profiles->currentText(),same->isChecked()};};
    const auto refresh=[&]{const auto c=checkInstallation(value());QStringList text;
        text<<QString("Game: %1").arg(c.gameReady?c.gameKind:"Needs configuration")<<QString("New collision export: %1").arg(c.collisionReady?"Elric detected":"Elric not configured")<<QString("MO2 profile: %1").arg(c.profileReady?profiles->currentText():"Not connected");
        if(!settingsError.isEmpty())text<<settingsError;
        if(c.collisionReady)text<<c.elric;
        text<<c.errors<<c.notes;report->setPlainText(text.join('\n'));
    };
    const auto refreshProfiles=[&]{const auto previous=profiles->currentText().isEmpty()?current.profile:profiles->currentText();const QSignalBlocker block(profiles);profiles->clear();profiles->addItems(mo2Profiles(mo2->text()));const auto index=profiles->findText(previous);if(index>=0)profiles->setCurrentIndex(index);refresh();};
    connect(game,&QLineEdit::editingFinished,&dialog,refresh);connect(kit,&QLineEdit::editingFinished,&dialog,refresh);
    connect(mo2,&QLineEdit::editingFinished,&dialog,refreshProfiles);connect(profiles,qOverload<int>(&QComboBox::currentIndexChanged),&dialog,[&]{refresh();});
    connect(same,&QCheckBox::toggled,&dialog,[&](bool enabled){kit->parentWidget()->setEnabled(!enabled);refresh();});
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,&dialog);layout->addWidget(buttons);
    auto* connectSource=buttons->addButton("Save and connect MO2",QDialogButtonBox::ActionRole);
    auto* packs=buttons->addButton("Export reload mods…",QDialogButtonBox::ActionRole);
    connect(packs,&QPushButton::clicked,&dialog,[&]{dialog.accept();QTimer::singleShot(0,this,&ReloadMakerController::showPackBuilder);});
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,[&]{if(configureInstallation(installationJson(value())))dialog.accept();else report->setPlainText(_lastError);});
    connect(connectSource,&QPushButton::clicked,&dialog,[&]{const auto c=checkInstallation(value());if(!c.gameReady||!c.profileReady){refresh();return;}
        if(!configureInstallation(installationJson(value()))||!connectInstalledProfile()){report->setPlainText(_lastError);return;}dialog.accept();});
    refreshProfiles();dialog.exec();
}
}
