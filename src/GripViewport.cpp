#include "GripViewport.h"
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
            hand=rotateGripAbout(_dragHand,vec(_dragPivot),vec(_dragDirection),angle*_dragPrecision);
        } else {
            QVector3D delta;
            if(_dragAxis==3) {
                QVector3D plane; if(!gripPlanePoint(ray,_dragPivot,_dragDirection,plane)) return; delta=plane-_dragRadial;
            } else {
                float parameter; if(!gripAxisParameter(ray,_dragPivot,_dragDirection,parameter)) return;
                if(_simulating){if(simulationDrag)simulationDrag((parameter-_dragParameter)*_dragPrecision);return;}
                delta=_dragDirection*(parameter-_dragParameter);
            }
            hand.translation=vec(v(_dragHand.translation)+delta*_dragPrecision);
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
        } else if(type==QEvent::MouseMove && _active && _dragAxis>=0 && mouse->buttons().testFlag(Qt::LeftButton)) drag(position);
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
        QFont font=painter.font(); font.setPixelSize(12); painter.setFont(font);
        const QString labels[]{_teaching?"PICK PART":_simulating?"RESET":_workshop?(_workshopHand?"PLACE PALM":"PICK PART"):"PICK POINT",
            _simulating&&!_teaching?"DRAG PART":"MOVE",_simulating&&!_teaching?"STAGED":"ROTATE",
            _simulating&&!_teaching?"RELEASE":_pointAuthoring?"SAVE POINTS":_workshop?"SAVE":"SAVE GRIP",_teaching?"TEST":_simulating?"EDIT":"END"};
        qreal x=16;
        for(int i=0;i<5;++i) {
            const bool selected=(i==0 && _picking) || (i==1 && !_picking && !_rotate) || (i==2 && !_picking && _rotate);
            const bool enabled=_simulating&&!_teaching?true:i==0?_canPick:i==3?_canSave:!(i==2&&_pointAuthoring);
            const qreal width=painter.fontMetrics().horizontalAdvance(labels[i])+24;
            const QRectF rect(x,16,width,30); _buttons.append(rect); x+=width+6;
            painter.setPen(QColor(selected?"#f0c785":"#59646a")); painter.setBrush(QColor(selected?"#443522":"#1b2023")); painter.drawRoundedRect(rect,3,3);
            painter.setPen(QColor(enabled?"#eee8df":"#67777d")); painter.drawText(rect,Qt::AlignCenter,labels[i]);
        }
        painter.setPen(QColor("#f0c785"));
        painter.drawText(QRectF(16,52,size.width()-32,40),Qt::AlignLeft|Qt::TextWordWrap,
            _teaching?QStringLiteral("PART: %1 · Drag arrows to SET travel, rings to SET rotation · SAVE keeps the part + hand · TEST checks grab/release").arg(_partName):
            _simulating?QStringLiteral("TESTING PART: %1 · Grab or drag its handle · release tests its rule · EDIT returns to endpoint authoring").arg(_partName):_picking?QStringLiteral("Click the weapon surface · Esc cancels"):
            QStringLiteral("Arrows move · rings rotate · center square moves in screen plane · Shift+drag for precision · background drag orbits"));
        if(!_hasGrip&&!_simulating) return;
        const auto pivot=_simulating?_simulationPivot:palmWorld(_hand,_palm); QPointF center; float depth=0;
        if(!projectGripPoint(camera,size,pivot,center,&depth)) return;
        QVector3D edge; GripRay centerRay;
        if(!unprojectGripPoint(camera,size,center+QPointF(76,0),depth,edge) || !gripRay(camera,size,center,centerRay)) return;
        const float radius=(edge-pivot).length(); if(!(radius>0)) return;
        const QColor colors[]{QColor("#ef7468"),QColor("#9cca75"),QColor("#6ba5e8"),QColor("#efcf99")};
        const Vec3f units[]{{1,0,0},{0,1,0},{0,0,1}};
        const bool rotate=_simulating?_simulationRotation:_rotate;
        for(int axis=0;axis<(_teaching?3:_simulating?1:4) && !_picking;++axis) {
            const auto direction=_teaching?v(rotated(_teachingFrame.rotation,units[axis])).normalized():_simulating?_simulationAxis:axis==3?centerRay.direction:v(rotated(_reference.rotation,units[axis])).normalized();
            const int colorIndex=_simulating&&!_teaching?_simulationAxisIndex:axis;
            painter.setPen(QPen(colors[colorIndex],2)); painter.setBrush(colors[colorIndex]);
            if(!rotate && axis<3) {
                QPointF tip; if(!projectGripPoint(camera,size,pivot+direction*radius,tip) || QLineF(center,tip).length()<15) continue;
                painter.drawLine(center,tip); const QLineF line(center,tip); const auto unit=(tip-center)/line.length(),side=QPointF(-unit.y(),unit.x());
                painter.drawPolygon(QPolygonF{tip,tip-unit*11+side*4,tip-unit*11-side*4});
                painter.drawText(tip+QPointF(5,-5),QStringList{"X","Y","Z"}[colorIndex]); _handles.append({axis,direction,QPolygonF{center+unit*12,tip},false});
            } else if(rotate) {
                if(std::abs(QVector3D::dotProduct(centerRay.direction,direction))<.06f) continue;
                auto basis=QVector3D::crossProduct(direction,QVector3D(0,0,1));
                if(basis.lengthSquared()<.01f) basis=QVector3D::crossProduct(direction,QVector3D(0,1,0));
                basis.normalize(); const auto other=QVector3D::crossProduct(direction,basis).normalized();
                QPolygonF ring; const float scale=axis==3?1.22f:1.f;
                for(int segment=0;segment<=64;++segment) {
                    const float angle=float(segment)*6.28318530718f/64; QPointF p;
                    if(projectGripPoint(camera,size,pivot+(basis*std::cos(angle)+other*std::sin(angle))*radius*scale,p)) ring.append(p);
                }
                painter.setBrush(Qt::NoBrush); painter.drawPolyline(ring); _handles.append({axis,direction,ring,true});
            }
        }
        if(!rotate && !_picking && !_simulating) {
            const QRectF box(center-QPointF(6,6),QSizeF(12,12)); painter.setPen(colors[3]); painter.setBrush(QColor("#443522")); painter.drawRect(box);
            _handles.append({3,centerRay.direction,QPolygonF{box.topLeft(),box.topRight(),box.bottomRight(),box.bottomLeft(),box.topLeft()},false});
        }
        if(_surface&&!_simulating) {
            QPointF point; if(projectGripPoint(camera,size,v(*_surface),point)) {
                painter.setPen(QPen(QColor("#f0c785"),1.5)); painter.setBrush(Qt::NoBrush); painter.drawEllipse(point,4,4);
                painter.drawLine(point-QPointF(8,0),point+QPointF(8,0)); painter.drawLine(point-QPointF(0,8),point+QPointF(0,8));
            }
        }
    }
}
