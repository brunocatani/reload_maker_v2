#include "WeaponAmmunition.h"
#include <QSet>
#include <bit>
#include <cmath>
#include <optional>

namespace reloadmaker {
WeaponAmmunition resolveWeaponAmmunition(const PluginContent& plugin,const WeaponRecord& weapon,
    const WeaponConfiguration& configuration,const QHash<QString,QString>& referenceNames) {
    WeaponAmmunition result;result.ammo=weapon.ammo;result.capacity=weapon.ammoCapacity;result.automatic=weapon.automatic;
    const auto fail=[&](const QString& message){result.error=message;return result;};
    if(!weapon.ammoDataKnown)return fail("Weapon ammunition data is missing or truncated.");
    result.sources.append(QJsonObject{{"record",recordIdentity(plugin,weapon.formId)},{"capacity",weapon.ammoCapacity},{"ammo",recordIdentity(plugin,weapon.ammo)}});
    std::optional<int> setCapacity;std::optional<std::uint32_t> setAmmo;std::int64_t added=0;bool hasAdd=false;
    std::optional<float> capacityMultiplier;
    std::optional<bool> setAutomatic;
    QSet<std::uint32_t> keywords(weapon.keywords.cbegin(),weapon.keywords.cend()),addedKeywords,removedKeywords;
    for(auto id:configuration.activeMods()) {
        const auto* mod=configuration.mod(id);if(!mod)continue;
        if(!mod->metadataWarning.isEmpty())return fail("Cannot fully resolve ammunition properties for "+mod->editorId+": "+mod->metadataWarning);
        for(auto keyword:mod->removedKeywords)removedKeywords.insert(keyword);
        for(auto keyword:mod->addedKeywords)addedKeywords.insert(keyword);
        for(const auto& p:mod->firingProperties) {
            if(p.valueType!=2||p.function!=0||p.value1>1||p.stepBits!=0)return fail("Unsupported automatic-fire property in "+mod->editorId);
            const bool value=p.value1!=0;
            if(setAutomatic&&*setAutomatic!=value)return fail("Conflicting automatic-fire overrides in selected parts.");
            setAutomatic=value;
        }
        for(const auto& p:mod->ammoProperties) {
            if(p.stepBits!=0)return fail("Rank-scaled ammunition property needs explicit support: "+mod->editorId);
            if(p.property==12) {
                if(p.value2!=0)return fail("Ranged capacity property needs explicit support: "+mod->editorId);
                if(p.function==1&&(p.valueType==0||p.valueType==1)) {
                    // F4SE VR ObjectMod's Mul_Add operator uses float storage
                    // even for integer targets. A 1.0 modifier adds the base
                    // capacity once (the vanilla 10mm large mag: 12 -> 24).
                    const float value=std::bit_cast<float>(p.value1);
                    if(!std::isfinite(value))return fail("Non-finite capacity multiplier in "+mod->editorId);
                    if(capacityMultiplier)return fail("Multiple capacity multipliers need verified evaluation order.");
                    capacityMultiplier=value;
                    result.sources.append(QJsonObject{{"record",recordIdentity(plugin,id)},{"name",mod->name},{"capacityOperation","multiply-add"},{"value",value}});
                    continue;
                }
                if(p.valueType!=0||(p.function!=0&&p.function!=2))return fail("Unsupported capacity operation in "+mod->editorId);
                const auto value=static_cast<std::int32_t>(p.value1);
                if(p.function==0){if(setCapacity&&*setCapacity!=value)return fail("Conflicting capacity overrides in selected parts.");setCapacity=value;}
                else {hasAdd=true;added+=value;}
                result.sources.append(QJsonObject{{"record",recordIdentity(plugin,id)},{"name",mod->name},{"capacityOperation",p.function==0?"set":"add"},{"value",value}});
            } else if(p.property==61) {
                // FormID,Int stores the selected form in value1. Its companion
                // integer is not an alternate capacity or a rank range.
                if(p.valueType!=4||p.function!=0)return fail("Unsupported ammo-type operation in "+mod->editorId);
                if(setAmmo&&*setAmmo!=p.value1)return fail("Conflicting ammo overrides in selected parts.");
                setAmmo=p.value1;result.sources.append(QJsonObject{{"record",recordIdentity(plugin,id)},{"ammo",recordIdentity(plugin,p.value1)}});
            }
        }
    }
    if(setCapacity&&hasAdd)return fail("Mixed set/add capacity modifiers need verified evaluation order.");
    if(capacityMultiplier&&(setCapacity||hasAdd))return fail("Mixed capacity multiplier and set/add modifiers need verified evaluation order.");
    const double capacity=capacityMultiplier?double(float(weapon.ammoCapacity)*(1.f+*capacityMultiplier)):
        setCapacity?double(*setCapacity):double(std::int64_t(weapon.ammoCapacity)+added);
    if(!std::isfinite(capacity)||capacity<1||capacity>65535)return fail("Configured magazine capacity is outside the supported range.");
    result.capacity=int(capacity);if(setAmmo)result.ammo=*setAmmo;
    if(setAutomatic)result.automatic=*setAutomatic;
    if(!result.ammo)return fail("This configuration has no ammunition type.");
    result.ammoRecord=recordIdentity(plugin,result.ammo);
    result.ammoName=plugin.ammoNames.value(result.ammo,referenceNames.value(result.ammoRecord.toLower()));
    if(result.ammoName.isEmpty())result.ammoName=result.ammoRecord;
    // Configuration slots are sorted for display, not engine evaluation order.
    // Contradictory keyword edits cannot decide the pistol policy by UI sorting.
    if(addedKeywords.intersects(removedKeywords))return fail("Conflicting keyword edits in selected parts need verified evaluation order.");
    keywords.subtract(removedKeywords);keywords.unite(addedKeywords);
    for(auto id:keywords) {
        const auto name=plugin.keywordNames.value(id,referenceNames.value(recordIdentity(plugin,id).toLower()));
        if(name.compare("WeaponTypePistol",Qt::CaseInsensitive)==0||name.compare("AnimsGripPistol",Qt::CaseInsensitive)==0)result.pistol=true;
    }
    return result;
}
QJsonObject WeaponAmmunition::json()const {
    return {{"resolved",valid()},{"capacity",capacity},{"ammoRecord",ammoRecord},{"ammoName",ammoName},{"pistolSlideLock",pistol},{"automatic",automatic},{"sources",sources},{"error",error}};
}
}
