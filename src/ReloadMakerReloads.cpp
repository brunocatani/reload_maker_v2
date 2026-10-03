#include "ReloadMakerController.h"
#include "ReloadGraph.h"
#include "StudioWindow.h"
#include "glview.h"
#include "model/nifmodel.h"
#include <QScopedValueRollback>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDockWidget>
#include <QDir>
#include <QFileInfo>
#include <QGroupBox>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

namespace reloadmaker {
    void ReloadMakerController::createReloadUi()
    {
        auto* page=new QWidget(_pages);auto* layout=new QVBoxLayout(page);layout->setContentsMargins(8,12,8,8);
        auto* title=new QLabel("RELOAD / LEARN",page);title->setStyleSheet("color:#e6bd7d;font-size:19px;font-weight:700;");layout->addWidget(title);
        auto* intro=new QLabel("Choose a phase, mark its grabbable parts, then test their native movement.",page);intro->setWordWrap(true);layout->addWidget(intro);
        _reloadSourceLabel=new QLabel("Load a weapon to resolve its base reload.",page);_reloadSourceLabel->setWordWrap(true);_reloadSourceLabel->setTextFormat(Qt::PlainText);layout->addWidget(_reloadSourceLabel);
        _reloadAnalyze=new QPushButton("LEARN RELOAD AGAIN",page);layout->addWidget(_reloadAnalyze);
        _reloadProgress=new QProgressBar(page);layout->addWidget(_reloadProgress);_reloadProgress->hide();
        _reloadSummary=new QLabel(page);_reloadSummary->setWordWrap(true);layout->addWidget(_reloadSummary);
        auto* playback=new QHBoxLayout();_reloadPlay=new QPushButton("PLAY RELOAD",page);_reloadTest=new QPushButton("TEST WINDOW",page);_reloadTest->setCheckable(true);
        playback->addWidget(_reloadPlay);playback->addWidget(_reloadTest);layout->addLayout(playback);
        _reloadWindows=new QListWidget(page);_reloadWindows->setAlternatingRowColors(true);layout->addWidget(_reloadWindows,1);
        _reloadWindowProgress=new QSlider(Qt::Horizontal,page);_reloadWindowProgress->setRange(0,10000);layout->addWidget(_reloadWindowProgress);
        auto* edits=new QHBoxLayout();auto* split=new QPushButton("SPLIT AT CURSOR",page);auto* merge=new QPushButton("MERGE NEXT",page);edits->addWidget(split);edits->addWidget(merge);layout->addLayout(edits);
        _reloadSave=new QPushButton("SAVE RELOAD SETUP",page);_reloadSave->setObjectName("PrimaryButton");layout->addWidget(_reloadSave);
        auto* clips=new QLabel("NATIVE CLIPS",page);clips->setObjectName("Section");layout->addWidget(clips);
        auto* clipsHint=new QLabel("Inspect other native clips on the timeline. Selecting a phase or PLAY RELOAD returns to the reload.",page);clipsHint->setWordWrap(true);clipsHint->setObjectName("Hint");layout->addWidget(clipsHint);
        _animationList->setParent(page);_animationList->setMinimumHeight(140);_animationList->setMaximumHeight(220);_animationList->show();layout->addWidget(_animationList);
        _pages->addTab(page,"04  RELOAD");

        _reloadGraphDock=new QDockWidget("RELOAD GRAPH · gold = grabbable · blue = rigid timing · green = co-timed",_window);
        _reloadGraphDock->setObjectName("ReloadMakerReloadGraphDock");_reloadGraphDock->setAllowedAreas(Qt::BottomDockWidgetArea);
        auto* graphScroll=new QScrollArea(_reloadGraphDock);graphScroll->setWidgetResizable(true);_reloadGraph=new ReloadGraph(graphScroll);graphScroll->setWidget(_reloadGraph);
        _reloadGraphDock->setWidget(graphScroll);_window->addDockWidget(Qt::BottomDockWidgetArea,_reloadGraphDock);_reloadGraphDock->hide();

        _reloadInspectorDock=new QDockWidget("RELOAD WINDOW",_window);_reloadInspectorDock->setObjectName("ReloadMakerReloadInspectorDock");_reloadInspectorDock->setAllowedAreas(Qt::RightDockWidgetArea|Qt::LeftDockWidgetArea);_reloadInspectorDock->setMinimumWidth(330);
        auto* inspector=new QWidget(_reloadInspectorDock);auto* details=new QVBoxLayout(inspector);details->setContentsMargins(14,14,14,14);
        _reloadWindowName=new QLineEdit(inspector);_reloadWindowName->setMaxLength(128);_reloadWindowName->setPlaceholderText("Window label · e.g. Lift bolt / Remove magazine");details->addWidget(_reloadWindowName);
        _reloadWindowInfo=new QLabel(inspector);_reloadWindowInfo->setWordWrap(true);_reloadWindowInfo->setTextFormat(Qt::PlainText);details->addWidget(_reloadWindowInfo);
        details->addWidget(new QLabel("PARTS IN THIS WINDOW · CHECK = GRABBABLE",inspector));
        _reloadMembers=new QListWidget(inspector);_reloadMembers->setAlternatingRowColors(true);details->addWidget(_reloadMembers,1);
        _reloadPartName=new QLineEdit(inspector);_reloadPartName->setMaxLength(512);_reloadPartName->setPlaceholderText("Selected part label · does not rename the mesh");details->addWidget(_reloadPartName);
        details->addWidget(new QLabel("HAND FOR THE SELECTED PART",inspector));
        _reloadGripChoice=new QComboBox(inspector);_reloadGripChoice->setMinimumContentsLength(18);_reloadGripChoice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);details->addWidget(_reloadGripChoice);
        _reloadReturn=new QCheckBox("On release: return to this window's start",inspector);details->addWidget(_reloadReturn);
        auto* releaseHelp=new QLabel("Otherwise the part stays where released. A reload clip does not establish whether the real mechanism is spring-returning.",inspector);releaseHelp->setWordWrap(true);details->addWidget(releaseHelp);
        auto* files=new QPushButton("RELOAD SETUP FILES",inspector);details->addWidget(files);
        _reloadInspectorDock->setWidget(inspector);_window->addDockWidget(Qt::RightDockWidgetArea,_reloadInspectorDock);_reloadInspectorDock->hide();
        _reloadCaptureTimer=new QTimer(this);_reloadCaptureTimer->setInterval(0);
        _reloadPreviewTimer=new QTimer(this);_reloadPreviewTimer->setInterval(16);_reloadPreviewTimer->setTimerType(Qt::PreciseTimer);
        connect(_reloadCaptureTimer,&QTimer::timeout,this,&ReloadMakerController::advanceReloadCapture);
        connect(_reloadPreviewTimer,&QTimer::timeout,this,&ReloadMakerController::tickReloadPreview);
        connect(_pages,&QTabWidget::currentChanged,this,[this](int i){setReloadWorkspace(i==kReloadPage);});
        connect(_reloadAnalyze,&QPushButton::clicked,this,[this](){invalidateReloadCapture();_reloadIgnoreSavedOnce=true;startReloadCapture();});
        connect(_reloadPlay,&QPushButton::clicked,this,[this](){
            if(!_reloadRecipe||_reloadCapturing)return;
            restoreReloadClip();
            _reloadPlaying=!_reloadPlaying;_reloadHeld=false;_reloadReturning=false;_partGripDraft.reset();_reloadDrive->clearCurve();_reloadTest->setChecked(false);_reloadDrive->setActive(_reloadWorkspaceActive,false);
            if(_reloadPlaying){if(_playbackTime>=_reloadRecipe->duration)seekReload(0);_reloadClock.restart();_reloadPreviewTimer->start();}else _reloadPreviewTimer->stop();
            _reloadPlay->setText(_reloadPlaying?"PAUSE RELOAD":"PLAY RELOAD");
        });
        connect(_reloadTest,&QPushButton::toggled,this,[this](bool active){
            _reloadPlaying=false;_reloadReturning=false;_reloadHeld=false;_reloadPreviewTimer->stop();_reloadPlay->setText("PLAY RELOAD");
            if(_reloadDrive)_reloadDrive->setActive(_reloadWorkspaceActive,active&&_reloadRecipe&&_reloadWindow>=0&&!_reloadCapturing);
            if(active&&_reloadRecipe&&_reloadWindow>=0){const auto& w=_reloadRecipe->windows[_reloadWindow];const float first=_reloadRecipe->duration*w.first/(_reloadRecipe->frames-1),last=_reloadRecipe->duration*w.last/(_reloadRecipe->frames-1);if(_playbackTime<first||_playbackTime>last)seekReload(first);}
        });
        connect(_reloadWindows,&QListWidget::currentRowChanged,this,[this](int row){if(!_reloadUpdating)selectReloadWindow(row);});
        connect(_reloadMembers,&QListWidget::currentRowChanged,this,[this](){if(!_reloadUpdating)selectReloadMember();});
        connect(_reloadMembers,&QListWidget::itemChanged,this,[this](QListWidgetItem* item){
            if(_reloadUpdating||!_reloadRecipe||_reloadWindow<0){return;}
            const int track=item->data(Qt::UserRole).toInt();
            for(auto& member:_reloadRecipe->windows[_reloadWindow].members)if(member.track==track)member.grabbable=item->checkState()==Qt::Checked&&_reloadRecipe->tracks[track].surface;
            _reloadGraph->update();
        });
        connect(_reloadWindowName,&QLineEdit::editingFinished,this,[this](){
            if(_reloadUpdating||!_reloadRecipe||_reloadWindow<0||_reloadWindowName->text().trimmed().isEmpty())return;
            _reloadRecipe->windows[_reloadWindow].label=_reloadWindowName->text().trimmed();refreshReloadWindows();
        });
        connect(_reloadPartName,&QLineEdit::editingFinished,this,[this](){
            if(_reloadUpdating||!_reloadRecipe||_reloadTrack<0||_reloadPartName->text().trimmed().isEmpty())return;
            _reloadRecipe->tracks[_reloadTrack].label=_reloadPartName->text().trimmed();refreshReloadMembers();_reloadGraph->update();
        });
        connect(_reloadReturn,&QCheckBox::toggled,this,[this](bool value){if(!_reloadUpdating&&_reloadRecipe&&_reloadWindow>=0)_reloadRecipe->windows[_reloadWindow].returnOnRelease=value;});
        connect(_reloadGripChoice,qOverload<int>(&QComboBox::activated),this,[this](){
            if(!_reloadRecipe||_reloadWindow<0||_reloadTrack<0)return;
            for(auto& member:_reloadRecipe->windows[_reloadWindow].members)if(member.track==_reloadTrack)member.gripId=_reloadGripChoice->currentData().toString();
            applyReloadGrip();
        });
        connect(_reloadWindowProgress,&QSlider::sliderPressed,this,[this](){_reloadPlaying=false;_reloadReturning=false;_reloadPreviewTimer->stop();_reloadPlay->setText("PLAY RELOAD");});
        connect(_reloadWindowProgress,&QSlider::valueChanged,this,[this](int value){
            if(_reloadUpdating||!_reloadRecipe||_reloadWindow<0){return;}
            const auto& w=_reloadRecipe->windows[_reloadWindow];
            seekReload(_reloadRecipe->duration*(w.first+(w.last-w.first)*float(value)/10000)/(_reloadRecipe->frames-1));
        });
        connect(split,&QPushButton::clicked,this,[this](){
            if(!_reloadRecipe){return;}
            QString error;const int frame=qRound(_playbackTime/_reloadRecipe->duration*(_reloadRecipe->frames-1));
            if(!splitReloadWindow(*_reloadRecipe,_reloadWindow,frame,error)){setStatus(error,true);return;}refreshReloadWindows();selectReloadWindow(_reloadWindow);
        });
        connect(merge,&QPushButton::clicked,this,[this](){
            if(!_reloadRecipe){return;}
            QString error;if(!mergeReloadWindow(*_reloadRecipe,_reloadWindow,error)){setStatus(error,true);return;}refreshReloadWindows();selectReloadWindow(_reloadWindow);
        });
        connect(_reloadSave,&QPushButton::clicked,this,&ReloadMakerController::saveReloadRecipe);
        connect(files,&QPushButton::clicked,this,[](){const auto path=ReloadRecipeLibrary::defaultDirectory();if(QDir().mkpath(path))QDesktopServices::openUrl(QUrl::fromLocalFile(path));});
        _reloadGraph->selected=[this](int window,int track){selectReloadWindow(window,track);};
        _reloadGraph->sought=[this](float seconds){_reloadPlaying=false;_reloadReturning=false;_reloadPreviewTimer->stop();seekReload(seconds);};
        invalidateReloadCapture();
    }
    void ReloadMakerController::createReloadViewport()
    {
        _reloadDrive=new ReloadDriveViewport(_window->getGLView(),this);
        _reloadDrive->begin=[this](QPointF point,ReloadDriveCurve& curve){return beginReloadGrab(point,curve);};
        _reloadDrive->drive=[this](float time){seekReload(time);};
        _reloadDrive->release=[this](bool cancel){releaseReloadGrab(cancel);};
    }
    void ReloadMakerController::setReloadWorkspace(bool active)
    {
        if(_reloadWorkspaceActive==active)return;
        _reloadWorkspaceActive=active;
        if(active){
            _suspendedAuthoringGrip=_partGripDraft;
            restoreHandPosePreview();stopPlayback();
            // Dock visibility is owned by setWorkspace; phase editing only
            // forces the weapon-fixed frame and restores the user's choice after.
            _reloadOldWeaponFixed=_weaponFixedMotion;_weaponFixedMotion=true;showWeaponFixedMotion();
            _window->resizeDocks({_reloadGraphDock},{230},Qt::Vertical);
            if(_reloadDrive)_reloadDrive->setActive(true,false);
            startReloadCapture();
        }else{
            if(_reloadCapturing)invalidateReloadCapture();
            _reloadPreviewTimer->stop();_reloadPlaying=false;_reloadReturning=false;_reloadHeld=false;
            _partGripDraft=_suspendedAuthoringGrip;_suspendedAuthoringGrip.reset();
            if(_reloadDrive){_reloadDrive->setActive(false,false);_reloadDrive->clearCurve();}
            _weaponFixedMotion=_reloadOldWeaponFixed;showWeaponFixedMotion();
            if(_clip.valid())(void)applyPose(_playbackTime);
        }
        updateGripViewport();
    }
    void ReloadMakerController::refreshReloadWindows()
    {
        if(!_reloadRecipe||_reloadCapturing){return;}
        _reloadUpdating=true;const QSignalBlocker block(_reloadWindows);_reloadWindows->clear();
        const auto& r=*_reloadRecipe;
        for(const auto& w:r.windows)_reloadWindows->addItem(QStringLiteral("%1\n%2–%3 s · %4 parts").arg(w.label).arg(r.duration*w.first/(r.frames-1),0,'f',3).arg(r.duration*w.last/(r.frames-1),0,'f',3).arg(w.members.size()));
        if(_reloadWindow>=0&&_reloadWindow<r.windows.size())_reloadWindows->setCurrentRow(_reloadWindow);
        _reloadUpdating=false;_reloadGraph->setRecipe(&r);_reloadGraph->setSelection(_reloadWindow,_playbackTime);
        _reloadPlay->setEnabled(true);_reloadTest->setEnabled(!r.windows.isEmpty());_reloadSave->setEnabled(true);_reloadWindowProgress->setEnabled(!r.windows.isEmpty());
        _reloadSummary->setText(QStringLiteral("%1 phases · %2 native part tracks").arg(r.windows.size()).arg(r.tracks.size()));
    }
    void ReloadMakerController::selectReloadWindow(int window,int track)
    {
        if(!_reloadRecipe||_reloadCapturing||window<0||window>=_reloadRecipe->windows.size())return;
        restoreReloadClip();
        _reloadPreviewTimer->stop();_reloadPlaying=false;_reloadReturning=false;_reloadHeld=false;_reloadPlay->setText("PLAY RELOAD");_reloadDrive->clearCurve();_partGripDraft.reset();
        _reloadWindow=window;_reloadTrack=track;{const QSignalBlocker b(_reloadWindows);_reloadWindows->setCurrentRow(window);}
        refreshReloadMembers();const auto& w=_reloadRecipe->windows[window];seekReload(_reloadRecipe->duration*w.first/(_reloadRecipe->frames-1));
        _reloadDrive->setActive(_reloadWorkspaceActive,_reloadTest->isChecked());
    }
    void ReloadMakerController::refreshReloadMembers()
    {
        if(!_reloadRecipe||_reloadWindow<0){return;}
        _reloadUpdating=true;
        const QSignalBlocker a(_reloadMembers),b(_reloadWindowName),c(_reloadReturn);_reloadMembers->clear();
        const auto& r=*_reloadRecipe;const auto& w=r.windows[_reloadWindow];_reloadWindowName->setText(w.label);_reloadReturn->setChecked(w.returnOnRelease);
        _reloadWindowName->setEnabled(true);_reloadGripChoice->setEnabled(true);_reloadReturn->setEnabled(true);
        for(const auto& m:w.members){
            const auto& t=r.tracks[m.track];const QString relation=m.relation==ReloadRelation::Independent?"lead path":m.relation==ReloadRelation::Rigid?"rigid relation":"co-timed · own path";
            auto* item=new QListWidgetItem(t.label+"\n"+relation,_reloadMembers);item->setData(Qt::UserRole,m.track);item->setFlags(item->flags()|Qt::ItemIsUserCheckable);item->setCheckState(m.grabbable?Qt::Checked:Qt::Unchecked);
            if(!t.surface)item->setFlags(item->flags()&~Qt::ItemIsUserCheckable);
            item->setToolTip(t.anchor.ownerRecord+"\n"+t.anchor.ownerModel+"\n"+t.anchor.anchorPath+(t.surface?QString():"\nNo directly pickable geometry; timing evidence only."));
            if(m.track==_reloadTrack)_reloadMembers->setCurrentItem(item);
        }
        if(!_reloadMembers->currentItem()){int selected=0;for(int i=0;i<w.members.size();++i)if(w.members[i].grabbable){selected=i;if(w.members[i].relation==ReloadRelation::Independent)break;}_reloadMembers->setCurrentRow(selected);}
        _reloadUpdating=false;selectReloadMember();
    }
    void ReloadMakerController::selectReloadMember()
    {
        if(!_reloadRecipe||_reloadWindow<0||!_reloadMembers->currentItem())return;
        _reloadTrack=_reloadMembers->currentItem()->data(Qt::UserRole).toInt();const auto& track=_reloadRecipe->tracks[_reloadTrack];
        {const QSignalBlocker block(_reloadPartName);_reloadPartName->setText(track.label);_reloadPartName->setEnabled(true);}
        QString selected;for(const auto& m:_reloadRecipe->windows[_reloadWindow].members)if(m.track==_reloadTrack)selected=m.gripId;
        {const QSignalBlocker block(_reloadGripChoice);_reloadGripChoice->clear();_reloadGripChoice->addItem("Use the authored reload hand animation",QString());
            for(const auto& grip:_partGrips)if(grip.weaponRecord.compare(_reloadRecipe->source["weaponRecord"].toString(),Qt::CaseInsensitive)==0&&sameMechanismPart(grip.pose.placement,track.anchor))_reloadGripChoice->addItem(grip.pose.name,grip.pose.id);
            const int found=_reloadGripChoice->findData(selected);if(found>=0)_reloadGripChoice->setCurrentIndex(found);
            else if(!selected.isEmpty()){_reloadGripChoice->addItem("Saved grip missing — choose a replacement",selected);_reloadGripChoice->setCurrentIndex(_reloadGripChoice->count()-1);}
        }
        _reloadWindowInfo->setText(QStringLiteral("%1\nNo manual axis or endpoint is needed. TEST WINDOW lets you pull along the reload's actual path. Check only the parts that should accept a grab.").arg(track.label));
        applyReloadGrip();
    }
    void ReloadMakerController::applyReloadGrip()
    {
        _partGripDraft.reset();if(!_reloadRecipe||_reloadTrack<0)return;
        const QString id=_reloadGripChoice->currentData().toString();if(id.isEmpty())return;
        const auto grip=std::find_if(_partGrips.cbegin(),_partGrips.cend(),[&](const auto& g){return g.pose.id==id;});
        if(grip==_partGrips.cend()){setStatus("The saved grip for this reload part is missing. Choose a replacement or the authored reload hands.",true);return;}
        QString error;const int anchor=resolveMechanismAnchor(_reloadRecipe->tracks[_reloadTrack].anchor,error);
        if(anchor<0||!preparePartGripFrame(anchor,error)){setStatus(error,true);return;}
        _partGripDraft=*grip;if(_reloadHeld)seekReload(_playbackTime);
    }
    void ReloadMakerController::seekReload(float seconds)
    {
        if(!_reloadWorkspaceActive||!_reloadRecipe||_reloadCapturing||!_clip.valid()||_activeAnimationPath!=_reloadExpectedPath)return;
        _playbackTime=std::clamp(seconds,0.f,_reloadRecipe->duration);
        if(!applyPose(_playbackTime)){invalidateReloadCapture();return;}
        if(_reloadHeld&&_partGripDraft&&!_gripPreviewValid)_reloadWindowInfo->setText("Saved hand cannot reach this target: "+_gripError+"\nThe source hand is shown; part motion remains on the authored path.");
        _reloadGraph->setSelection(_reloadWindow,_playbackTime);_reloadDrive->setTime(_playbackTime);
        if(_reloadWindow>=0){const auto& w=_reloadRecipe->windows[_reloadWindow];const float frame=_playbackTime/_reloadRecipe->duration*(_reloadRecipe->frames-1);const QSignalBlocker block(_reloadWindowProgress);_reloadWindowProgress->setValue(qRound(std::clamp((frame-w.first)/(w.last-w.first),0.f,1.f)*10000));}
    }
    bool ReloadMakerController::beginReloadGrab(QPointF point,ReloadDriveCurve& curve)
    {
        if(!_reloadWorkspaceActive||!_reloadRecipe||_reloadCapturing||_reloadWindow<0||!_reloadTest->isChecked())return false;
        int automatic=-1;Vec3f hit;QString surface,error;QVector<int> candidates;
        if(!pickWeaponPart(point,automatic,hit,surface,candidates,error))return false;
        const auto& w=_reloadRecipe->windows[_reloadWindow];QVector<int> eligible;
        for(const auto& member:w.members)if(member.grabbable)for(int anchor:candidates)if(sameMechanismPart(_handPoseAnchors[anchor].description,_reloadRecipe->tracks[member.track].anchor)&&!eligible.contains(member.track))eligible.append(member.track);
        int track=-1;if(eligible.contains(_reloadTrack))track=_reloadTrack;else if(eligible.size()==1)track=eligible[0];
        if(track<0){setStatus(eligible.isEmpty()?"This part is not marked grabbable in the selected reload window.":"This mesh has several moving frames. Select the intended part in RELOAD WINDOW first.",true);return true;}
        const float first=_reloadRecipe->duration*w.first/(_reloadRecipe->frames-1),last=_reloadRecipe->duration*w.last/(_reloadRecipe->frames-1);
        if(_playbackTime<first||_playbackTime>last){setStatus("Select the window again to return to its start before grabbing.",true);return true;}
        Transformf part,inverse;
        if(!sceneNodeWorld(_sceneNodes[_reloadTrackNodes[track]],part,error)||!invertUniformTransform(part,inverse)){setStatus(error,true);return true;}
        const auto pointInPart=composed(inverse,Transformf{hit}).translation;
        _reloadTrack=track;for(int row=0;row<_reloadMembers->count();++row)if(_reloadMembers->item(row)->data(Qt::UserRole).toInt()==track)_reloadMembers->setCurrentRow(row);
        _reloadPlaying=false;_reloadReturning=false;_reloadPreviewTimer->stop();_reloadHeld=true;_reloadGrabStart=_playbackTime;
        curve.current=_playbackTime;
        for(int frame=w.first;frame<=w.last;++frame){const auto p=composed(_reloadRecipe->tracks[track].poses[frame],Transformf{pointInPart}).translation;curve.points.append({p.x,p.y,p.z});curve.times.append(_reloadRecipe->duration*frame/(_reloadRecipe->frames-1));}
        applyReloadGrip();
        if(!_reloadGripChoice->currentData().toString().isEmpty()&&!_partGripDraft){_reloadHeld=false;curve={};return true;}
        float arc=0;for(int i=1;i<curve.points.size();++i)arc+=(curve.points[i]-curve.points[i-1]).length();
        if(arc<.001f)setStatus("This point lies on the motion's pivot. Use the window slider, or grab a point farther from the pivot.");
        seekReload(_playbackTime);return true;
    }
    void ReloadMakerController::releaseReloadGrab(bool cancel)
    {
        if(!_reloadRecipe||_reloadWindow<0)return;
        _reloadHeld=false;_reloadReturning=false;
        if(cancel)seekReload(_reloadGrabStart);
        else{seekReload(_playbackTime);if(_reloadRecipe->windows[_reloadWindow].returnOnRelease){_reloadReturning=true;_reloadClock.restart();_reloadPreviewTimer->start();}}
    }
    void ReloadMakerController::tickReloadPreview()
    {
        const QScopedValueRollback<bool> viewOnly(_viewRefresh, true);
        if(!_reloadRecipe||!_reloadWorkspaceActive){_reloadPreviewTimer->stop();return;}
        const float delta=float(_reloadClock.nsecsElapsed())/1e9f;_reloadClock.restart();
        if(_reloadReturning&&_reloadWindow>=0){const float start=_reloadRecipe->duration*_reloadRecipe->windows[_reloadWindow].first/(_reloadRecipe->frames-1);seekReload(std::max(start,_playbackTime-delta));if(_playbackTime<=start)_reloadReturning=false;}
        else if(_reloadPlaying){const auto previous=_playbackTime;seekReload(_playbackTime+delta);playSourceSounds(_reloadClip,previous,_playbackTime);if(_reloadRecipe&&_playbackTime>=_reloadRecipe->duration){_reloadPlaying=false;_reloadPlay->setText("PLAY RELOAD");}}
        if(!_reloadPlaying&&!_reloadReturning)_reloadPreviewTimer->stop();
    }
    // Empties the reload page after the recipe is dropped.
    void ReloadMakerController::resetReloadPage()
    {
        if(_reloadWindows){const QSignalBlocker block(_reloadWindows);_reloadWindows->clear();}
        if(_reloadMembers){const QSignalBlocker block(_reloadMembers);_reloadMembers->clear();}
        if(_reloadPlay){_reloadAnalyze->setEnabled(true);_reloadPlay->setEnabled(false);_reloadPlay->setText("PLAY RELOAD");_reloadTest->setChecked(false);_reloadTest->setEnabled(false);_reloadSave->setEnabled(false);_reloadWindowProgress->setEnabled(false);_reloadProgress->hide();_reloadSummary->setText("Select a configured weapon. Its base Reload is the only motion source for this page.");}
        if(_reloadWindowName){_reloadWindowName->clear();_reloadWindowName->setEnabled(false);_reloadPartName->clear();_reloadPartName->setEnabled(false);_reloadWindowInfo->setText("Select a reload movement window to label its parts.");_reloadGripChoice->clear();_reloadGripChoice->setEnabled(false);_reloadReturn->setEnabled(false);}
    }
    void ReloadMakerController::showReloadCaptureStarted(const QString& expected)
    {
        _reloadSourceLabel->setText(QStringLiteral("Native reload · %1").arg(QFileInfo(QFileInfo(expected).path()).fileName()));_reloadSourceLabel->setToolTip(expected+"\n"+(_libraryMode?_packageResources.provider(expected):_package.sourcePath));
        _reloadAnalyze->setEnabled(false);_reloadProgress->setRange(0,_reloadRecipe->frames);_reloadProgress->setValue(0);_reloadProgress->show();_reloadSummary->setText("Sampling native weapon-part poses…");
    }
}
