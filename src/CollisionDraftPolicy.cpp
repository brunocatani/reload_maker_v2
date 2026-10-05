#include "CollisionDraftPolicy.h"
#include <QRegularExpression>

namespace reloadmaker {
bool validCollisionDrafts(const QJsonObject& drafts) {
    const QRegularExpression edit("^[a-f0-9]{32}$");
    if(drafts.size()>2)return false;
    for(auto it=drafts.begin();it!=drafts.end();++it){const auto item=it.value().toObject();if((it.key()!="full"&&it.key()!="empty")||item.size()!=2||!edit.match(item["edit"].toString()).hasMatch()||!item["usable"].isBool())return false;}
    return true;
}
}
