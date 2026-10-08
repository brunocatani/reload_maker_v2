#include "WeaponAmmunition.h"
#include "SourcePolicy.h"
#include <QSet>
#include <bit>
#include <cmath>
#include <optional>

namespace reloadmaker {
WeaponAmmunition resolveWeaponAmmunition(const PluginContent& plugin,const WeaponRecord& weapon,
    const WeaponConfiguration& configuration,const QHash<QString,QString>& referenceNames) {
    WeaponAmmunition result;result.ammo=weapon.ammo;result.capacity=weapon.ammoCapacity;result.automatic=weapon.automatic;
    result.repeatableSingleFire=weapon.repeatableSingleFire;
    const auto fail=[&](const QString& message){result.error=message;return result;};
    // Uncertain combinations warn and take a best-effort value: the later
    // part wins a Set conflict, a ranged or rank-scaled value uses its base.
    const auto warn=[&](const QString& message){if(!result.warnings.contains(message))result.warnings.append(message);};
    if(!weapon.ammoDataKnown)return fail("Weapon ammunition data is missing or truncated.");
    result.sources.append(QJsonObject{{"record",recordIdentity(plugin,weapon.formId)},{"capacity",weapon.ammoCapacity},{"ammo",recordIdentity(plugin,weapon.ammo)}});
    std::optional<int> setCapacity;std::optional<std::uint32_t> setAmmo;std::int64_t added=0;bool hasAdd=false;
    std::optional<float> capacityMultiplier;
    std::optional<bool> setAutomatic,setRepeatableSingleFire;
    QSet<std::uint32_t> keywords(weapon.keywords.cbegin(),weapon.keywords.cend()),addedKeywords,removedKeywords;
    for(auto id:configuration.activeMods()) {
        const auto* mod=configuration.mod(id);if(!mod)continue;
        if(auxiliaryReloadMode(recordIdentity(plugin,id)))continue;
        if(!mod->metadataWarning.isEmpty())warn("Ammunition properties of "+mod->editorId+" are partly unreadable: "+mod->metadataWarning);
        for(auto keyword:mod->removedKeywords)removedKeywords.insert(keyword);
        for(auto keyword:mod->addedKeywords)addedKeywords.insert(keyword);
        for(const auto& p:mod->firingProperties) {
            if((p.property!=25&&p.property!=86)||p.valueType!=2||p.function!=0||p.value1>1||p.stepBits!=0){warn("Unsupported firing property in "+mod->editorId+" is ignored.");continue;}
            const bool value=p.value1!=0;
            auto& selected=p.property==25?setAutomatic:setRepeatableSingleFire;
            if(selected&&*selected!=value)warn("Selected parts set opposite fire modes; "+mod->editorId+" is taken.");
            selected=value;
        }
        for(const auto& p:mod->ammoProperties) {
            if(p.stepBits!=0)warn("Rank-scaled ammunition property in "+mod->editorId+" uses its base value.");
            if(p.property==12) {
                if(p.value2!=0)warn("Ranged capacity in "+mod->editorId+" uses its first value.");
                if(p.function==1&&(p.valueType==0||p.valueType==1)) {
                    // F4SE VR ObjectMod's Mul_Add operator uses float storage
                    // even for integer targets. A 1.0 modifier adds the base
                    // capacity once (the vanilla 10mm large mag: 12 -> 24).
                    const float value=std::bit_cast<float>(p.value1);
                    if(!std::isfinite(value)){warn("Non-finite capacity multiplier in "+mod->editorId+" is ignored.");continue;}
                    if(capacityMultiplier)warn("Several capacity multipliers are summed.");
                    capacityMultiplier=capacityMultiplier.value_or(0.f)+value;
                    result.sources.append(QJsonObject{{"record",recordIdentity(plugin,id)},{"name",mod->name},{"capacityOperation","multiply-add"},{"value",value}});
                    continue;
                }
                if(p.valueType!=0||(p.function!=0&&p.function!=2)){warn("Unsupported capacity operation in "+mod->editorId+" is ignored.");continue;}
                const auto value=static_cast<std::int32_t>(p.value1);
                if(p.function==0){if(setCapacity&&*setCapacity!=value)warn("Selected parts set different capacities; "+mod->editorId+" is taken.");setCapacity=value;}
                else {hasAdd=true;added+=value;}
                result.sources.append(QJsonObject{{"record",recordIdentity(plugin,id)},{"name",mod->name},{"capacityOperation",p.function==0?"set":"add"},{"value",value}});
            } else if(p.property==61) {
                // FormID,Int stores the selected form in value1. Its companion
                // integer is not an alternate capacity or a rank range.
                if(p.valueType!=4||p.function!=0){warn("Unsupported ammo-type operation in "+mod->editorId+" is ignored.");continue;}
                if(setAmmo&&*setAmmo!=p.value1)warn("Selected parts set different ammunition; "+mod->editorId+" is taken.");
                setAmmo=p.value1;result.sources.append(QJsonObject{{"record",recordIdentity(plugin,id)},{"ammo",recordIdentity(plugin,p.value1)}});
            }
        }
    }
    // Mixed operations: set, then add, then the multiplier on the base.
    if((setCapacity&&hasAdd)||(capacityMultiplier&&(setCapacity||hasAdd)))warn("Mixed capacity operations are applied as set, add, then multiply.");
    double capacity=setCapacity?double(*setCapacity):double(weapon.ammoCapacity);
    capacity+=double(added);
    if(capacityMultiplier)capacity+=double(float(weapon.ammoCapacity)**capacityMultiplier);
    if(!std::isfinite(capacity)||capacity<1||capacity>65535)return fail("Configured magazine capacity is outside the supported range.");
    result.capacity=int(capacity);if(setAmmo)result.ammo=*setAmmo;
    if(setAutomatic)result.automatic=*setAutomatic;
    if(setRepeatableSingleFire)result.repeatableSingleFire=*setRepeatableSingleFire;
    if(!result.ammo)return fail("This configuration has no ammunition type.");
    result.ammoRecord=recordIdentity(plugin,result.ammo);
    result.ammoName=plugin.ammoNames.value(result.ammo,referenceNames.value(result.ammoRecord.toLower()));
    if(result.ammoName.isEmpty())result.ammoName=result.ammoRecord;
    // Configuration slots are sorted for display, not engine evaluation order.
    // Contradictory keyword edits cannot decide the pistol policy by UI sorting.
    if(addedKeywords.intersects(removedKeywords))warn("Selected parts both add and remove a keyword; the addition is taken.");
    keywords.subtract(removedKeywords);keywords.unite(addedKeywords);
    for(auto id:keywords) {
        const auto name=plugin.keywordNames.value(id,referenceNames.value(recordIdentity(plugin,id).toLower()));
        if(name.compare("WeaponTypePistol",Qt::CaseInsensitive)==0||name.compare("AnimsGripPistol",Qt::CaseInsensitive)==0)result.pistol=true;
    }
    return result;
}
QJsonObject WeaponAmmunition::json()const {
    return {{"resolved",valid()},{"capacity",capacity},{"ammoRecord",ammoRecord},{"ammoName",ammoName},{"pistolSlideLock",pistol},{"automatic",automatic},{"repeatableSingleFire",repeatableSingleFire},{"sources",sources},{"error",error},{"warnings",QJsonArray::fromStringList(warnings)}};
}
}
