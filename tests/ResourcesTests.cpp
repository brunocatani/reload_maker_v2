#include "PackageSource.h"
#include "SourceEvidence.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>
#include <QVariant>
#include <stdexcept>

using namespace reloadmaker;

namespace
{
    void require(bool value, const char* message)
    {
        if (!value) { throw std::runtime_error(message); }
    }

    QByteArray contents(const QString& path)
    {
        QFile file(path);
        require(file.open(QIODevice::ReadOnly), "cannot open comparison input");
        return file.readAll();
    }

    void write(const QString& path, const QByteArray& bytes)
    {
        require(QDir().mkpath(QFileInfo(path).path()), "cannot prepare temporary fixture");
        QFile file(path);
        require(file.open(QIODevice::WriteOnly), "cannot write temporary fixture");
        require(file.write(bytes) == bytes.size(), "short fixture write");
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    try {
        QTemporaryDir temporary;
        require(temporary.isValid(), "cannot create temporary test folder");
        const QString path = QStringLiteral("Meshes/Actors/Example/Action.HKX");
        constexpr char sourceBytes[] = "loose animation bytes\0with binary payload";
        const QByteArray payload(sourceBytes, sizeof(sourceBytes) - 1);
        write(temporary.filePath(path), payload);
        ResourceArchive resources;
        QString error;
        require(resources.open(temporary.path(), error), "cannot open loose fixture");
        require(resources.contains(path.toLower()), "loose HKX missing from index");
        require(resources.filesWithSuffix(QStringLiteral("hkx")).size() == 1,
                "loose HKX listing is incomplete or duplicated");
        require(resources.read(QStringLiteral("MESHES\\ACTORS\\EXAMPLE\\ACTION.HKX"), error) == payload,
                "case or slash normalization changed loose bytes");
        require(!resources.contains(QStringLiteral("../outside.hkx")), "outside path resolved");
        require(resources.read(QStringLiteral("missing.hkx"), error).isEmpty() && !error.isEmpty(),
                "missing input did not return a diagnostic");
        require(resources.read(path, error) == payload && error.isEmpty(), "successful read retains stale error");
        const QJsonObject evidence{{"version",1},{"resources",QJsonArray{QJsonObject{{"path",path.toLower()},{"kind","animation"},
            {"sha256",QString::fromLatin1(QCryptographicHash::hash(payload,QCryptographicHash::Sha256).toHex())}}}}};
        const SourceReader sourceRead=[&](const QString&,const QString& key){SourceBytes r;r.bytes=resources.read(key,r.error);r.provider=resources.provider(key);return r;};
        require(inspectSourceEvidence(evidence,sourceRead)["compatible"].toBool(),"unchanged source rejected");
        write(temporary.filePath(path),"replacement animation without any plugin change");
        const auto changed=inspectSourceEvidence(evidence,sourceRead);
        require(!changed["compatible"].toBool()&&changed["changed"].toInt()==1&&changed["missing"].toInt()==0,"same-path loose replacement was not detected");
        write(temporary.filePath(path),payload);
        require(inspectSourceEvidence(evidence,[&](const QString& kind,const QString& key){auto r=sourceRead(kind,key);r.provider="different mod label, same content";return r;})["compatible"].toBool(),"provider label invalidated identical content");
        // Deleting a winning loose input must report its failure, not silently
        // substitute another provider.
        require(QFile::remove(temporary.filePath(path)), "cannot remove owned fixture");
        require(resources.read(path, error).isEmpty() && error.contains(QStringLiteral("loose")),
                "missing indexed loose input was silently replaced");
        require(inspectSourceEvidence(evidence,sourceRead)["missing"].toInt()==1,"missing source was not reported");
        resources.close();
        require(!resources.isOpen() && resources.files().isEmpty() && !resources.contains(path),
                "close retained the old index");
        const auto base=temporary.filePath("base"),patch=temporary.filePath("patch");
        write(QDir(base).filePath(path),"base");write(QDir(patch).filePath(path),"patch");
        write(QDir(base).filePath("textures/base.dds"),"base-only");
        require(resources.openPackages({base,patch},error),"Cannot index ordered source packages");
        require(resources.read(path,error)=="patch"&&resources.provider(path)==patch,"Later package did not win");
        require(resources.read("textures/base.dds",error)=="base-only","Master package resource lost");
        require(QFile::remove(QDir(patch).filePath(path)),"Cannot remove winning test resource");
        require(resources.read(path,error).isEmpty()&&!error.isEmpty(),"Broken winning package silently fell back");
        resources.close();require(!resources.isOpen()&&resources.files().isEmpty(),"Layered index survived close");
        out << "PASS loose HKX indexing, bytes, normalized paths, missing-input failure and index lifetime\n";

        const QStringList args = app.arguments();
        if (args.size() == 4 && args.at(1) == QStringLiteral("--zip")) {
            app.setProperty("studioRoot", temporary.path());
            const auto package=PackageSource::prepare(args.at(2));
            require(package.ok(),qPrintable(package.error));
            require(package.rootPath.startsWith(temporary.path()+QLatin1Char('/'),Qt::CaseInsensitive),"ZIP cache escaped the task-owned test directory");
            require(resources.open(package.rootPath,error),"Cannot index the prepared ZIP package");
            require(!resources.filesWithSuffix("hkx").isEmpty(),"Prepared weapon fixture has no native animation resources");
            resources.close();
            const auto cached=PackageSource::prepare(args.at(2));
            require(cached.ok()&&cached.rootPath==package.rootPath,"Validated ZIP cache could not be reopened");
            const auto invalid=PackageSource::prepare(args.at(3));
            require(!invalid.ok()&&invalid.error.contains("Unsafe ZIP entry"),"ZIP traversal was not rejected before extraction");
            out << "PASS Windows canonical ZIP containment, real package indexing, cache reuse and traversal rejection\n";
        } else if (args.size() >= 3 && args.at(1) == QStringLiteral("--loose")) {
            for (int argument = 2; argument < args.size(); ++argument) {
                const QDir root(args.at(argument));
                require(resources.open(root.absolutePath(), error), "cannot index loose corpus");
                QDirIterator files(root.absolutePath(), { QStringLiteral("*.hkx") },
                                   QDir::Files, QDirIterator::Subdirectories);
                int count = 0;
                while (files.hasNext()) {
                    const QString physical = files.next();
                    const QString relative = root.relativeFilePath(physical);
                    require(resources.contains(relative), "corpus HKX missing from resource index");
                    require(resources.read(relative, error) == contents(physical) && error.isEmpty(),
                            "corpus loose resource byte mismatch");
                    ++count;
                }
                require(count > 0, "corpus has no loose HKX files");
                require(resources.filesWithSuffix(QStringLiteral("hkx")).size() == count,
                        "unexpected loose corpus index count");
                out << "PASS " << count << " loose HKX files indexed and read exactly: " << root.path() << '\n';
            }
        } else if (args.size() == 4) {
            // A real archive entry and its bsa_mcp-extracted reference are
            // supplied externally; the test never constructs a BA2 archive.
            const QString archivePath = args.at(1);
            const QString entry = args.at(2);
            const QByteArray expected = contents(args.at(3));
            require(!expected.isEmpty(), "empty archive reference");
            require(resources.open(archivePath, error), "cannot open real archive");
            require(resources.read(entry, error) == expected, "archive bytes differ from reference");
            require(QFile::copy(archivePath, temporary.filePath(QStringLiteral("fixture.ba2"))),
                    "cannot copy archive to owned temporary test folder");
            const QByteArray overrideBytes("selected loose override");
            write(temporary.filePath(entry), overrideBytes);
            require(resources.open(temporary.path(), error), "cannot open combined sources");
            require(resources.read(entry, error) == overrideBytes, "loose HKX did not override archive");
            const QString normalized = ResourceArchive::normalizeResourcePath(entry);
            require(resources.files().count(normalized) == 1, "overridden entry listed twice");
            out << "PASS real BA2 bytes against extracted reference and loose-over-archive precedence\n";
        } else {
            require(args.size() == 1, "usage: ResourcesTests [archive entry extracted-reference]");
        }
    } catch (const std::exception& error) {
        out << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
