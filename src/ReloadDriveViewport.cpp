#include "ReloadDriveViewport.h"
#include "ReloadDriveMath.h"
#include "glview.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLPaintDevice>
#include <QPainter>
#include <limits>

namespace reloadmaker {
    ReloadDriveViewport::ReloadDriveViewport(GLView* view,QObject* parent):QObject(parent),_view(view){_view->installEventFilter(this);connect(_view,&GLView::paintUpdate,this,[this](){paint();});}
    void ReloadDriveViewport::setActive(bool active,bool canDrive){_active=active;_canDrive=canDrive;if(!active||!canDrive)_dragging=false;_view->update();}
    void ReloadDriveViewport::clearCurve(){_curve={};_projected.clear();_dragging=false;_view->update();}
    void ReloadDriveViewport::setTime(float time){_curve.current=time;_view->update();}
    bool ReloadDriveViewport::eventFilter(QObject* watched,QEvent* event){
        if(watched!=_view){return false;}
        const auto type=event->type();
        if(type==QEvent::Leave||type==QEvent::FocusOut||type==QEvent::WindowDeactivate){_markers.clear();if(hoverLeave)hoverLeave();_view->update();}
        if(type==QEvent::FocusOut||type==QEvent::WindowDeactivate||type==QEvent::UngrabMouse){if(_dragging&&release)release(true);_dragging=false;(void)_gesture.consume(type,Qt::NoButton,Qt::NoButton);return false;}
        if(type==QEvent::Wheel&&_gesture.captured())return true;
        if(type==QEvent::KeyPress&&_dragging&&static_cast<QKeyEvent*>(event)->key()==Qt::Key_Escape){_dragging=false;if(release)release(true);return true;}
        if(type!=QEvent::MouseButtonPress&&type!=QEvent::MouseButtonDblClick&&type!=QEvent::MouseMove&&type!=QEvent::MouseButtonRelease)return false;
        auto* mouse=static_cast<QMouseEvent*>(event);bool hit=false;
        if(_active&&hover&&type==QEvent::MouseMove&&mouse->buttons()==Qt::NoButton){const auto point=hover(mouse->position());_markers=point?QVector<QVector3D>{*point}:QVector<QVector3D>{};_view->update();}
        const bool press=(type==QEvent::MouseButtonPress||type==QEvent::MouseButtonDblClick)&&mouse->button()==Qt::LeftButton&&!_gesture.captured();
        if(_active&&_canDrive&&press&&begin){
            ReloadDriveCurve curve;hit=begin(mouse->position(),curve);
            if(hit){_curve=std::move(curve);_dragging=_curve.points.size()>1&&_curve.times.size()==_curve.points.size();_projected.clear();
                const auto camera=_view->reloadMakerWorldToClip();const QSizeF size(_view->width(),_view->height());
                for(const auto& p:_curve.points){QPointF screen;if(!projectGripPoint(camera,size,p,screen))screen={std::numeric_limits<double>::quiet_NaN(),0};_projected.append(screen);}
            }
        }
        if(!_gesture.consume(type,mouse->button(),mouse->buttons(),hit))return false;
        if(_dragging&&type==QEvent::MouseMove&&mouse->buttons().testFlag(Qt::LeftButton)){
            _curve.current=reloadCurveTime(_projected,_curve.times,mouse->position(),_curve.current,_curve.endpointSnapFraction);if(drive)drive(_curve.current);
        }
        if(type==QEvent::MouseButtonRelease||mouse->buttons()==Qt::NoButton){if(_dragging&&release)release(false);_dragging=false;}
        return true;
    }
    void ReloadDriveViewport::paint(){
        if(!_active||!_view->glContext)return;
        const auto dpr=_view->devicePixelRatioF();QOpenGLPaintDevice device(QSize(qRound(_view->width()*dpr),qRound(_view->height()*dpr)));device.setDevicePixelRatio(dpr);
        QPainter p(&device);p.setRenderHint(QPainter::Antialiasing);p.setPen(QColor("#f0c88a"));
        p.drawText(QRectF(18,14,_view->width()-36,42),Qt::AlignLeft|Qt::TextWordWrap,!_instructions.isEmpty()?_instructions:_canDrive?"RELOAD DRIVE · grab a marked part and follow its source path · release holds or returns within this window · background drag orbits":"RELOAD SOURCE · choose a movement window below, then TEST WINDOW to grab its parts");
        const auto camera=_view->reloadMakerWorldToClip();const QSizeF size(_view->width(),_view->height());QPointF previous;bool valid=false;
        p.setPen(QPen(QColor("#87dfce"),2));p.setBrush(QColor(15,45,42,110));
        for(const auto& marker:_markers){QPointF screen;if(projectGripPoint(camera,size,marker,screen)){p.drawEllipse(screen,9,9);p.drawEllipse(screen,2,2);}}
        if(_curve.points.size()<2)return;
        p.setPen(QPen(QColor("#e6ba76"),2,Qt::DashLine));
        for(const auto& world:_curve.points){QPointF point;if(!projectGripPoint(camera,size,world,point)){valid=false;continue;}if(valid)p.drawLine(previous,point);previous=point;valid=true;}
    }
}
