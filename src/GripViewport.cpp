#include "GripViewport.h"
#include "design/Theme.h"
#include "glview.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLPaintDevice>
#include <QPainter>
#include <algorithm>
#include <cmath>

namespace reloadmaker {
    namespace {
        QVector3D v(Vec3f p) { return {p.x,p.y,p.z}; }
        Vec3f vec(QVector3D p) { return {p.x(),p.y(),p.z()}; }
        QVector3D palmWorld(const Transformf& hand,Vec3f palm) { return v(composed(hand,Transformf{palm}).translation); }
        float lineDistance(QPointF p,QPointF a,QPointF b) {
            const auto d=b-a; const auto n=d.x()*d.x()+d.y()*d.y(); if(n<.01) return float(QLineF(p,a).length());
            const auto t=std::clamp(((p.x()-a.x())*d.x()+(p.y()-a.y())*d.y())/n,0.,1.);
            return float(QLineF(p,a+d*t).length());
        }
        // Ctrl+drag steps of a placement edit (not of Operate drags, which test
        // the mechanism itself).
        constexpr float kSnapDegrees=15.f;
        constexpr float kSnapUnits=.5f;
        float snapped(float value,float step){return std::round(value/step)*step;}
        // Opacity of handle parts behind the weapon or the hands. The handles
        // start at the palm, inside the hand being placed, so they stay usable.
        constexpr float kHiddenHandleAlpha=.55f;
    }
    GripViewport::GripViewport(GLView* view,QObject* parent):QObject(parent),_view(view) {
        _view->installEventFilter(this);
        connect(_view,&GLView::paintUpdate,this,[this](){paint();});
    }
    void GripViewport::configure(bool active,bool canPick,bool canSave) {
        _active=active; _canPick=canPick; _canSave=canSave;
        if(!active) { _picking=false; _dragAxis=-1; _handles.clear(); _buttons.clear(); }
        _view->setCursor(_active && _picking?Qt::CrossCursor:Qt::ArrowCursor); _view->update();
    }
    void GripViewport::setGrip(const Transformf& reference,const Transformf& hand,Vec3f palm,std::optional<Vec3f> surface) {
        _reference=reference; _hand=hand; _palm=palm; _surface=surface; _hasGrip=true; _handles.clear(); _view->update();
    }
    void GripViewport::clearGrip() { _hasGrip=false; if(!_simulating)_dragAxis=-1; _handles.clear(); _view->update(); }
    void GripViewport::setSimulation(bool active,QVector3D pivot,QVector3D axis,bool rotation,int axisIndex) {
        if(_simulating!=active)_dragAxis=-1;
        _simulating=active;_simulationPivot=pivot;_simulationAxis=axis;_simulationRotation=rotation;
        _simulationAxisIndex=std::clamp(axisIndex,0,2);
        if(active)_picking=false;
    }
    void GripViewport::setPicking(bool picking) {
        _picking=picking; _view->setCursor(_picking?Qt::CrossCursor:Qt::ArrowCursor); _view->update();
    }
    void GripViewport::setTeaching(bool active,const Transformf& frame){
        if(_teaching!=active)_dragAxis=-1;
        _teaching=active;_teachingFrame=frame;
    }
    void GripViewport::setWorkshop(bool active,const QString& name,bool handEditing){_workshop=active;_partName=name;_workshopHand=handEditing;}
    void GripViewport::setRotationTool(bool rotation){_rotate=rotation;_simulationRotation=rotation;_view->update();}
    void GripViewport::finishDrag(bool restore) {
        const bool dragging=_dragAxis>=0; _dragAxis=-1;
        if(dragging&&_simulating){if(restore){if(simulationCancel)simulationCancel();}else if(simulationRelease)simulationRelease();return;}
        if(restore && dragging && edited) edited(_dragHand);
    }
    bool GripViewport::beginDrag(Handle handle,QPointF point) {
        _dragCamera=_view->reloadMakerWorldToClip(); _dragSize=QSizeF(_view->width(),_view->height());
        GripRay ray; if(!gripRay(_dragCamera,_dragSize,point,ray)) return false;
        _dragPivot=_simulating?_simulationPivot:palmWorld(_hand,_palm); _dragDirection=handle.direction; _dragRotate=handle.rotate;
        if(handle.rotate) {
            QVector3D plane; if(!gripPlanePoint(ray,_dragPivot,_dragDirection,plane) || (plane-_dragPivot).lengthSquared()<1e-8f) return false;
            _dragRadial=(plane-_dragPivot).normalized();
        } else if(handle.axis==3) {
            if(!gripPlanePoint(ray,_dragPivot,_dragDirection,_dragRadial)) return false;
        } else if(!gripAxisParameter(ray,_dragPivot,_dragDirection,_dragParameter)) return false;
        _dragHand=_hand; _dragAngle=0; _dragLastAngle=0; _dragAxis=handle.axis; return true;
    }
    void GripViewport::drag(QPointF point) {
        GripRay ray; if(_dragAxis<0 || (!_simulating&&!edited) || !gripRay(_dragCamera,_dragSize,point,ray)) return;
        Transformf hand=_dragHand;
        if(_dragRotate) {
            QVector3D plane; if(!gripPlanePoint(ray,_dragPivot,_dragDirection,plane) || (plane-_dragPivot).lengthSquared()<1e-8f) return;
            const auto radial=(plane-_dragPivot).normalized();
            const float angle=std::atan2(QVector3D::dotProduct(_dragDirection,QVector3D::crossProduct(_dragRadial,radial)),QVector3D::dotProduct(_dragRadial,radial))*180.f/3.14159265358979323846f;
            if(_simulating){
                _dragAngle+=std::remainder(angle-_dragLastAngle,360.f);_dragLastAngle=angle;
                if(simulationDrag){simulationDrag(_dragAngle*_dragPrecision);}
                return;
            }
            hand=rotateGripAbout(_dragHand,vec(_dragPivot),vec(_dragDirection),_dragSnap?snapped(angle,kSnapDegrees):angle*_dragPrecision);
        } else {
            QVector3D delta;
            if(_dragAxis==3) {
                QVector3D plane; if(!gripPlanePoint(ray,_dragPivot,_dragDirection,plane)) return; delta=plane-_dragRadial;
                if(_dragSnap)delta={snapped(delta.x(),kSnapUnits),snapped(delta.y(),kSnapUnits),snapped(delta.z(),kSnapUnits)};
            } else {
                float parameter; if(!gripAxisParameter(ray,_dragPivot,_dragDirection,parameter)) return;
                if(_simulating){if(simulationDrag)simulationDrag((parameter-_dragParameter)*_dragPrecision);return;}
                delta=_dragDirection*(_dragSnap?snapped(parameter-_dragParameter,kSnapUnits):parameter-_dragParameter);
            }
            hand.translation=vec(v(_dragHand.translation)+delta*(_dragSnap?1.f:_dragPrecision));
        }
        edited(hand);
    }
    bool GripViewport::eventFilter(QObject* watched,QEvent* event) {
        if(watched!=_view) return false;
        const auto type=event->type();
        if(type==QEvent::Wheel && _gesture.captured()) return true;
        if(type==QEvent::KeyPress && _active && static_cast<QKeyEvent*>(event)->key()==Qt::Key_Escape) {
            if(_dragAxis>=0) finishDrag(true); else {setPicking(false);if(pickingCancelled)pickingCancelled();} return true;
        }
        if(type==QEvent::FocusOut || type==QEvent::WindowDeactivate || type==QEvent::UngrabMouse) {
            finishDrag(true); setPicking(false);if(pickingCancelled)pickingCancelled(); (void)_gesture.consume(type,Qt::NoButton,Qt::NoButton); return false;
        }
        if(type!=QEvent::MouseButtonPress && type!=QEvent::MouseButtonDblClick && type!=QEvent::MouseMove && type!=QEvent::MouseButtonRelease) return false;
        const auto* mouse=static_cast<QMouseEvent*>(event); const auto position=mouse->position();
        int button=-1,handle=-1;
        bool simulationPart=false;
        const bool press=(type==QEvent::MouseButtonPress || type==QEvent::MouseButtonDblClick) && mouse->button()==Qt::LeftButton && !_gesture.captured();
        if(_active && press) {
            for(int i=0;i<_buttons.size();++i) if(_buttons[i].contains(position)) {button=i;break;}
            if(button<0 && !_picking) {
                float nearest=9;
                for(int i=0;i<_handles.size();++i) for(int segment=1;segment<_handles[i].line.size();++segment) {
                    const auto d=lineDistance(position,_handles[i].line[segment-1],_handles[i].line[segment]);
                    if(d<nearest) {nearest=d;handle=i;}
                }
            }
        }
        if(_active&&_simulating&&!_teaching&&press&&button<0&&handle<0&&simulationHit)simulationPart=simulationHit(position);
        if(!_gesture.consume(type,mouse->button(),mouse->buttons(),_active && (button>=0 || handle>=0 || _picking || simulationPart))) return false;
        if(press && _active) {
            if(_simulating){
                if(_teaching&&button>=0){
                    if(button==0){if(beginPick)beginPick();}
                    else if(button==1||button==2){if(teachingTool)teachingTool(button==2);}
                    else if(button==3){if(_canSave&&save)save();}
                    else if(button==4){if(test)test();}
                }
                else if(button==0){if(simulationReset)simulationReset();}
                else if(button==3){if(simulationRelease)simulationRelease();}
                else if(button==4){if(simulationStop)simulationStop();}
                else if(button<0&&(handle>=0||simulationPart)){
                    const Handle chosen=handle>=0?_handles[handle]:Handle{0,_simulationAxis,{},_simulationRotation};
                    if(_teaching){if(teachingGrab)teachingGrab(chosen.axis,chosen.rotate);}
                    else if(!simulationPart&&simulationGrab)simulationGrab();
                    _dragPrecision=mouse->modifiers().testFlag(Qt::ShiftModifier)?.1f:1.f;
                    if(!beginDrag(chosen,position)&&simulationRelease)simulationRelease();
                }
                return true;
            }
            if(button==0) {if(_canPick && beginPick) beginPick();}
            else if(button==1 || (button==2&&!_pointAuthoring)) {setPicking(false);_rotate=button==2;_view->update();}
            else if(button==3) {if(_canSave && save) save();}
            else if(button==4) {if(end) end();}
            else if(_picking) {if(surfacePicked) surfacePicked(position);}
            else if(handle>=0) {_dragPrecision=mouse->modifiers().testFlag(Qt::ShiftModifier)?.1f:1.f;(void)beginDrag(_handles[handle],position);}
        } else if(type==QEvent::MouseMove && _active && _dragAxis>=0 && mouse->buttons().testFlag(Qt::LeftButton)) {
            _dragSnap=!_simulating&&mouse->modifiers().testFlag(Qt::ControlModifier);drag(position);
        }
        if(type==QEvent::MouseButtonRelease || mouse->buttons()==Qt::NoButton) finishDrag(false);
        return true;
    }
    void GripViewport::paint() {
        _buttons.clear(); _handles.clear();
        if(!_active || !_view->glContext) return;
        const QSizeF size(_view->width(),_view->height()); if(size.width()<300 || size.height()<180) return;
        const auto camera=_view->reloadMakerWorldToClip(); const auto dpr=_view->devicePixelRatioF();
        QOpenGLPaintDevice device(QSize(qRound(size.width()*dpr),qRound(size.height()*dpr))); device.setDevicePixelRatio(dpr);
        QPainter painter(&device); painter.setRenderHint(QPainter::Antialiasing);
        painter.setFont(design::uiFont(design::kSmallPx, QFont::Medium));
        const QString labels[]{_lensAuthoring?"Pick lens":_teaching?"Pick part":_simulating?"Reset":_workshop?(_workshopHand?"Place palm":"Pick part"):"Pick point",
            _simulating&&!_teaching?"Drag part":"Move",_simulating&&!_teaching?"Staged":"Rotate",
            _lensAuthoring?"Save scope":_simulating&&!_teaching?"Release":_pointAuthoring?"Save points":_workshop?"Save":"Save grip",_teaching?"Test":_simulating?"Edit":"End"};
        // Canvas viewport chips: translucent dark, amber when active.
        qreal x=16;
        for(int i=0;i<5;++i) {
            const bool selected=(i==0 && _picking) || (i==1 && !_picking && !_rotate) || (i==2 && !_picking && _rotate);
            const bool enabled=_simulating&&!_teaching?true:i==0?_canPick:i==3?_canSave:!(i==2&&_pointAuthoring);
            const qreal width=painter.fontMetrics().horizontalAdvance(labels[i])+24;
            const QRectF rect(x,14,width,30); _buttons.append(rect); x+=width+6;
            painter.setPen(QPen(design::qcolor(selected?design::color::selection:design::color::borderStrong),selected?1.5:1));
            painter.setBrush(selected?design::qcolor(design::color::selectionFill):design::qcolor(design::color::header,217)); painter.drawRoundedRect(rect,6,6);
            painter.setPen(design::qcolor(!enabled?design::color::textDisabled:selected?0xfff6e4c6:design::color::textSecondary)); painter.drawText(rect,Qt::AlignCenter,labels[i]);
        }
        const QString hint=_teaching?QStringLiteral("Part: %1 · drag arrows to set travel, rings to set rotation · Save keeps the part and hand · Test checks grab and release").arg(_partName):
            _simulating?QStringLiteral("Testing %1 · grab or drag its handle · releasing tests its rule · Edit returns to its endpoints").arg(_partName):_picking?QStringLiteral("Click the weapon surface · Esc cancels"):
            QStringLiteral("Arrows move · rings rotate · the centre square moves in the screen plane · Shift+drag for precision · Ctrl+drag snaps to 15° / 0.5 units · background drag orbits");
        design::paintViewportHint(painter,{16,52},size.width()-32,hint);
        painter.setFont(design::uiFont(design::kSmallPx));
        if(!_hasGrip&&!_simulating) return;
        const auto pivot=_simulating?_simulationPivot:palmWorld(_hand,_palm); QPointF center; float depth=0;
        if(!projectGripPoint(camera,size,pivot,center,&depth)) return;
        if(_lensAuthoring) {
            QPolygonF ring;
            for(int i=0;i<=96;++i){const float angle=float(i)*6.28318530718f/96;
                const auto world=composed(_hand,Transformf{{_lensRadius*std::cos(angle),0,_lensRadius*std::clamp(std::sin(angle),-_lensAspect,_lensAspect)}}).translation;
                QPointF point;if(projectGripPoint(camera,size,v(world),point))ring.append(point);}
            painter.setPen(QPen(QColor("#65e5d0"),2));painter.setBrush(QColor(70,220,196,28));painter.drawPolygon(ring);
            const auto world=composed(_hand,Transformf{{0,_lensForward,0}}).translation;QPointF tip;
            if(projectGripPoint(camera,size,v(world),tip)){painter.drawLine(center,tip);painter.drawEllipse(tip,4,4);painter.drawText(tip+QPointF(8,0),"View camera forward");}
        }
        QVector3D edge; GripRay centerRay;
        if(!unprojectGripPoint(camera,size,center+QPointF(76,0),depth,edge) || !gripRay(camera,size,center,centerRay)) return;
        const float radius=(edge-pivot).length(); if(!(radius>0)) return;
        const QColor colors[]{QColor("#ef7468"),QColor("#9cca75"),QColor("#6ba5e8"),QColor("#efcf99")};
        const Vec3f units[]{{1,0,0},{0,1,0},{0,0,1}};
        const bool rotate=_simulating?_simulationRotation:_rotate;
        // Handles are drawn in the GL frame, depth-tested against the weapon and
        // hands; their projected lines stay the hit-test shapes.
        _overlay.begin(camera,QSizeF(size.width()*dpr,size.height()*dpr));
        const float stroke=2.f*float(dpr);
        for(int axis=0;axis<(_teaching?3:_simulating?1:4) && !_picking;++axis) {
            const auto direction=_teaching?v(rotated(_teachingFrame.rotation,units[axis])).normalized():_simulating?_simulationAxis:axis==3?centerRay.direction:v(rotated(_reference.rotation,units[axis])).normalized();
            const int colorIndex=_simulating&&!_teaching?_simulationAxisIndex:axis;
            painter.setPen(QPen(colors[colorIndex],2)); painter.setBrush(colors[colorIndex]);
            if(!rotate && axis<3) {
                QPointF tip; if(!projectGripPoint(camera,size,pivot+direction*radius,tip) || QLineF(center,tip).length()<15) continue;
                const QLineF line(center,tip); const auto unit=(tip-center)/line.length();
                _overlay.addPolyline({pivot,pivot+direction*radius},stroke,colors[colorIndex]);
                _overlay.addArrowHead(pivot,pivot+direction*radius,11.f*float(dpr),4.f*float(dpr),colors[colorIndex]);
                painter.drawText(tip+QPointF(5,-5),QStringList{"X","Y","Z"}[colorIndex]); _handles.append({axis,direction,QPolygonF{center+unit*12,tip},false});
            } else if(rotate) {
                if(std::abs(QVector3D::dotProduct(centerRay.direction,direction))<.06f) continue;
                auto basis=QVector3D::crossProduct(direction,QVector3D(0,0,1));
                if(basis.lengthSquared()<.01f) basis=QVector3D::crossProduct(direction,QVector3D(0,1,0));
                basis.normalize(); const auto other=QVector3D::crossProduct(direction,basis).normalized();
                QPolygonF ring; QVector<QVector3D> world; const float scale=axis==3?1.22f:1.f;
                for(int segment=0;segment<=64;++segment) {
                    const float angle=float(segment)*6.28318530718f/64; QPointF p;
                    const auto point=pivot+(basis*std::cos(angle)+other*std::sin(angle))*radius*scale;
                    world.append(point);if(projectGripPoint(camera,size,point,p)) ring.append(p);
                }
                _overlay.addPolyline(world,stroke,colors[colorIndex]); _handles.append({axis,direction,ring,true});
            }
        }
        if(!rotate && !_picking && !_simulating) {
            const QRectF box(center-QPointF(6,6),QSizeF(12,12));
            // The pivot marks the point being edited: drawn over everything.
            _pivotMarker.begin(camera,QSizeF(size.width()*dpr,size.height()*dpr));
            _pivotMarker.addBox(pivot,6.f*float(dpr),colors[3]); _pivotMarker.addBox(pivot,4.5f*float(dpr),QColor("#443522"));
            _handles.append({3,centerRay.direction,QPolygonF{box.topLeft(),box.topRight(),box.bottomRight(),box.bottomLeft(),box.topLeft()},false});
        }
        painter.beginNativePainting(); _overlay.draw(kHiddenHandleAlpha);
        if(!rotate && !_picking && !_simulating)_pivotMarker.draw(1.f);
        painter.endNativePainting();
        if(_surface&&!_simulating) {
            QPointF point; if(projectGripPoint(camera,size,v(*_surface),point)) {
                painter.setPen(QPen(QColor("#f0c785"),1.5)); painter.setBrush(Qt::NoBrush); painter.drawEllipse(point,4,4);
                painter.drawLine(point-QPointF(8,0),point+QPointF(8,0)); painter.drawLine(point-QPointF(0,8),point+QPointF(0,8));
            }
        }
    }
}
