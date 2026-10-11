#include "MagazineViewport.h"
#include "glview.h"
#include "design/Theme.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QOpenGLPaintDevice>
#include <QPainter>

namespace reloadmaker {
MagazineViewport::MagazineViewport(GLView* view,QObject* parent):QObject(parent),_view(view) {
    _view->installEventFilter(this);connect(_view,&GLView::paintUpdate,this,[this]{paint();});
}
void MagazineViewport::setActive(bool active){_active=active;if(!active){_planeReady=_withdrawalReady=_withdrawalGesture=false;}}
bool MagazineViewport::eventFilter(QObject* watched,QEvent* event) {
    if(watched!=_view)return false;
    if(_active&&state&&event->type()==QEvent::MouseButtonPress){
        const auto* mouse=static_cast<QMouseEvent*>(event);
        if(mouse->button()==Qt::LeftButton&&state().seated()&&acquire&&acquire(mouse->position())){
            _withdrawalReady=false;_withdrawalGesture=true;updatePointer(mouse->position());return true;
        }
    }
    const bool carried=_active&&state&&state().carried();
    if(!carried&&!_gesture.captured())return false;
    const auto type=event->type();
    if(type==QEvent::MouseButtonRelease&&_withdrawalGesture&&static_cast<QMouseEvent*>(event)->button()==Qt::LeftButton){
        _withdrawalGesture=_withdrawalReady=false;if(cancel)cancel();return true;
    }
    if(type==QEvent::WindowDeactivate){if(carried&&cancel)cancel();_planeReady=false;(void)_gesture.consume(type,Qt::NoButton,Qt::NoButton);return false;}
    if(type==QEvent::KeyPress&&static_cast<QKeyEvent*>(event)->key()==Qt::Key_Escape){if(carried&&cancel)cancel();_planeReady=false;return true;}
    if(type==QEvent::Wheel&&carried){const auto* e=static_cast<QWheelEvent*>(event);updatePointer(e->position(),float(e->angleDelta().y())/120);return true;}
    if(type==QEvent::MouseMove||type==QEvent::MouseButtonPress||type==QEvent::MouseButtonDblClick||type==QEvent::MouseButtonRelease) {
        const auto* mouse=static_cast<QMouseEvent*>(event);
        // Seating may finish in the middle of a held pointer sequence. Own its
        // remaining moves/release so GLView cannot interpret them as orbit.
        const bool owned=_gesture.consume(type,mouse->button(),mouse->buttons(),carried);
        if(type==QEvent::MouseMove&&carried)updatePointer(mouse->position());
        return carried||owned;
    }
    return false;
}
void MagazineViewport::updatePointer(QPointF point,float wheel) {
    const auto r=rail?rail():std::nullopt;if(!r)return;
    const auto before=state();const auto camera=_view->reloadMakerWorldToClip();const QSizeF size(_view->width(),_view->height());
    GripRay ray,center;
    if(!gripRay(camera,size,point,ray)||!gripRay(camera,size,{size.width()/2,size.height()/2},center))return;
    if(before.mountedHold||before.extracting){
        float distance=0;if(!gripAxisParameter(ray,r->handle,r->outward,distance))return;
        if(!_withdrawalReady){_withdrawalOrigin=distance;_withdrawalReady=true;return;}
        if(railMove)railMove(distance-_withdrawalOrigin);
        if(state().phase==MagazinePhase::Free)_planeReady=false;
    } else if(before.phase==MagazinePhase::Free) {
        if(!_planeReady){_planePoint=r->handle+before.offset;_planeNormal=center.direction;_planeReady=true;}
        _planePoint+=_planeNormal*(wheel*r->captureRadius*.5f);
        QVector3D world;if(gripPlanePoint(ray,_planePoint,_planeNormal,world)&&freeMove)freeMove(world-r->handle);
    } else if(before.phase==MagazinePhase::Rail) {
        float distance=before.distance;
        if(wheel)distance-=wheel*r->captureRadius*.5f;
        else if(!gripAxisParameter(ray,r->handle,r->outward,distance))return;
        if(railMove)railMove(distance);
        if(state().phase==MagazinePhase::Free)_planeReady=false;
    }
}
void MagazineViewport::paint() {
    if(!_active||!state||!rail||!_view->glContext)return;
    const auto r=rail();const auto s=state();if(!r||(s.seated()&&!s.mountedHold)||s.phase==MagazinePhase::Absent)return;
    const auto dpr=_view->devicePixelRatioF();QOpenGLPaintDevice device(QSize(qRound(_view->width()*dpr),qRound(_view->height()*dpr)));device.setDevicePixelRatio(dpr);
    QPainter painter(&device);painter.setRenderHint(QPainter::Antialiasing);
    const auto camera=_view->reloadMakerWorldToClip();const QSizeF size(_view->width(),_view->height());
    QPointF seated,entrance;
    if(projectGripPoint(camera,size,r->handle,seated)&&projectGripPoint(camera,size,r->handle+magazineInputOffset(*r,r->travel),entrance)) {
        painter.setPen(QPen(QColor("#84d9c2"),2,Qt::DashLine));painter.drawLine(seated,entrance);
        painter.setBrush(Qt::NoBrush);painter.drawEllipse(entrance,12,12);painter.drawEllipse(seated,5,5);
        design::paintViewportHint(painter,entrance+QPointF(18,-12),280,s.phase==MagazinePhase::Rail?QStringLiteral("Guided · move straight toward the weapon"):QStringLiteral("Align anywhere along this line"));
    }
    if(s.carried()) {
        design::paintViewportHint(painter,{16,14},_view->width()-32,(s.mountedHold||s.extracting)?QStringLiteral("Magazine held · pull outward to remove it when unlocked · release to let go") :s.phase==MagazinePhase::Free?QStringLiteral("Magazine in hand · move the mouse · the wheel adjusts depth · Esc cancels")
            :QStringLiteral("On the rail · move toward the weapon to seat it · pull past the entrance to let go · Esc cancels"),true);
    }
}
}
