#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <functional>

namespace reloadmaker {
struct SourceBytes {QByteArray bytes;QString provider,error;};
using SourceReader=std::function<SourceBytes(const QString&,const QString&)>;
// Explicit authoring check, never called in a render/animation tick. Content
// identity is independent of the label/path of the winning MO2 provider.
QJsonObject inspectSourceEvidence(const QJsonObject& expected,const SourceReader& read);
bool validSourceEvidence(const QJsonObject&,QString& error);
}
