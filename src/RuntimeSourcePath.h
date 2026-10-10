#pragma once
#include <QString>
#include <QStringList>
namespace reloadmaker {
// Keep the animation's weapon ancestors and the model's complete part ancestry.
inline QString runtimeSourcePath(const QString& skeletonPath,const QString& modelPath) {
    const auto runtime=skeletonPath.split('/'),source=modelPath.split('/');
    if(runtime.size()<2||source.size()<2||runtime.back()!=source.back())return skeletonPath;
    for(int a=1;a<source.size();++a)for(int b=1;b<runtime.size();++b) {
        if(source[a]!=runtime[b])continue;
        int next=b;
        for(int i=a;i<source.size()&&next<runtime.size();++i)if(source[i]==runtime[next])++next;
        if(next==runtime.size())return (runtime.mid(0,b)+source.mid(a)).join('/');
    }
    return skeletonPath;
}
}
