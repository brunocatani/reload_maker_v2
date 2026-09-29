#include "GripViewportMath.h"
#include "SlotOverlay.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QQuaternion>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUuid>
#include <cmath>
#include <stdexcept>
using namespace reloadmaker;
namespace {
    void require(bool ok,const QString& why){if(!ok)throw std::runtime_error(why.toStdString());}
    QVector3D v(Vec3f p){return {p.x,p.y,p.z};}
    void near(QVector3D a,QVector3D b){require((a-b).length()<.003f,"Point mismatch");}
    void sameRotation(Quaternionf a,Quaternionf b){for(auto axis:{Vec3f{1,0,0},Vec3f{0,1,0},Vec3f{0,0,1}})near(v(rotated(a,axis)),v(rotated(b,axis)));}
    void cameras(){
        for(bool perspective:{false,true}) {
            QMatrix4x4 projection,view;
            if(perspective)projection.perspective(60,1.7f,.5f,400);else projection.ortho(-34,34,-20,20,.5f,400);
            view.lookAt({30,-70,40},{0,0,0},{0,0,1}); const auto camera=projection*view;
            for(auto point:{QVector3D{0,0,0},QVector3D{2,-4,3},QVector3D{-7,6,-1}}) {
                QPointF pixel; float depth;
                require(projectGripPoint(camera,{1020,600},point,pixel,&depth),"Projection failed");
                QVector3D world; require(unprojectGripPoint(camera,{1020,600},pixel,depth,world),"Unprojection failed"); near(world,point);
                for(float dpr:{1.f,1.25f,1.5f,2.f}) {
                    require(unprojectGripPoint(camera,{1020*dpr,600*dpr},pixel*dpr,depth,world),"DPI unprojection failed");near(world,point);
                }
                GripRay ray;require(gripRay(camera,{1020,600},pixel,ray),"Ray failed");
                QVector3D plane;require(gripPlanePoint(ray,point,ray.direction,plane),"Plane failed");near(plane,point);
                for(auto axis:{QVector3D{1,0,0},QVector3D{0,1,0},QVector3D{0,0,1}}) {
                    QPointF moved;require(projectGripPoint(camera,{1020,600},point+axis*3,moved),"Moved projection failed");
                    GripRay target;require(gripRay(camera,{1020,600},moved,target),"Moved ray failed");
                    float parameter;require(gripAxisParameter(target,point,axis,parameter),"Axis failed");require(std::abs(parameter-3)<.003f,"Axis drag has wrong direction/units");
                }
            }
        }
        float t;require(!gripAxisParameter({{0,0,0},{1,0,0}},{0,0,0},{1,0,0},t),"Parallel axis accepted");
        QVector3D p;require(!gripPlanePoint({{0,0,0},{1,0,0}},{0,0,0},{0,1,0},p),"Parallel plane accepted");
        QMatrix4x4 singular;singular.fill(0);require(!unprojectGripPoint(singular,{100,100},{50,50},.5,p),"Singular camera accepted");
    }
    void placements(){
        for(auto side:{PoseHand::Left,PoseHand::Right}) {
            HandPose pose;pose.hand=side;
            for(int i:{3,6,9,12})pose.fingers[i].translation={side==PoseHand::Left?-2.f:2.f,8,1};
            const auto palm=genericPalmPoint(pose);near(v(palm),{side==PoseHand::Left?-1.f:1.f,4,.5f});
            const auto q=QQuaternion::fromEulerAngles(31,67,-24);
            Transformf hand{{4,8,-2},{q.x(),q.y(),q.z(),q.scalar()},{1.2f,1.2f,1.2f}};
            const Vec3f contact{9,-3,7};hand=placePalmAt(hand,palm,contact);
            near(v(composed(hand,Transformf{palm}).translation),v(contact));
            for(auto axis:{Vec3f{1,0,0},Vec3f{0,1,0},Vec3f{0,0,1}}) {
                const auto rotatedHand=rotateGripAbout(hand,contact,axis,73);
                near(v(composed(rotatedHand,Transformf{palm}).translation),v(contact));
                const auto restored=rotateGripAbout(rotatedHand,contact,axis,-73);
                near(v(restored.translation),v(hand.translation));sameRotation(restored.rotation,hand.rotation);
            }
        }
    }
    void gestures(){
        SlotPointerGesture input;
        require(input.consume(QEvent::MouseButtonPress,Qt::LeftButton,Qt::LeftButton,true),"Placement press leaked");
        require(input.consume(QEvent::MouseMove,Qt::NoButton,Qt::LeftButton,false),"Drag outside handle leaked");
        // Picking mode may have ended on press, and the editor may disappear.
        require(input.consume(QEvent::MouseButtonRelease,Qt::LeftButton,Qt::NoButton,false),"Consumed press leaked its release");
        require(!input.captured(),"Gesture stuck after release");
        require(!input.consume(QEvent::MouseButtonPress,Qt::LeftButton,Qt::LeftButton,false),"Camera drag blocked");
        require(!input.consume(QEvent::MouseMove,Qt::NoButton,Qt::LeftButton,false),"Camera move blocked");
        require(input.consume(QEvent::MouseButtonPress,Qt::LeftButton,Qt::LeftButton,true),"Second handle failed");
        (void)input.consume(QEvent::FocusOut,Qt::NoButton,Qt::NoButton); require(!input.captured(),"Focus loss stuck gesture");
    }
    void storage(){
        PartGrip g;g.pose.id=QUuid::createUuid().toString(QUuid::WithoutBraces);g.sourcePoseId=g.pose.id;g.weaponRecord="Test.esp:012345";
        g.pose.name="Mouse palm";g.pose.createdUtc=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);g.pose.placement.anchorKind="weapon";g.pose.placement.anchorName="Weapon";
        QString error;PartGrip decoded;
        auto bytes=encodePartGrip(g,error);require(!bytes.isEmpty() && QJsonDocument::fromJson(bytes)["version"].toInt()==1,"Legacy encoding changed");
        require(decodePartGrip(bytes,decoded,error) && !decoded.surfaceContact,"Legacy file lost compatibility");
        g.surfaceContact=PartGrip::SurfaceContact{{3,4,5},{1,2,3},"Magazine surface"};
        g.pose.placement.handInAnchor.translation={-4,6,-2};
        const auto q=QQuaternion::fromEulerAngles(17,23,71);g.pose.placement.handInAnchor.rotation={q.x(),q.y(),q.z(),q.scalar()};
        bytes=encodePartGrip(g,error);require(QJsonDocument::fromJson(bytes)["version"].toInt()==2,"Surface version missing");
        require(decodePartGrip(bytes,decoded,error),error);
        near(v(decoded.surfaceContact->pointInAnchor),{3,4,5});near(v(decoded.pose.placement.handInAnchor.translation),{-4,6,-2});
        sameRotation(decoded.pose.placement.handInAnchor.rotation,g.pose.placement.handInAnchor.rotation);
        QTemporaryDir dir;require(dir.isValid(),"Temporary folder unavailable");PartGripLibrary library(dir.path());require(library.save(g,error),error);
        QStringList errors;const auto loaded=PartGripLibrary(dir.path()).load(errors);require(loaded.size()==1 && errors.isEmpty(),"Restart lost mouse grip");
        near(v(loaded[0].surfaceContact->palmInHand),{1,2,3});
        auto object=QJsonDocument::fromJson(bytes).object();object.remove("surfaceContact");require(!decodePartGrip(QJsonDocument(object).toJson(),decoded,error),"Version 2 missing contact accepted");
        object["version"]=3;require(!decodePartGrip(QJsonDocument(object).toJson(),decoded,error),"Shared version without mechanism ID accepted");
        g.mechanismId=QUuid::createUuid().toString(QUuid::WithoutBraces);
        bytes=encodePartGrip(g,error);
        require(QJsonDocument::fromJson(bytes)["version"].toInt()==3,"Linked grip version missing");
        require(decodePartGrip(bytes,decoded,error)&&decoded.mechanismId==g.mechanismId,error);
        near(v(decoded.surfaceContact->pointInAnchor),{3,4,5});
        sameRotation(decoded.pose.placement.handInAnchor.rotation,g.pose.placement.handInAnchor.rotation);
        g.surfaceContact.reset();bytes=encodePartGrip(g,error);
        require(decodePartGrip(bytes,decoded,error)&&!decoded.surfaceContact&&decoded.mechanismId==g.mechanismId,error);
        object=QJsonDocument::fromJson(bytes).object();object["version"]=1;
        require(!decodePartGrip(QJsonDocument(object).toJson(),decoded,error),"Legacy version silently dropped mechanism link");
        object["version"]=4;require(!decodePartGrip(QJsonDocument(object).toJson(),decoded,error),"Unknown version accepted");
    }
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{cameras();placements();gestures();storage();QTextStream(stdout)<<"PASS projection/depth/DPI, axis dragging, palm placement, rotation pivot, gesture ownership, surface-save persistence and legacy compatibility\n";return 0;}catch(const std::exception& e){QTextStream(stderr)<<"FAIL "<<e.what()<<'\n';return 1;}}
