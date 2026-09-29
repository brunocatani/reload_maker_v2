#include "ReloadGraph.h"
#include <QMouseEvent>
#include <QPainter>
#include <QSet>
#include <algorithm>

namespace reloadmaker {
    ReloadGraph::ReloadGraph(QWidget* parent):QWidget(parent){setMinimumHeight(130);setMinimumWidth(420);}
    void ReloadGraph::setRecipe(const ReloadRecipe* recipe){
        _recipe=recipe;_hits.clear();QSet<int> tracks;
        if(recipe)for(const auto& window:recipe->windows)for(const auto& member:window.members)tracks.insert(member.track);
        setMinimumHeight(std::max(130,70+int(tracks.size())*25));update();
    }
    void ReloadGraph::setSelection(int window,float seconds){_selected=window;_seconds=seconds;update();}
    void ReloadGraph::paintEvent(QPaintEvent*){
        QPainter p(this);p.fillRect(rect(),QColor("#111719"));p.setRenderHint(QPainter::Antialiasing);_hits.clear();
        if(!_recipe||_recipe->windows.isEmpty()){p.setPen(QColor("#a6b0b5"));p.drawText(rect().adjusted(18,12,-18,-12),Qt::AlignCenter,"Reload movement windows will appear here after analysis.");return;}
        const auto& r=*_recipe;const float span=std::max(1,width()-labelWidth-18);
        const auto x=[&](float seconds){return labelWidth+span*seconds/r.duration;};
        p.setPen(QColor("#a7b6bc"));p.drawText(QRect(12,3,labelWidth-20,22),Qt::AlignVCenter,"PARTS / RELOAD TIME");
        for(int i=0;i<=10;++i){const float time=r.duration*i/10.f;const float pixel=x(time);p.setPen(QColor("#28343a"));p.drawLine(QPointF(pixel,26),QPointF(pixel,height()));p.setPen(QColor("#91a3ad"));p.drawText(QPointF(pixel+3,19),QString::number(time,'f',1)+"s");}
        for(int i=0;i<r.windows.size();++i){const auto& w=r.windows[i];const QRectF cell(x(r.duration*w.first/(r.frames-1)),30,std::max(2.f,span*(w.last-w.first)/(r.frames-1)),23);
            p.setPen(QColor(i==_selected?"#f4cf92":"#50636d"));p.setBrush(QColor(i==_selected?"#72502c":"#23353d"));p.drawRoundedRect(cell,3,3);if(cell.width()>20)p.drawText(cell,Qt::AlignCenter,QString::number(i+1));_hits.append({cell,i,-1});}
        int row=0;
        for(int track=0;track<r.tracks.size();++track){
            bool present=false;for(const auto& w:r.windows)for(const auto& m:w.members)if(m.track==track){present=true;break;}if(!present)continue;
            const int y=64+25*row++;if(row%2)p.fillRect(QRect(0,y-3,width(),25),QColor("#182023"));
            p.setPen(QColor("#d5dfdf"));p.drawText(QRect(12,y,labelWidth-24,19),Qt::AlignVCenter,p.fontMetrics().elidedText(r.tracks[track].label,Qt::ElideRight,labelWidth-24));
            for(int i=0;i<r.windows.size();++i){const auto& w=r.windows[i];for(const auto& m:w.members)if(m.track==track){
                const QRectF bar(x(r.duration*w.first/(r.frames-1)),y,std::max(2.f,span*(w.last-w.first)/(r.frames-1)),17);
                const QColor color(m.grabbable?"#d3a25d":m.relation==ReloadRelation::Rigid?"#719eb8":"#548981");
                p.setPen(i==_selected?QPen(color.lighter(140),2):QPen(color));p.setBrush(color.darker(i==_selected?140:210));p.drawRoundedRect(bar,2,2);_hits.append({bar,i,track});
            }}
        }
        p.setPen(QPen(QColor("#f7e1ba"),1.5));p.drawLine(QPointF(x(_seconds),25),QPointF(x(_seconds),height()));
    }
    void ReloadGraph::mousePressEvent(QMouseEvent* event){
        if(event->button()!=Qt::LeftButton||!_recipe)return;
        if(event->position().y()<28&&sought){sought(std::clamp(float(event->position().x()-labelWidth)/std::max(1,width()-labelWidth-18),0.f,1.f)*_recipe->duration);return;}
        for(const auto& hit:_hits)if(hit.rect.contains(event->position())){if(selected)selected(hit.window,hit.track);return;}
    }
}
