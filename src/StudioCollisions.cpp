#include "ReloadMakerController.h"
#include "CollisionPanel.h"
#include "StudioWindow.h"
#include "StudioPaths.h"
#include "glview.h"
#include "design/Theme.h"
#include <QDockWidget>
#include <QScrollArea>
#include <QOpenGLPaintDevice>
#include <QPainter>
#include <QFileDialog>
#include <QDateTime>

namespace reloadmaker {
void ReloadMakerController::createCollisionsUi(){
    _collisionsDock=new QDockWidget("COLLISIONS",_window);_collisionsDock->setObjectName("CollisionsDock");_collisionsDock->setFeatures(QDockWidget::NoDockWidgetFeatures);_collisionsDock->setMinimumWidth(340);_collisionsDock->setMaximumWidth(470);
    _collisions=new CollisionPanel(_collisionWorkshop);auto* scroll=new QScrollArea;scroll->setWidgetResizable(true);scroll->setWidget(_collisions);_collisionsDock->setWidget(scroll);_window->addDockWidget(Qt::RightDockWidgetArea,_collisionsDock);_collisionsDock->hide();
    _collisionWorkshop.snapshot=[this](int index,bool reference){return collisionSnapshot(index,reference);};
    _collisionWorkshop.cook=[this]{showCollisionCooker();};
    _collisionWorkshop.prepare=[this]{const auto state=_collisionWorkshop.profile().referencePose["presentation"].toString("part");const auto suggested=studioDataPath("exports/objects/"+QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz")+"/"+state+".nif");const auto output=QFileDialog::getSaveFileName(_window,"Capture visible part in the selected root frame",suggested,"Prepared world model (*.nif)");if(!output.isEmpty())capturePreparedPart(output);};
    _collisionWorkshop.chooseProfile=[this]{return QFileDialog::getOpenFileName(_collisions,"Open collision profile",studioDataPath("collisions"),"Collision JSON (*.json)");};
    _collisionWorkshop.chooseExport=[this]{return QFileDialog::getSaveFileName(_collisions,"Export collision points",studioDataPath("exports/collision.json"),"Collision JSON (*.json)");};
    connect(this,&ReloadMakerController::packJobChanged,this,[this]{if(_packJob["operation"]!="prepare")return;
        if(_packJob["phase"]=="complete"){const auto result=_packJob["result"].toObject();_preparedPart=result;setStatus("Prepared visible part saved · "+result["output"].toString());}
        else if(_packJob["phase"]=="failed")setStatus(_packJob["error"].toString(),true);
    });
    connect(&_collisionWorkshop,&CollisionWorkshop::changed,this,[this](bool authored){if(authored)rememberCollisionDraft();++_revision;_window->getGLView()->update();});
    connect(_window->getGLView(),&GLView::paintUpdate,this,[this]{paintCollisions();});
}
void ReloadMakerController::paintCollisions(){
    if(!collisionWorkspace(_workspace)||!_collisions||!_collisionWorkshop.overlay())return;
    auto* view=_window->getGLView();const auto size=QSizeF(view->width(),view->height());const auto dpr=view->devicePixelRatio();QOpenGLPaintDevice device(QSize(qRound(size.width()*dpr),qRound(size.height()*dpr)));device.setDevicePixelRatio(dpr);QPainter painter(&device);painter.setRenderHint(QPainter::Antialiasing);
    const auto camera=view->reloadMakerWorldToClip();
    auto draw=[&](const CollisionProfile& profile,bool baseline){QHash<QString,QMatrix4x4> frames;for(const auto& s:profile.sources)frames.insert(s.key,camera*s.frame);
        for(int i=0;i<profile.hulls.size();++i){const auto& h=profile.hulls[i];if(!h.enabled||(!baseline&&_collisionWorkshop.isolated()&&i!=_collisionWorkshop.selectedHull()))continue;const auto transform=frames.value(h.source);
            QColor color=baseline?QColor(93,145,239):i==_collisionWorkshop.selectedHull()?QColor(255,211,113):QColor(81,210,183);
            auto project=[&](quint32 index,QPointF& out){const auto clip=transform*QVector4D(h.points[int(index)],1);if(clip.w()<=.001||clip.z()<-clip.w())return false;out={(clip.x()/clip.w()+1)*size.width()/2,(1-clip.y()/clip.w())*size.height()/2};return true;};
            if(!baseline){auto fill=color;fill.setAlpha(12);painter.setPen(Qt::NoPen);painter.setBrush(fill);for(auto face:h.faces){QPolygonF polygon;for(auto index:face){QPointF point;if(!project(index,point)){polygon.clear();break;}polygon<<point;}if(polygon.size()==3)painter.drawPolygon(polygon);}}
            painter.setBrush(Qt::NoBrush);color.setAlpha(baseline?140:220);painter.setPen(QPen(color,baseline?.8:1.1));for(auto edge:h.edges){QPointF a,b;if(project(edge[0],a)&&project(edge[1],b))painter.drawLine(a,b);}
        }};
    if(_collisionWorkshop.compare())draw(_collisionWorkshop.baseline(),true);
    draw(_collisionWorkshop.profile(),false);
    design::paintViewportHint(painter,{16,14},size.width()-32,QStringLiteral("Convex point envelopes, drawn in x-ray · drag the background to orbit"));
}
}
