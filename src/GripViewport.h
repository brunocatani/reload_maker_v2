#pragma once
#include "GripViewportMath.h"
#include "SlotOverlay.h"
#include "viewport/DepthOverlay.h"
#include <QPolygonF>

namespace reloadmaker {
    // One event owner for the complete placement/drag gesture. Ordinary background
    // drags remain GLView's camera gestures; a consumed press never leaks its release.
    class GripViewport final:public QObject {
    public:
        explicit GripViewport(GLView* view,QObject* parent);
        void configure(bool active,bool canPick,bool canSave);
        void setGrip(const Transformf& reference,const Transformf& hand,Vec3f palm,std::optional<Vec3f> surface);
        void clearGrip();
        void setPicking(bool picking);
        void setSimulation(bool active,QVector3D pivot={},QVector3D axis={0,1,0},bool rotation=false,int axisIndex=0);
        void setTeaching(bool active,const Transformf& frame={});
        void setWorkshop(bool active,const QString& partName={},bool handEditing=false);
        void setRotationTool(bool rotation);
        void setPointAuthoring(bool value) {if(value&&!_pointAuthoring)_rotate=false;_pointAuthoring=value;}
        void setLensAuthoring(float radius,float aspect,float cameraForward) {_lensAuthoring=true;_lensRadius=radius;_lensAspect=aspect;_lensForward=cameraForward;}
        [[nodiscard]] bool picking() const { return _picking; }
        std::function<void()> beginPick,save,end;
        std::function<void(QPointF)> surfacePicked;
        std::function<void(const Transformf&)> edited;
        std::function<bool(QPointF)> simulationHit;
        std::function<void()> simulationGrab,simulationRelease,simulationCancel,simulationReset,simulationStop;
        std::function<void(float)> simulationDrag;
        std::function<void(int,bool)> teachingGrab;
        std::function<void(bool)> teachingTool;
        std::function<void()> test;
        std::function<void()> pickingCancelled;
    protected:
        bool eventFilter(QObject* watched,QEvent* event) override;
    private:
        struct Handle { int axis; QVector3D direction; QPolygonF line; bool rotate; };
        void paint();
        void finishDrag(bool restore);
        bool beginDrag(Handle handle,QPointF point);
        void drag(QPointF point);
        GLView* _view;
        bool _active{false},_canPick{false},_canSave{false},_picking{false},_hasGrip{false},_rotate{false};
        Transformf _reference{},_hand{},_dragHand{};
        Vec3f _palm{};
        std::optional<Vec3f> _surface;
        QVector<Handle> _handles;
        QVector<QRectF> _buttons;
        SlotPointerGesture _gesture;
        int _dragAxis{-1};
        bool _dragRotate{false};
        QMatrix4x4 _dragCamera;
        QSizeF _dragSize;
        QVector3D _dragPivot,_dragDirection,_dragRadial;
        float _dragParameter{0};
        float _dragPrecision{1};
        bool _dragSnap{false};
        DepthOverlay _overlay;
        DepthOverlay _pivotMarker;
        float _dragAngle{0},_dragLastAngle{0};
        bool _simulating{false},_simulationRotation{false};
        bool _teaching{false},_workshop{false},_workshopHand{false},_pointAuthoring{false};
        Transformf _teachingFrame;
        QString _partName;
        bool _lensAuthoring{false};
        float _lensRadius{1},_lensAspect{1},_lensForward{10};
        QVector3D _simulationPivot,_simulationAxis;
        int _simulationAxisIndex{0};
    };
}
