#include "ReloadRecipe.h"
#include "PartGrip.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>

namespace reloadmaker {
    namespace {
        constexpr qint64 maximumBytes=96*1024*1024;
        constexpr int kRecipeVersion=2;
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
        QJsonObject anchorJson(const HandPosePlacement& a){return {{"kind",a.anchorKind},{"name",a.anchorName},{"path",a.anchorPath},{"model",a.ownerModel},{"record",a.ownerRecord}};}
        HandPosePlacement readAnchor(const QJsonObject& a){return {a["kind"].toString(),a["name"].toString(),a["path"].toString(),a["model"].toString(),a["record"].toString(),{}, {}};}
        bool validCapture(const ReloadRecipe& r,QString& error){
            if(r.source["action"]!="WPNReload"||!std::isfinite(r.duration)||r.duration<=0||r.frames<2||r.frames>maximumReloadFrames||r.tracks.isEmpty()||r.tracks.size()>maximumReloadTracks){error="Invalid base-reload source or capture size/duration";return false;}
            for(int i=0;i<r.tracks.size();++i){const auto& t=r.tracks[i];
                if(t.poses.size()!=r.frames||t.label.isEmpty()||t.label.size()>512||t.anchor.ownerRecord.isEmpty()||t.anchor.ownerModel.isEmpty()||t.anchor.anchorPath.isEmpty()||t.parent< -1||t.parent>=r.tracks.size()||t.parent==i){error="Invalid captured part identity or track";return false;}
                int ancestor=t.parent;
                for(int depth=0;ancestor>=0;++depth){if(depth>=r.tracks.size()||ancestor>=r.tracks.size()||ancestor==i){error="Cyclic captured part ancestry";return false;}ancestor=r.tracks[ancestor].parent;}
                for(int j=0;j<i;++j)if(sameGripAnchor(t.anchor,r.tracks[j].anchor)){error="Duplicate captured part identity";return false;}
                for(const auto& p:t.poses)if(!finite(p)){error="Non-finite/invalid captured transform";return false;}
            }
            return true;
        }
        QVector<ReloadSpan> movingSpans(const ReloadTrack& track,int stable){
            QVector<ReloadSpan> spans;const auto& poses=track.poses;int rest=0,start=-1,peak=0;float excursion=0;int retreat=0;
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
    }
    float reloadPoseDistance(const Transformf& a,const Transformf& b){return length(subtract(a.translation,b.translation))+3*angle(a.rotation,b.rotation);}
    bool reloadPoseVisible(const Transformf& pose){return std::abs(pose.scale.x)>1e-5f&&std::abs(pose.scale.y)>1e-5f&&std::abs(pose.scale.z)>1e-5f;}
    Transformf reloadTrackPose(const ReloadRecipe& r,int track,float seconds){
        if(track<0||track>=r.tracks.size()||r.frames<2||r.duration<=0)return {};
        const float position=std::clamp(seconds/r.duration,0.f,1.f)*float(r.frames-1);const int first=int(position),last=std::min(first+1,r.frames-1);
        return interpolatedTransform(r.tracks[track].poses[first],r.tracks[track].poses[last],position-first);
    }
    bool identifyReloadRecipe(ReloadRecipe& r,QString& error)
    {
        error.clear();
        if (!validCapture(r, error)) {
            return false;
        }
        r.id = reloadRecipeIdentity(r);
        return true;
    }

    QVector<ReloadSpan> reloadMotionSpans(const ReloadRecipe& r,int track)
    {
        if (track < 0 || track >= r.tracks.size() || r.frames < 2 || !(r.duration > 0)) {
            return {};
        }
        // A rest lasts at least about 67 ms, whatever the capture rate.
        const int stable = std::max(2, int(std::round(.067f * (r.frames - 1) / r.duration)));
        return movingSpans(r.tracks[track], stable);
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
    // Every recipe starts at the first frame of its source; nothing is saved
    // between sessions, so there is no revision. Version 1 also held reload
    // windows, which were retired.
    QByteArray encodeReloadRecipe(const ReloadRecipe& r,QString& error)
    {
        error.clear();
        if (!validCapture(r, error)) {
            return {};
        }
        QJsonArray tracks;
        for (const auto& t : r.tracks) {
            QJsonArray poses;
            for (const auto& p : t.poses) {
                poses.append(QJsonArray{ p.translation.x, p.translation.y, p.translation.z, p.rotation.x, p.rotation.y, p.rotation.z,
                                         p.rotation.w, p.scale.x, p.scale.y, p.scale.z });
            }
            tracks.append(QJsonObject{ { "anchor", anchorJson(t.anchor) }, { "label", t.label }, { "parent", t.parent },
                                       { "surface", t.surface }, { "poses", poses } });
        }
        const QJsonObject recipe{ { "format", "reload-maker-reload-recipe" }, { "version", kRecipeVersion }, { "space", "weapon-local" },
                                  { "id", reloadRecipeIdentity(r) }, { "source", r.source }, { "duration", r.duration }, { "frames", r.frames },
                                  { "tracks", tracks } };
        const auto bytes = QJsonDocument(recipe).toJson(QJsonDocument::Compact);
        if (bytes.size() > maximumBytes) {
            error = "Reload recipe exceeds 96 MiB";
            return {};
        }
        return bytes;
    }

    bool decodeReloadRecipe(const QByteArray& bytes,ReloadRecipe& recipe,QString& error)
    {
        error.clear();
        if (bytes.isEmpty() || bytes.size() > maximumBytes) {
            error = "Invalid reload recipe size";
            return false;
        }
        QJsonParseError parse;
        const auto d = QJsonDocument::fromJson(bytes, &parse);
        if (parse.error != QJsonParseError::NoError || !d.isObject() || d["format"] != "reload-maker-reload-recipe" ||
            d["version"].toDouble() != kRecipeVersion || d["space"] != "weapon-local" || !d["tracks"].isArray()) {
            error = "Invalid or unsupported reload recipe";
            return false;
        }
        ReloadRecipe r;
        r.source = d["source"].toObject();
        r.duration = float(d["duration"].toDouble());
        r.frames = d["frames"].toInt();
        if (r.frames < 2 || r.frames > maximumReloadFrames || d["tracks"].toArray().size() > maximumReloadTracks) {
            error = "Reload recipe exceeds supported bounds";
            return false;
        }
        for (const auto& value : d["tracks"].toArray()) {
            const auto o = value.toObject();
            ReloadTrack t;
            t.anchor = readAnchor(o["anchor"].toObject());
            t.label = o["label"].toString();
            t.parent = o["parent"].toInt(-2);
            t.surface = o["surface"].toBool();
            if (!o["poses"].isArray() || o["poses"].toArray().size() != r.frames) {
                error = "Reload track frame count differs";
                return false;
            }
            for (const auto& packed : o["poses"].toArray()) {
                const auto a = packed.toArray();
                if (a.size() != 10) {
                    error = "Invalid packed reload transform";
                    return false;
                }
                for (const auto& number : a) {
                    if (!number.isDouble()) {
                        error = "Non-numeric reload transform";
                        return false;
                    }
                }
                t.poses.append(Transformf{ { float(a[0].toDouble()), float(a[1].toDouble()), float(a[2].toDouble()) },
                                           { float(a[3].toDouble()), float(a[4].toDouble()), float(a[5].toDouble()), float(a[6].toDouble()) },
                                           { float(a[7].toDouble()), float(a[8].toDouble()), float(a[9].toDouble()) } });
            }
            r.tracks.append(std::move(t));
        }
        if (!validCapture(r, error)) {
            return false;
        }
        // The id names the recipe's content; it is derived, never compared
        // with a stored one (owner, 2026-10-05).
        r.id = reloadRecipeIdentity(r);
        recipe = std::move(r);
        return true;
    }
    QJsonArray recipeTrackAnchors(const ReloadRecipe& recipe)
    {
        QJsonArray anchors;
        for (const auto& track : recipe.tracks) {
            anchors.append(anchorJson(track.anchor));
        }
        return anchors;
    }
    int mapRecipeTrack(const QJsonArray& savedAnchors, const ReloadRecipe& recipe, int index)
    {
        if (index < 0) {
            return -1;
        }
        if (savedAnchors.isEmpty()) {
            return index < recipe.tracks.size() ? index : -1;
        }
        if (index >= savedAnchors.size()) {
            return -1;
        }
        const auto saved = savedAnchors[index].toObject();
        const auto same = [&](const QString& a, const char* key) { return a.compare(saved[key].toString(), Qt::CaseInsensitive) == 0; };
        int found = -1;
        for (const bool withModel : { true, false }) {
            int matches = 0;
            for (int i = 0; i < recipe.tracks.size(); ++i) {
                const auto& anchor = recipe.tracks[i].anchor;
                if (same(anchor.anchorName, "name") && same(anchor.anchorPath, "path") && (!withModel || same(anchor.ownerModel, "model"))) {
                    found = i;
                    ++matches;
                }
            }
            if (matches == 1) {
                return found;
            }
        }
        return -1;
    }
}
