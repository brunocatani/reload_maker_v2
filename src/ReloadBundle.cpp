#include "ReloadBundle.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QUuid>

namespace reloadmaker {
QString magazineGrabKey(const QString& weapon,const QString& magazine){return weapon.toLower()+"|"+magazine.toLower();}
bool magazineGrabSupported(const HandPosePlacement& anchor) {
    return anchor.anchorName.compare("WeaponMagazine",Qt::CaseInsensitive)==0;
}
std::optional<PartGrip> magazineGripFor(const QString& weapon,const HandPosePlacement& anchor,
    const QVector<PartGrip>& library,const QHash<QString,QString>& assignments,
    const QHash<QString,QString>& defaults,QString& error) {
    error.clear();if(!magazineGrabSupported(anchor))return {};
    const auto key=magazineGrabKey(weapon,anchor.ownerRecord);
    const bool custom=assignments.contains(key);
    const auto id=custom?assignments.value(key):defaults.value(weapon.toLower());
    if(id.isEmpty())return {};
    const PartGrip* found=nullptr;
    for(const auto& grip:library)if(grip.pose.id==id){if(found){error="Duplicate magazine grip ID";return {};}found=&grip;}
    if(!found||found->weaponRecord.compare(weapon,Qt::CaseInsensitive)!=0||!magazineGrabSupported(found->pose.placement)) {
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
    for(const auto& track:recipe.tracks)if(magazineGrabSupported(track.anchor))anchors.append(track.anchor);
    for(const auto& anchor:anchors) {
        const auto key=magazineGrabKey(weapon,anchor.ownerRecord);
        if(magazineGrabs.contains(key))continue;
        auto fit=magazineGripFor(weapon,anchor,savedLibrary,savedGrabs,defaults,error);
        if(!error.isEmpty())return {};
        if(fit){magazineGrabs[key]=fit->pose.id;library.append(*fit);}
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
    auto keys=magazineGrabs.keys();keys.sort();
    for(const auto& key:keys)if(key.startsWith(weapon.toLower()+"|")) {
        const auto* grip=findGrip(magazineGrabs[key]);if(!grip)return {};
        if(key!=magazineGrabKey(weapon,grip->pose.placement.ownerRecord)||!magazineGrabSupported(grip->pose.placement)) {
            error="Magazine grab assignment does not match a supported magazine: "+grip->pose.name;return {};
        }
        if(!include(*grip))return {};
        assignments.append(QJsonObject{{"ownerRecord",grip->pose.placement.ownerRecord},{"ownerModel",grip->pose.placement.ownerModel},{"gripId",grip->pose.id}});
    }
    return QJsonDocument(QJsonObject{{"format","reload-maker-reload-bundle"},{"version",1},{"weaponRecord",weapon},
        {"recipe",QJsonDocument::fromJson(raw).object()},{"grips",grips},{"mechanisms",parts},{"magazineGrabBindings",assignments}}).toJson(QJsonDocument::Indented);
}
}
