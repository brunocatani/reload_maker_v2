#include "ReloadBundle.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QUuid>
#include <algorithm>

namespace reloadmaker {
QString magazineGrabKey(const QString& weapon,const QString& magazine){return weapon.toLower()+"|"+magazine.toLower();}
QString handAssignmentKey(const QString& key,PoseHand hand){return key.toLower()+(hand==PoseHand::Left?"|left":"|right");}
bool magazineGrabSupported(const HandPosePlacement& anchor) {
    return anchor.anchorName.compare("WeaponMagazine",Qt::CaseInsensitive)==0||
        (anchor.anchorKind=="installed-part"&&anchor.anchorName.compare("Mag",Qt::CaseInsensitive)==0);
}
std::optional<PartGrip> magazineGripFor(const QString& weapon,const HandPosePlacement& anchor,
    const QVector<PartGrip>& library,const QHash<QString,QString>& assignments,
    const QHash<QString,QString>& defaults,QString& error,std::optional<PoseHand> hand) {
    error.clear();if(!magazineGrabSupported(anchor))return {};
    const auto key=magazineGrabKey(weapon,anchor.ownerRecord);
    const auto forHand=[&](const QHash<QString,QString>& values,const QString& base) {
        if(hand&&values.contains(handAssignmentKey(base,*hand)))return values.value(handAssignmentKey(base,*hand));
        const auto legacy=values.value(base);
        if(hand)for(const auto& grip:library)if(grip.pose.id==legacy&&grip.pose.hand!=*hand)return QString();
        return legacy;
    };
    const auto customId=forHand(assignments,key);
    const bool custom=!customId.isEmpty();
    const auto id=custom?customId:forHand(defaults,weapon.toLower());
    if(id.isEmpty())return {};
    const PartGrip* found=nullptr;
    for(const auto& grip:library)if(grip.pose.id==id){if(found){error="Duplicate magazine grip ID";return {};}found=&grip;}
    if(!found||found->weaponRecord.compare(weapon,Qt::CaseInsensitive)!=0||!magazineGrabSupported(found->pose.placement)||(hand&&found->pose.hand!=*hand)) {
        error="The magazine fit is missing or belongs to another weapon";return {};
    }
    if(custom) {
        if(!sameGripAnchor(found->pose.placement,anchor)){error="The assigned fit targets a different magazine model or frame";return {};}
        return *found;
    }
    PartGrip fit=*found;
    fit.pose.id=QUuid::createUuidV5(QUuid(id),(key+"|"+anchor.ownerModel.toLower()+"|"+anchor.anchorPath).toUtf8()).toString(QUuid::WithoutBraces);
    const auto placement=fit.pose.placement.handInAnchor;
    fit.pose.placement=anchor;fit.pose.placement.handInAnchor=placement;
    fit.mechanismId.clear();fit.surfaceContact.reset();
    return fit;
}
QByteArray encodeReloadBundle(const ReloadRecipe& recipe,const QVector<PartGrip>& savedLibrary,
    const QVector<PartMechanism>& mechanisms,const QHash<QString,QString>& savedGrabs,QString& error,
    const QHash<QString,QString>& defaults) {
    error.clear();const auto raw=encodeReloadRecipe(recipe,error);if(raw.isEmpty())return {};
    const QString weapon=recipe.source["weaponRecord"].toString();
    auto library=savedLibrary;auto magazineGrabs=savedGrabs;
    QVector<HandPosePlacement> anchors;
    for(const auto& grip:savedLibrary)if(grip.weaponRecord.compare(weapon,Qt::CaseInsensitive)==0&&magazineGrabSupported(grip.pose.placement))anchors.append(grip.pose.placement);
    for(const auto& track:recipe.tracks)if(magazineGrabSupported(track.anchor)) {
        // RU556's WeaponMagazine belongs to the receiver. Its installed Mag
        // attachment owns the grip; never materialize a magazine default on
        // that receiver just because the reload animates its parent frame.
        const bool receiverFrame=std::any_of(recipe.tracks.cbegin(),recipe.tracks.cend(),[&](const auto& other){
            return other.anchor.anchorName.compare("WeaponBolt",Qt::CaseInsensitive)==0&&
                other.anchor.ownerRecord.compare(track.anchor.ownerRecord,Qt::CaseInsensitive)==0&&
                other.anchor.ownerModel.compare(track.anchor.ownerModel,Qt::CaseInsensitive)==0;
        });
        if(!receiverFrame)anchors.append(track.anchor);
    }
    for(const auto& anchor:anchors)for(const auto hand:{PoseHand::Left,PoseHand::Right}) {
        const auto key=magazineGrabKey(weapon,anchor.ownerRecord);
        auto fit=magazineGripFor(weapon,anchor,savedLibrary,savedGrabs,defaults,error,hand);
        if(!error.isEmpty())return {};
        if(fit&&!magazineGrabs.values().contains(fit->pose.id)) {
            const auto assigned=magazineGrabs.contains(key)||defaults.contains(handAssignmentKey(weapon,hand))?handAssignmentKey(key,hand):key;
            magazineGrabs[assigned]=fit->pose.id;
            if(!std::any_of(library.cbegin(),library.cend(),[&](const auto& g){return g.pose.id==fit->pose.id;}))library.append(*fit);
        }
    }
    QJsonArray grips,parts,assignments;QSet<QString> included,definitions;
    const auto findGrip=[&](const QString& id)->const PartGrip* {
        const PartGrip* result=nullptr;
        for(const auto& grip:library)if(grip.pose.id==id){if(result){error="Duplicate grip ID: "+id;return nullptr;}result=&grip;}
        if(!result)error="Reload references a missing grip: "+id;
        else if(result->weaponRecord.compare(weapon,Qt::CaseInsensitive)!=0){error="Reload grip belongs to another weapon: "+result->pose.name;return nullptr;}
        return result;
    };
    const auto include=[&](const PartGrip& grip)->bool {
        if(included.contains(grip.pose.id))return true;
        const auto bytes=encodePartGrip(grip,error);if(bytes.isEmpty())return false;
        if(!grip.mechanismId.isEmpty()) {
            const PartMechanism* part=nullptr;
            for(const auto& candidate:mechanisms)if(candidate.id==grip.mechanismId){if(part){error="Duplicate part definition";return false;}part=&candidate;}
            if(!part||part->weaponRecord.compare(weapon,Qt::CaseInsensitive)!=0||!sameMechanismPart(part->anchor,grip.pose.placement)) {
                error="Grip has a missing or mismatched part definition: "+grip.pose.name;return false;
            }
            if(!definitions.contains(part->id)) {
                const auto encoded=encodeMechanism(*part,error);if(encoded.isEmpty())return false;
                parts.append(QJsonDocument::fromJson(encoded).object());definitions.insert(part->id);
            }
        }
        grips.append(QJsonDocument::fromJson(bytes).object());included.insert(grip.pose.id);return true;
    };
    for(const auto& window:recipe.windows)for(const auto& member:window.members)if(!member.gripId.isEmpty()) {
        const auto* grip=findGrip(member.gripId);if(!grip)return {};
        if(!sameMechanismPart(grip->pose.placement,recipe.tracks[member.track].anchor)) {
            error="Reload phase grip targets a different part: "+grip->pose.name;return {};
        }
        if(!include(*grip))return {};
    }
    bool bothHands=false;QSet<QString> magazineOwners;auto keys=magazineGrabs.keys();keys.sort();
    for(const auto& key:keys)if(key.startsWith(weapon.toLower()+"|")) {
        const auto* grip=findGrip(magazineGrabs[key]);if(!grip)return {};
        const auto base=magazineGrabKey(weapon,grip->pose.placement.ownerRecord),handKey=handAssignmentKey(base,grip->pose.hand);
        if(key==base&&magazineGrabs.contains(handKey))continue;
        if((key!=base&&key!=handKey)||!magazineGrabSupported(grip->pose.placement)) {
            error="Magazine grab assignment does not match a supported magazine: "+grip->pose.name;return {};
        }
        if(!include(*grip))return {};
        bothHands|=magazineOwners.contains(base);magazineOwners.insert(base);
        assignments.append(QJsonObject{{"ownerRecord",grip->pose.placement.ownerRecord},{"ownerModel",grip->pose.placement.ownerModel},{"gripId",grip->pose.id},{"hand",grip->pose.hand==PoseHand::Left?"left":"right"}});
    }
    return QJsonDocument(QJsonObject{{"format","reload-maker-reload-bundle"},{"version",bothHands?2:1},{"weaponRecord",weapon},
        {"recipe",QJsonDocument::fromJson(raw).object()},{"grips",grips},{"mechanisms",parts},{"magazineGrabBindings",assignments}}).toJson(QJsonDocument::Indented);
}
}
