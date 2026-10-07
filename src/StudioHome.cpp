#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "StudioPaths.h"
#include "InstallationSetup.h"
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>
#include <stdexcept>

namespace reloadmaker {
void ReloadMakerController::showHome() {
    if(auto* existing=_window->findChild<QDialog*>("MakerHome")){existing->show();existing->raise();return;}
    auto* dialog=new QDialog(_window);dialog->setObjectName("MakerHome");dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle("Reload Maker");dialog->resize(780,470);
    auto* layout=new QVBoxLayout(dialog);auto* heading=new QLabel("PHYSICAL RELOADS · CREATE, SHARE AND USE");heading->setObjectName("Section");layout->addWidget(heading);
    auto* workspace=new QLabel("Workspace: "+QDir::toNativeSeparators(studioRoot()));workspace->setWordWrap(true);workspace->setTextInteractionFlags(Qt::TextSelectableByMouse);layout->addWidget(workspace);
    auto* routes=new QHBoxLayout;layout->addLayout(routes,1);
    const auto card=[&](const QString& title,const QString& description,const QString& action){auto* box=new QGroupBox(title);auto* content=new QVBoxLayout(box);auto* text=new QLabel(description);text->setWordWrap(true);content->addWidget(text,1);auto* button=new QPushButton(action);button->setMinimumHeight(42);content->addWidget(button);routes->addWidget(box,1);return button;};
    auto* creator=card("CREATE / EDIT","Load an MO2 profile or a source package. Configure the mechanism, magazine hands, contact points and sounds. Creation Kit tools are needed when cooking new collision models.","Create or edit a reload");
    auto* player=card("USE FINISHED PACKS","Export saved source-plugin projects as one ESL-flagged ESP per source mod, together in one MO2 mod folder or ZIP / 7z.","Export reload mods");
    auto* status=new QLabel;status->setWordWrap(true);layout->addWidget(status);
    auto* actions=new QHBoxLayout;layout->addLayout(actions);auto* resume=new QPushButton("Continue where you left off");auto* setup=new QPushButton("Setup & tools");auto* close=new QPushButton("Close");for(auto* button:{resume,setup,close})actions->addWidget(button);
    const auto current=homeState();resume->setEnabled(current["canResume"].toBool());
    connect(creator,&QPushButton::clicked,dialog,[=,this]{try{chooseHomeRole("creator");dialog->close();if(currentWeapon())showChecklist();else showSetup();}catch(const std::exception& e){status->setText(e.what());}});
    connect(player,&QPushButton::clicked,dialog,[=,this]{try{chooseHomeRole("player");dialog->close();showPackBuilder();}catch(const std::exception& e){status->setText(e.what());}});
    resume->setToolTip(current["recentWeapon"].toString());
    connect(resume,&QPushButton::clicked,dialog,[=,this]{try{chooseHomeRole("creator");if(!leaveDocument(QStringLiteral("opening another source")))return;dialog->close();resumeLastSession();}catch(const std::exception& e){setStatus(e.what(),true);}});
    connect(setup,&QPushButton::clicked,dialog,[this,dialog]{dialog->close();showSetup();});connect(close,&QPushButton::clicked,dialog,&QDialog::close);
    dialog->show();
}
}
