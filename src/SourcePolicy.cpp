#include "SourcePolicy.h"
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
namespace reloadmaker {
bool validSourcePolicy(const QJsonObject& policy,QString& error) {
    const auto key=[](const QJsonValue& value){return value.isString()&&value.toString().size()<=261&&QRegularExpression(R"(^[^:/\\\x00]+\.(?:esp|esm|esl):[0-9a-f]{6}$)",QRegularExpression::CaseInsensitiveOption).match(value.toString()).hasMatch()&&!value.toString().endsWith(":000000");};
    for(auto it=policy.begin();it!=policy.end();++it)if(it.key()!="sourceOverride"&&it.key()!="blockedMods"&&it.key()!="magazineCollections"){error="Unknown source compatibility rule";return false;}
    if(policy.contains("sourceOverride")) {
        const auto value=policy["sourceOverride"].toObject();
        if(!policy["sourceOverride"].isObject()||value.size()!=2||!key(value["marker"])||!value["present"].isBool()){error="The add-on condition needs a plugin:local-ID marker and an installed/absent choice";return false;}
    }
    QSet<QString> roles;
    for(const auto* field:{"blockedMods","magazineCollections"})if(policy.contains(field)) {
        const auto list=policy[field].toArray();
        if(!policy[field].isArray()||list.size()>32){error="Use at most 32 records per compatibility rule";return false;}
        for(const auto& value:list){const auto record=value.toString().toLower();
            if(!key(value)||roles.contains(record)){error="Compatibility records must be valid and have one role";return false;}roles.insert(record);}
    }
    error.clear();return true;
}
}
