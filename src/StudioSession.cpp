#include "StudioSession.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
namespace reloadmaker {
QString workspacePipe(const QString& root) {
    const QFileInfo info(root);
    auto resolved=info.canonicalFilePath();
    if(resolved.isEmpty())resolved=info.absoluteFilePath();
    const auto normalized=QDir::fromNativeSeparators(resolved).toCaseFolded().toUtf8();
    return "ReloadMakerV2."+QString::fromLatin1(QCryptographicHash::hash(normalized,QCryptographicHash::Sha256).toHex().left(24));
}
}
