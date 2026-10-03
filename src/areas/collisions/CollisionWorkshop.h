#pragma once

#include "CollisionAuthoring.h"

#include <QFutureWatcher>
#include <QObject>
#include <functional>

namespace reloadmaker
{
    // A generation form value's range and precision. Values are rounded and
    // bounded as the panel's spin boxes show them, so command and panel edits
    // generate the same profile.
    struct CollisionFormField
    {
        double minimum;
        double maximum;
        int decimals;
    };
    inline constexpr CollisionFormField kCollisionGap{ .01, 100, 2 };
    inline constexpr CollisionFormField kCollisionSupportError{ 0, 10, 3 };
    inline constexpr CollisionFormField kCollisionTargetPoints{ 4, 252, 0 };
    inline constexpr CollisionFormField kCollisionMass{ .001, 100, 3 };
    inline constexpr CollisionFormField kCollisionRadius{ 0, .1, 5 };

    // The generation form: the gap to preserve, the maximum support error and
    // the target points per hull, plus the mass and convex radius, which a
    // generated or opened profile shows and edits.
    struct CollisionForm
    {
        double gap{ 1 };
        double supportError{ .5 };
        double targetPoints{ 96 };
        double mass{ 2 };
        double radius{ .01 };
    };

    // The collision workshop: the targets the scene offers, the generated or
    // opened profile and the one it replaced (drawn for comparison), the
    // generation form, the overlay choices, and the cancellable generation.
    class CollisionWorkshop final : public QObject
    {
        Q_OBJECT

    public:
        explicit CollisionWorkshop(QObject* parent = nullptr);
        ~CollisionWorkshop() override;

        // Provided by the controller: the scene snapshot of a target (index,
        // reference only), the cook and prepared-part dialogs, and the file
        // choices of the browse operations (empty if cancelled).
        std::function<CollisionProfile(int, bool)> snapshot;
        std::function<void()> cook;
        std::function<void()> prepare;
        std::function<QString()> chooseProfile;
        std::function<QString()> chooseExport;

        void setTargets(const QStringList& names);
        void chooseTarget(int index);
        void invalidate();
        void cancel();
        void reportError(const QString& error);
        void selectHull(int index);
        void setHullEnabled(int index, bool enabled);
        void setPieceIncluded(int index, bool included);
        void setGap(double value);
        void setSupportError(double value);
        void setTargetPoints(double value);
        void setMass(double value);
        void setRadius(double value);
        void setOverlay(bool shown);
        void setCompare(bool shown);
        void setIsolated(bool isolated);
        QJsonObject command(const QJsonObject& args);
        [[nodiscard]] QJsonObject state() const;

        [[nodiscard]] const QStringList& targets() const noexcept { return _targets; }
        [[nodiscard]] int targetIndex() const noexcept { return _targetIndex; }
        [[nodiscard]] const CollisionProfile& profile() const noexcept { return _profile; }
        [[nodiscard]] const CollisionProfile& baseline() const noexcept { return _baseline; }
        [[nodiscard]] const CollisionForm& form() const noexcept { return _form; }
        [[nodiscard]] bool overlay() const noexcept { return _overlay; }
        [[nodiscard]] bool compare() const noexcept { return _compare; }
        [[nodiscard]] bool isolated() const noexcept { return _isolated; }
        [[nodiscard]] int selectedHull() const noexcept { return _selectedHull; }
        [[nodiscard]] bool dirty() const noexcept { return _dirty; }
        [[nodiscard]] bool generating() const noexcept { return _generating; }
        [[nodiscard]] const QString& lastError() const noexcept { return _lastError; }
        [[nodiscard]] const QString& lastPath() const noexcept { return _lastPath; }

    signals:
        // The panel shows the workshop again.
        void updated();
        // After every update and view change; authored only for geometry and
        // physics changes.
        void changed(bool authored);

    private:
        void update(bool authored = false);
        void finishGeneration();
        void applyPhysicalForm();
        void generate(const QString& operation, const QJsonObject& args);
        void openProfile(const QString& path);
        void saveProfile(const QString& operation, const QString& path);
        [[nodiscard]] CollisionProfile context(bool referenceOnly);
        [[nodiscard]] QString savePath() const;

        QStringList _targets;
        int _targetIndex{ -1 };
        CollisionProfile _profile;
        CollisionProfile _baseline;
        CollisionForm _form;
        bool _overlay{ true };
        bool _compare{ false };
        bool _isolated{ false };
        int _selectedHull{ -1 };
        QFutureWatcher<CollisionResult> _worker;
        std::shared_ptr<std::atomic<bool>> _cancel;
        QString _lastError;
        QString _lastPath;
        bool _dirty{ false };
        bool _generating{ false };
    };
}
