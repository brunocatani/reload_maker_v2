#include "ReloadRecipe.h"
#include "PartGrip.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>

namespace reloadmaker {
    namespace {
        constexpr qint64 maximumBytes=96*1024*1024;
        constexpr float motionStart=.10f,minimumExcursion=.15f,dwellRadius=.05f;
        float length(Vec3f p){return std::sqrt(p.x*p.x+p.y*p.y+p.z*p.z);}
        Vec3f subtract(Vec3f a,Vec3f b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
        float angle(Quaternionf a,Quaternionf b){
            const double dot=double(a.x)*b.x+double(a.y)*b.y+double(a.z)*b.z+double(a.w)*b.w;
            const double aa=double(a.x)*a.x+double(a.y)*a.y+double(a.z)*a.z+double(a.w)*a.w;
            const double bb=double(b.x)*b.x+double(b.y)*b.y+double(b.z)*b.z+double(b.w)*b.w;
            return float(2*std::acos(std::clamp(std::abs(dot)/std::sqrt(aa*bb),0.,1.)));
        }
        bool finite(const Transformf& p){
            for(float v:{p.translation.x,p.translation.y,p.translation.z,p.rotation.x,p.rotation.y,p.rotation.z,p.rotation.w,p.scale.x,p.scale.y,p.scale.z})
                if(!std::isfinite(v)||std::abs(v)>1e7f)return false;
            const auto& q=p.rotation;return std::abs(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w-1)<.01f;
        }
        bool identity(const QString& id){return id.size()==64&&std::all_of(id.cbegin(),id.cend(),[](QChar c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});}
        QJsonObject anchorJson(const HandPosePlacement& a){return {{"kind",a.anchorKind},{"name",a.anchorName},{"path",a.anchorPath},{"model",a.ownerModel},{"record",a.ownerRecord}};}
        HandPosePlacement readAnchor(const QJsonObject& a){return {a["kind"].toString(),a["name"].toString(),a["path"].toString(),a["model"].toString(),a["record"].toString(),{}, {}};}
        bool validCapture(const ReloadRecipe& r,QString& error){
            if(r.source["action"]!="WPNReload"||!std::isfinite(r.duration)||r.duration<=0||r.frames<2||r.frames>maximumReloadFrames||r.tracks.isEmpty()||r.tracks.size()>maximumReloadTracks||r.revision<0){error="Invalid base-reload source or capture size/duration";return false;}
            for(int i=0;i<r.tracks.size();++i){const auto& t=r.tracks[i];
                if(t.poses.size()!=r.frames||t.label.isEmpty()||t.label.size()>512||t.anchor.ownerRecord.isEmpty()||t.anchor.ownerModel.isEmpty()||t.anchor.anchorPath.isEmpty()||t.parent< -1||t.parent>=r.tracks.size()||t.parent==i){error="Invalid captured part identity or track";return false;}
                int ancestor=t.parent;
                for(int depth=0;ancestor>=0;++depth){if(depth>=r.tracks.size()||ancestor>=r.tracks.size()||ancestor==i){error="Cyclic captured part ancestry";return false;}ancestor=r.tracks[ancestor].parent;}
                for(int j=0;j<i;++j)if(sameGripAnchor(t.anchor,r.tracks[j].anchor)){error="Duplicate captured part identity";return false;}
                for(const auto& p:t.poses)if(!finite(p)){error="Non-finite/invalid captured transform";return false;}
            }
            return true;
        }
        bool validWindows(const ReloadRecipe& r,QString& error){
            if(r.windows.size()>2048){error="Too many reload windows";return false;}
            int previous=0;
            for(const auto& w:r.windows){
                if(w.first<previous||w.first<0||w.last<=w.first||w.last>=r.frames||w.label.trimmed().isEmpty()||w.label.size()>128||w.members.isEmpty()||w.members.size()>r.tracks.size()){error="Invalid or overlapping reload window";return false;}
                previous=w.last;QVector<int> seen;
                for(const auto& member:w.members){
                    if(member.track<0||member.track>=r.tracks.size()||seen.contains(member.track)||int(member.relation)<0||int(member.relation)>2||(!member.gripId.isEmpty()&&QUuid(member.gripId).isNull())||(member.grabbable&&!r.tracks[member.track].surface)){error="Invalid reload member/grab selection";return false;}
                    seen.append(member.track);
                }
            }
            return true;
        }
        struct Span {int first,last;};
        QVector<Span> movingSpans(const ReloadTrack& track,int stable){
            QVector<Span> spans;const auto& poses=track.poses;int rest=0,start=-1,peak=0;float excursion=0;int retreat=0;
            const auto finish=[&](int last){
                if(start>=0&&last>start){float distance=0;for(int j=start+1;j<=last;++j)distance=std::max(distance,reloadPoseDistance(poses[start],poses[j]));if(distance>=minimumExcursion)spans.append({start,last});}
            };
            for(int i=1;i<poses.size();++i){
                if(!reloadPoseVisible(poses[i])||!reloadPoseVisible(poses[i-1])){finish(i-1);rest=i;start=-1;excursion=0;retreat=0;continue;}
                if(start<0){
                    if(reloadPoseDistance(poses[rest],poses[i])>=motionStart){start=rest;peak=i;excursion=reloadPoseDistance(poses[start],poses[i]);}
                    else if(reloadPoseDistance(poses[i-1],poses[i])<1e-5f)rest=i-1;
                    continue;
                }
                const float distance=reloadPoseDistance(poses[start],poses[i]);
                if(distance>excursion){excursion=distance;peak=i;retreat=0;}
                if(i-start>=stable){
                    const int beginning=i-stable;bool quiet=true;float arc=0;
                    for(int j=beginning+1;j<=i;++j)if(reloadPoseDistance(poses[beginning],poses[j])>dwellRadius){quiet=false;break;}
                    for(int j=beginning+1;j<=i;++j)arc+=reloadPoseDistance(poses[j-1],poses[j]);
                    // A small but consistently advancing path is slow motion,
                    // not a dwell. Do not repeatedly discard it below the start threshold.
                    if(arc>.001f&&reloadPoseDistance(poses[beginning],poses[i])>=arc*.5f)quiet=false;
                    if(quiet){finish(beginning);rest=beginning;start=-1;excursion=0;retreat=0;continue;}
                }
                if(excursion>=minimumExcursion&&distance<excursion-std::max(.15f,excursion*.08f))++retreat;else retreat=0;
                if(retreat>=2){finish(peak);start=peak;rest=peak;excursion=reloadPoseDistance(poses[start],poses[i]);peak=i;retreat=0;}
            }
            finish(poses.size()-1);return spans;
        }
        float excursion(const ReloadTrack& t,int first,int last){
            float result=0;int start=-1;
            for(int i=first;i<=last;++i){if(!reloadPoseVisible(t.poses[i])){start=-1;continue;}if(start<0)start=i;result=std::max(result,reloadPoseDistance(t.poses[start],t.poses[i]));}
            return result;
        }
        bool rigid(const ReloadTrack& leader,const ReloadTrack& other,int first,int last){
            Transformf initial;bool initialized=false;
            for(int i=first;i<=last;++i){
                Transformf inverse;if(!reloadPoseVisible(leader.poses[i])||!reloadPoseVisible(other.poses[i])||!invertUniformTransform(leader.poses[i],inverse))return false;
                const auto relative=composed(inverse,other.poses[i]);if(!initialized){initial=relative;initialized=true;}
                if(length(subtract(relative.translation,initial.translation))>.10f||angle(relative.rotation,initial.rotation)>.02f||length(subtract(relative.scale,initial.scale))>.005f)return false;
            }
            return initialized;
        }
        void keepDecisions(QVector<ReloadMember>& into,const QVector<ReloadMember>& from){
            for(auto& member:into)for(const auto& previous:from)if(member.track==previous.track){member.grabbable=previous.grabbable;member.gripId=previous.gripId;break;}
        }
    }
    float reloadPoseDistance(const Transformf& a,const Transformf& b){return length(subtract(a.translation,b.translation))+3*angle(a.rotation,b.rotation);}
    bool reloadPoseVisible(const Transformf& pose){return std::abs(pose.scale.x)>1e-5f&&std::abs(pose.scale.y)>1e-5f&&std::abs(pose.scale.z)>1e-5f;}
    Transformf reloadTrackPose(const ReloadRecipe& r,int track,float seconds){
        if(track<0||track>=r.tracks.size()||r.frames<2||r.duration<=0)return {};
        const float position=std::clamp(seconds/r.duration,0.f,1.f)*float(r.frames-1);const int first=int(position),last=std::min(first+1,r.frames-1);
        return interpolatedTransform(r.tracks[track].poses[first],r.tracks[track].poses[last],position-first);
    }
    QVector<ReloadMember> reloadMembers(const ReloadRecipe& r,int first,int last){
        QVector<ReloadMember> members;QVector<float> travel(r.tracks.size());
        for(int i=0;i<r.tracks.size();++i){
            bool visible=true;for(int j=first;j<=last;++j)if(!reloadPoseVisible(r.tracks[i].poses[j])){visible=false;break;}
            if(!visible||excursion(r.tracks[i],0,r.frames-1)<minimumExcursion){continue;}
            travel[i]=excursion(r.tracks[i],first,last);
            if(travel[i]>.025f)members.append({i,ReloadRelation::Timed,r.tracks[i].surface,{}});
        }
        if(members.isEmpty())return members;
        int leader=members[0].track;float best=-1;
        for(auto& member:members){
            bool ancestorGrab=false;int p=r.tracks[member.track].parent;
            for(int depth=0;p>=0&&depth<r.tracks.size();++depth){
                if(r.tracks[p].surface&&std::any_of(members.cbegin(),members.cend(),[p](const auto& m){return m.track==p;})&&rigid(r.tracks[p],r.tracks[member.track],first,last)){ancestorGrab=true;break;}p=r.tracks[p].parent;
            }
            if(ancestorGrab)member.grabbable=false;
            const float score=travel[member.track]+(member.grabbable?100000.f:0.f);
            if(score>best){best=score;leader=member.track;}
        }
        for(auto& member:members)member.relation=member.track==leader?ReloadRelation::Independent:rigid(r.tracks[leader],r.tracks[member.track],first,last)?ReloadRelation::Rigid:ReloadRelation::Timed;
        return members;
    }
    bool analyzeReloadWindows(ReloadRecipe& r,QString& error){
        error.clear();if(!validCapture(r,error))return false;
        const int stable=std::max(2,int(std::round(.067f*(r.frames-1)/r.duration)));
        QVector<int> boundaries;QVector<QVector<Span>> spans;
        for(const auto& track:r.tracks)spans.append(movingSpans(track,stable));
        for(int track=0;track<spans.size();++track)for(const auto& span:spans[track]){
            bool inherited=false;int parent=r.tracks[track].parent;
            for(int depth=0;parent>=0&&depth<r.tracks.size()&&!inherited;++depth){
                for(const auto& owner:spans[parent])if(owner.first<=span.first&&owner.last>=span.last){inherited=true;break;}
                parent=r.tracks[parent].parent;
            }
            if(!inherited)for(int other=0;other<spans.size()&&!inherited;++other){
                if(other==track||!r.tracks[other].surface)continue;
                for(const auto& owner:spans[other])if(owner.first<=span.first&&owner.last>=span.last&&(owner.first<span.first||owner.last>span.last)){
                    // Co-timed motion is a participant in the enclosing stroke,
                    // not a rigid child. A tiny long-running wobble cannot own
                    // a much larger manipulation just by surrounding it in time.
                    if(excursion(r.tracks[other],owner.first,owner.last)*1.5f>=excursion(r.tracks[track],span.first,span.last)){inherited=true;break;}
                }
            }
            // A moving magazine's child bullets must not fragment its interval
            // into one window per bullet. Their own full curves remain captured.
            if(!inherited){boundaries.append(span.first);boundaries.append(span.last);}
        }
        // Visibility events must survive co-timed grouping. Never connect a
        // parked shell/magazine pose to a live one through an interpolated path.
        for(const auto& track:r.tracks)for(int i=1;i<r.frames;++i)if(reloadPoseVisible(track.poses[i])!=reloadPoseVisible(track.poses[i-1])){
            boundaries.append(i-1);boundaries.append(i);
        }
        std::sort(boundaries.begin(),boundaries.end());boundaries.erase(std::unique(boundaries.begin(),boundaries.end()),boundaries.end());
        QVector<ReloadWindow> windows;
        for(int i=1;i<boundaries.size();++i){
            const int first=boundaries[i-1],last=boundaries[i];bool active=false;
            for(const auto& track:spans)for(const auto& span:track)if(span.first<=first&&span.last>=last){active=true;break;}
            if(!active||last<=first)continue;
            auto members=reloadMembers(r,first,last);if(members.isEmpty())continue;
            // All boundaries are retained: membership may be identical on the
            // opening and returning strokes, but their chronological paths differ.
            QStringList names;for(const auto& member:members)if(member.grabbable)names.append(r.tracks[member.track].anchor.anchorName);
            if(names.isEmpty())names.append(r.tracks[members[0].track].anchor.anchorName);
            const auto label=QStringLiteral("%1 · %2").arg(windows.size()+1).arg(names.mid(0,2).join(" + ")).left(128);
            windows.append({label,first,last,std::move(members),false});
        }
        if(windows.size()>2048){error="Reload has too many distinct motion windows to edit safely";return false;}
        r.windows=std::move(windows);r.id=reloadRecipeIdentity(r);return true;
    }
    bool splitReloadWindow(ReloadRecipe& r,int index,int frame,QString& error){
        if(index<0||index>=r.windows.size()||frame<=r.windows[index].first||frame>=r.windows[index].last){error="Place the cursor inside the selected window before splitting";return false;}
        auto a=r.windows[index],b=a;a.last=frame;b.first=frame;
        a.members=reloadMembers(r,a.first,a.last);b.members=reloadMembers(r,b.first,b.last);
        if(a.members.isEmpty()||b.members.isEmpty()){error="Split would create a window with no measurable movement";return false;}
        keepDecisions(a.members,r.windows[index].members);keepDecisions(b.members,r.windows[index].members);
        b.label=(b.label+" · next").left(128);r.windows[index]=a;r.windows.insert(index+1,b);return true;
    }
    bool mergeReloadWindow(ReloadRecipe& r,int index,QString& error){
        if(index<0||index+1>=r.windows.size()){error="Select a window with a following window to merge";return false;}
        if(r.windows[index].returnOnRelease!=r.windows[index+1].returnOnRelease){error="The windows have different release rules; keep them separate or make the rules agree first";return false;}
        for(const auto& a:r.windows[index].members)for(const auto& b:r.windows[index+1].members)if(a.track==b.track&&(a.grabbable!=b.grabbable||a.gripId!=b.gripId)){
            error="The windows have different grab/hand assignments; make them agree before merging";return false;
        }
        for(const auto& window:{r.windows[index],r.windows[index+1]})for(const auto& member:window.members)
            for(int i=r.windows[index].first;i<=r.windows[index+1].last;++i)if(!reloadPoseVisible(r.tracks[member.track].poses[i])){error="Keep visibility changes in separate windows; merging would bridge a hidden/parked part";return false;}
        auto merged=r.windows[index];merged.last=r.windows[index+1].last;merged.members=reloadMembers(r,merged.first,merged.last);
        if(merged.members.isEmpty()){error="The merged interval has no continuous visible movement";return false;}
        keepDecisions(merged.members,r.windows[index+1].members);keepDecisions(merged.members,r.windows[index].members);
        r.windows[index]=merged;r.windows.removeAt(index+1);return true;
    }
    QString reloadRecipeIdentity(const ReloadRecipe& r){
        QCryptographicHash hash(QCryptographicHash::Sha256);hash.addData(QJsonDocument(r.source).toJson(QJsonDocument::Compact));
        QByteArray bytes;QDataStream stream(&bytes,QIODevice::WriteOnly);stream.setVersion(QDataStream::Qt_6_0);stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
        stream<<r.duration<<qint32(r.frames);
        for(const auto& t:r.tracks){stream<<QJsonDocument(anchorJson(t.anchor)).toJson(QJsonDocument::Compact)<<qint32(t.parent)<<t.surface;
            for(const auto& p:t.poses)for(float value:{p.translation.x,p.translation.y,p.translation.z,p.rotation.x,p.rotation.y,p.rotation.z,p.rotation.w,p.scale.x,p.scale.y,p.scale.z})
                stream<<(value==0.f?0.f:value); // JSON canonicalizes negative zero.
        }
        hash.addData(bytes);return QString::fromLatin1(hash.result().toHex());
    }
    QByteArray encodeReloadRecipe(const ReloadRecipe& r,QString& error){
        error.clear();if(!validCapture(r,error)||!validWindows(r,error)||!identity(r.id)||reloadRecipeIdentity(r)!=r.id){if(error.isEmpty())error="Reload source identity differs from its capture";return {};}
        QJsonArray tracks,windows;
        for(const auto& t:r.tracks){QJsonArray poses;for(const auto& p:t.poses)poses.append(QJsonArray{p.translation.x,p.translation.y,p.translation.z,p.rotation.x,p.rotation.y,p.rotation.z,p.rotation.w,p.scale.x,p.scale.y,p.scale.z});tracks.append(QJsonObject{{"anchor",anchorJson(t.anchor)},{"label",t.label},{"parent",t.parent},{"surface",t.surface},{"poses",poses}});}
        for(const auto& w:r.windows){QJsonArray members;for(const auto& m:w.members)members.append(QJsonObject{{"track",m.track},{"relation",int(m.relation)},{"grabbable",m.grabbable},{"gripId",m.gripId}});windows.append(QJsonObject{{"label",w.label},{"first",w.first},{"last",w.last},{"returnOnRelease",w.returnOnRelease},{"members",members}});}
        const auto bytes=QJsonDocument(QJsonObject{{"format","reload-maker-reload-recipe"},{"version",1},{"space","weapon-local"},{"id",r.id},{"source",r.source},{"revision",r.revision},{"duration",r.duration},{"frames",r.frames},{"tracks",tracks},{"windows",windows}}).toJson(QJsonDocument::Compact);
        if(bytes.size()>maximumBytes){error="Reload recipe exceeds 96 MiB";return {};}return bytes;
    }
    bool decodeReloadRecipe(const QByteArray& bytes,ReloadRecipe& recipe,QString& error){
        error.clear();if(bytes.isEmpty()||bytes.size()>maximumBytes){error="Invalid reload recipe size";return false;}
        QJsonParseError parse;const auto d=QJsonDocument::fromJson(bytes,&parse);
        if(parse.error!=QJsonParseError::NoError||!d.isObject()||d["format"]!="reload-maker-reload-recipe"||d["version"].toDouble()!=1||d["space"]!="weapon-local"||!d["tracks"].isArray()||!d["windows"].isArray()){error="Invalid or unsupported reload recipe";return false;}
        ReloadRecipe r;r.id=d["id"].toString();r.source=d["source"].toObject();r.revision=d["revision"].toInt(-1);r.duration=float(d["duration"].toDouble());r.frames=d["frames"].toInt();
        if(r.frames<2||r.frames>maximumReloadFrames||d["tracks"].toArray().size()>maximumReloadTracks||d["windows"].toArray().size()>2048){error="Reload recipe exceeds supported bounds";return false;}
        for(const auto& value:d["tracks"].toArray()){
            const auto o=value.toObject();ReloadTrack t;t.anchor=readAnchor(o["anchor"].toObject());t.label=o["label"].toString();t.parent=o["parent"].toInt(-2);t.surface=o["surface"].toBool();
            if(!o["poses"].isArray()||o["poses"].toArray().size()!=r.frames){error="Reload track frame count differs";return false;}
            for(const auto& value:o["poses"].toArray()){const auto a=value.toArray();if(a.size()!=10){error="Invalid packed reload transform";return false;}for(const auto& number:a)if(!number.isDouble()){error="Non-numeric reload transform";return false;}
                t.poses.append(Transformf{{float(a[0].toDouble()),float(a[1].toDouble()),float(a[2].toDouble())},{float(a[3].toDouble()),float(a[4].toDouble()),float(a[5].toDouble()),float(a[6].toDouble())},{float(a[7].toDouble()),float(a[8].toDouble()),float(a[9].toDouble())}});
            }r.tracks.append(std::move(t));
        }
        for(const auto& value:d["windows"].toArray()){
            const auto o=value.toObject();ReloadWindow w;w.label=o["label"].toString();w.first=o["first"].toInt(-1);w.last=o["last"].toInt(-1);w.returnOnRelease=o["returnOnRelease"].toBool();
            if(!o["members"].isArray()){error="Window members missing";return false;}
            for(const auto& v:o["members"].toArray()){const auto m=v.toObject();w.members.append({m["track"].toInt(-1),ReloadRelation(m["relation"].toInt(-1)),m["grabbable"].toBool(),m["gripId"].toString()});}r.windows.append(w);
        }
        if(!validCapture(r,error)||!validWindows(r,error)||!identity(r.id)||reloadRecipeIdentity(r)!=r.id){if(error.isEmpty())error="Reload source identity differs from its capture";return false;}
        recipe=std::move(r);return true;
    }
    ReloadRecipeLibrary::ReloadRecipeLibrary(QString directory):_directory(std::move(directory)){}
    QString ReloadRecipeLibrary::defaultDirectory(){return QDir(HandPoseLibrary::defaultDirectory()).filePath("../reload-recipes");}
    QString ReloadRecipeLibrary::filePath(const QString& id)const{return identity(id)?QDir(_directory).filePath(id+".json"):QString();}
    bool ReloadRecipeLibrary::load(const QString& id,ReloadRecipe& recipe,QString& error)const{
        error.clear();const QString path=filePath(id);if(path.isEmpty()){error="Invalid recipe ID";return false;}QFile file(path);if(!file.exists())return false;
        if(file.size()>maximumBytes||!file.open(QIODevice::ReadOnly)){error="Cannot read reload recipe: "+file.errorString();return false;}
        ReloadRecipe loaded;if(!decodeReloadRecipe(file.read(maximumBytes+1),loaded,error))return false;
        if(loaded.id!=id){error="Reload recipe filename/source identity mismatch";return false;}recipe=std::move(loaded);return true;
    }
    bool ReloadRecipeLibrary::save(ReloadRecipe& recipe,QString& error)const{
        if(!identity(recipe.id)||recipe.revision==std::numeric_limits<int>::max()){error="Invalid recipe ID/revision";return false;}
        QDir directory(_directory);if(!directory.mkpath(".")){error="Cannot create reload recipe directory";return false;}
        QLockFile lock(directory.filePath(recipe.id+".lock"));if(!lock.tryLock(0)){error="Reload recipe is being saved by another editor";return false;}
        ReloadRecipe existing;const bool found=load(recipe.id,existing,error);if(!error.isEmpty())return false;
        if((found&&existing.revision!=recipe.revision)||(!found&&recipe.revision!=0)){error="Reload recipe changed elsewhere; reload before saving";return false;}
        auto next=recipe;++next.revision;const auto bytes=encodeReloadRecipe(next,error);if(bytes.isEmpty())return false;
        QSaveFile file(filePath(recipe.id));file.setDirectWriteFallback(false);
        if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit()){error=file.errorString();return false;}
        recipe.revision=next.revision;return true;
    }
}
