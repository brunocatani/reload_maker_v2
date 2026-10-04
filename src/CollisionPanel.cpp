#include "CollisionPanel.h"
#include "design/DesignWidgets.h"
#include "areas/collisions/CollisionWorkshop.h"
#include <QComboBox>
#include <QListWidget>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QFileInfo>
#include <QSignalBlocker>
#include <stdexcept>

namespace reloadmaker {
// Every connection into the workshop uses it as context, so none outlives it.
CollisionPanel::CollisionPanel(CollisionWorkshop& workshop,QWidget* parent):QWidget(parent),_workshop(workshop){
    // Sections in working order: what is captured, generate, inspect the
    // hulls and pieces, then save, export and cook. Tuning stays folded.
    auto* layout=new QVBoxLayout(this);layout->setContentsMargins(16,10,16,16);layout->setSpacing(8);
    auto label=[&](const QString& text){auto* w=new QLabel(text);w->setWordWrap(true);layout->addWidget(w);return w;};
    auto hint=[&](const QString& text){auto* w=design::hintLabel(text,this);layout->addWidget(w);return w;};
    auto section=[&](const QString& title){layout->addSpacing(8);layout->addWidget(design::sectionLabel(title,this));};
    _title=label("COLLISION WORKSHOP");_title->setObjectName("Section");
    _intro=label({});_intro->setObjectName("Hint");
    _target=new QComboBox;_target->setPlaceholderText("Nothing captured yet");layout->addWidget(_target);
    auto button=[&](const QString& title,const QString& operation,QBoxLayout* into=nullptr){auto* b=new QPushButton(title);(into?into:layout)->addWidget(b);connect(b,&QPushButton::clicked,&_workshop,[this,operation]{run(operation);});return b;};
    button("Generate with ROCK defaults","defaults")->setObjectName("PrimaryButton");

    section("Hulls");
    _overlay=new QCheckBox("Show hulls · x-ray");_overlay->setChecked(_workshop.overlay());layout->addWidget(_overlay);
    _compare=new QCheckBox("Compare previous result · blue wireframe");_compare->setChecked(_workshop.compare());layout->addWidget(_compare);
    _isolate=new QCheckBox("Show selected hull only");_isolate->setChecked(_workshop.isolated());layout->addWidget(_isolate);
    _hulls=new QListWidget;_hulls->setMinimumHeight(140);layout->addWidget(_hulls);
    hint("Yellow: selected · teal: ROCK. Uncheck a hull to exclude it from export. Save before changing the source, target or animation.");
    auto* settings=new QWidget(this);auto* settingsLayout=new QVBoxLayout(settings);settingsLayout->setContentsMargins(0,0,0,0);settingsLayout->setSpacing(8);
    auto* form=new QFormLayout;form->setSpacing(8);const auto& values=_workshop.form();
    auto spin=[&](const QString& name,const CollisionFormField& field,double value){auto* s=new QDoubleSpinBox;s->setRange(field.minimum,field.maximum);s->setDecimals(field.decimals);s->setValue(value);form->addRow(name,s);return s;};
    _gap=spin("Gap to preserve · game units",kCollisionGap,values.gap);_error=spin("Maximum support error",kCollisionSupportError,values.supportError);_points=spin("Target points per hull",kCollisionTargetPoints,values.targetPoints);
    _mass=spin("Mass · kg",kCollisionMass,values.mass);_radius=spin("Convex radius · Havok",kCollisionRadius,values.radius);settingsLayout->addLayout(form);
    button("Regenerate with these settings","generate",settingsLayout);
    layout->addWidget(design::disclosure("Generation settings",settings,this));layout->addWidget(settings);

    section("World model pieces");
    hint("Unchecked pieces are excluded from the prepared model and collision. Use separate captures for full and empty presentations. Regenerate hulls after re-including a piece.");
    _pieces=new QListWidget;_pieces->setMinimumHeight(140);layout->addWidget(_pieces);

    section("Save and export");
    button("Save or replace this profile","save");
    button("Open saved profile…","browse_profile");button("Export collision JSON…","browse_export");
    button("Cancel generation","cancel");

    section("Finish the model");
    button("Capture visible model…","prepare");button("Cook prepared magazine…","cook");
    hint("Preview shows convex point envelopes. Cook prepared magazine uses your configured Creation Kit to produce a finished collision model. Live Havok contacts still require an in-game test.");
    _status=label("No collision snapshot yet.");_status->setObjectName("Callout");_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addStretch();
    connect(_target,qOverload<int>(&QComboBox::activated),&_workshop,[this](int index){_workshop.chooseTarget(index);});
    connect(_hulls,&QListWidget::currentRowChanged,&_workshop,[this](int row){_workshop.selectHull(row);});
    connect(_hulls,&QListWidget::itemChanged,&_workshop,[this](QListWidgetItem* item){_workshop.setHullEnabled(_hulls->row(item),item->checkState()==Qt::Checked);});
    connect(_pieces,&QListWidget::itemChanged,&_workshop,[this](QListWidgetItem* item){_workshop.setPieceIncluded(_pieces->row(item),item->checkState()==Qt::Checked);});
    connect(_overlay,&QCheckBox::toggled,&_workshop,&CollisionWorkshop::setOverlay);
    connect(_compare,&QCheckBox::toggled,&_workshop,&CollisionWorkshop::setCompare);
    connect(_isolate,&QCheckBox::toggled,&_workshop,&CollisionWorkshop::setIsolated);
    connect(_gap,qOverload<double>(&QDoubleSpinBox::valueChanged),&_workshop,&CollisionWorkshop::setGap);
    connect(_error,qOverload<double>(&QDoubleSpinBox::valueChanged),&_workshop,&CollisionWorkshop::setSupportError);
    connect(_points,qOverload<double>(&QDoubleSpinBox::valueChanged),&_workshop,&CollisionWorkshop::setTargetPoints);
    connect(_mass,qOverload<double>(&QDoubleSpinBox::valueChanged),&_workshop,&CollisionWorkshop::setMass);
    connect(_radius,qOverload<double>(&QDoubleSpinBox::valueChanged),&_workshop,&CollisionWorkshop::setRadius);
    connect(&_workshop,&CollisionWorkshop::updated,this,[this]{showWorkshop();});
}
void CollisionPanel::setSubject(const QString& title,const QString& intro){
    _title->setText(title.toUpper());_intro->setText(intro+" Orbit the model to inspect gaps and overlaps.");
}
void CollisionPanel::run(const QString& operation){
    try{_workshop.command({{"operation",operation}});}catch(const std::exception& e){_workshop.reportError(QString::fromUtf8(e.what()));}
}
void CollisionPanel::showWorkshop(){
    const auto& profile=_workshop.profile();
    {const QSignalBlocker block(_target);QStringList shown;for(int i=0;i<_target->count();++i)shown<<_target->itemText(i);if(shown!=_workshop.targets()){_target->clear();_target->addItems(_workshop.targets());}_target->setCurrentIndex(_workshop.targetIndex());}
    {const auto& values=_workshop.form();const QSignalBlocker a(_gap),b(_error),c(_points),d(_mass),e(_radius);_gap->setValue(values.gap);_error->setValue(values.supportError);_points->setValue(values.targetPoints);_mass->setValue(values.mass);_radius->setValue(values.radius);}
    {const QSignalBlocker a(_overlay),b(_compare),c(_isolate);_overlay->setChecked(_workshop.overlay());_compare->setChecked(_workshop.compare());_isolate->setChecked(_workshop.isolated());}
    {const QSignalBlocker block(_hulls);_hulls->clear();for(const auto& h:profile.hulls){auto* item=new QListWidgetItem(QString("%1 · %2 points").arg(h.name).arg(h.points.size()),_hulls);item->setFlags(item->flags()|Qt::ItemIsUserCheckable);item->setCheckState(h.enabled?Qt::Checked:Qt::Unchecked);}
        _hulls->setCurrentRow(_workshop.selectedHull());}
    {const QSignalBlocker block(_pieces);_pieces->clear();for(const auto& source:profile.sources){auto* item=new QListWidgetItem(source.name+"\n"+QFileInfo(source.material).fileName(),_pieces);item->setToolTip(source.path+"\n"+source.material);item->setFlags(item->flags()|Qt::ItemIsUserCheckable);item->setCheckState(source.worldIncluded?Qt::Checked:Qt::Unchecked);}}
    QString text=_workshop.generating()?"Generating on a cancellable worker…":profile.valid()?QString("%1 hulls · %2 source triangles\n%3\n%4").arg(profile.hulls.size()).arg(profile.inputTriangles).arg(_workshop.dirty()?"Unsaved changes":"Saved profile").arg(_workshop.lastPath()):"No collision snapshot yet.";
    if(!profile.warnings.isEmpty())text+="\n"+profile.warnings.join('\n');
    if(!_workshop.lastError().isEmpty())text+="\n"+_workshop.lastError();
    _status->setText(text);
}
}
