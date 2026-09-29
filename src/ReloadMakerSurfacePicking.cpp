#include "glview.h"
#include "gl/glshape.h"
#include "gl/renderer.h"
#include "gl/gltools.h"
#include "GripViewportMath.h"
#include <QOpenGLFramebufferObject>
#include <QScopeGuard>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <string>

// These declarations are injected into the staged headers only. The renderer
// owns vertex skinning/LOD and shaders; there is no second NIF decoder or bind pose.
void Shape::reloadMakerDrawSurface()
{
    if(isHidden() || !scene || !scene->renderer || !bindShape()) return;
    auto* context=scene->renderer;
    auto* program=context->useProgram("selection.prog"); if(!program) return;
    setUniforms(program); program->uni1i("selectionFlags",int(Scene::SelObject));
    program->uni1i("selectionParam",nodeId);
    if(isDoubleSided) glDisable(GL_CULL_FACE); else glEnable(GL_CULL_FACE);
    // Unlike wireframe/selection highlighting, contact depth must not be biased.
    glDisable(GL_POLYGON_OFFSET_FILL);
    const auto count=std::clamp<qsizetype>(lodTriangleCount,0,triangles.size());
    if(count>0) context->fn->glDrawElements(GL_TRIANGLES,GLsizei(count)*3,GL_UNSIGNED_SHORT,nullptr);
}

QVector<int> Shape::reloadMakerSkinBones() const
{
    if(transformRigid || boneWeights0.empty()) return {};
    QSet<int> used;
    const auto scan=[&](const auto& weights){for(const auto& row:weights)for(int j=0;j<4;++j){
        const float encoded=row[j]; if(!(encoded>0)||!std::isfinite(encoded)||encoded>=float(boneData.size()))break;
        const int bone=int(encoded); if(bone>=0 && bone<boneData.size() && encoded-float(bone)>0)used.insert(boneData[bone].bone);
    }};
    scan(boneWeights0);scan(boneWeights1);
    auto result=used.values();std::sort(result.begin(),result.end());return result;
}

QMatrix4x4 GLView::reloadMakerWorldToClip() const
{
    QMatrix4x4 projection,viewMatrix;
    if(!scene || !glContext || !glContext->globalUniforms) { projection.fill(0); return projection; }
    for(int r=0;r<4;++r) for(int c=0;c<4;++c) projection(r,c)=glContext->globalUniforms->projectionMatrix[c][r];
    const auto origin=scene->view*Vector3(0,0,0);
    const Vector3 basis[]{scene->view*Vector3(1,0,0)-origin,scene->view*Vector3(0,1,0)-origin,scene->view*Vector3(0,0,1)-origin};
    for(int r=0;r<3;++r) { for(int c=0;c<3;++c) viewMatrix(r,c)=basis[c][r]; viewMatrix(r,3)=origin[r]; }
    return projection*viewMatrix;
}

bool GLView::reloadMakerSurfaceAt(const QPointF& point,const QVector<int>& allowedBlocks,int& block,Vector3& world)
{
    if(!model || !scene || !scene->renderer || !isValid() || !isVisible() || doCompile ||
       point.x()<0 || point.y()<0 || point.x()>=width() || point.y()>=height() || allowedBlocks.isEmpty()) return false;
    auto* previous=pushGLContext(); const auto restoreContext=qScopeGuard([&](){popGLContext(previous);});
    auto* renderer=scene->renderer;
    const std::string previousProgram=renderer->getCurrentProgram()?std::string(renderer->getCurrentProgram()->name):std::string();
    const auto uniforms=*renderer->globalUniforms;
    GLint viewport[4],drawFbo=0,readFbo=0,depthFunc=0,polygon[2],cullMode=0,vao=0;
    GLboolean depthMask,colorMask[4]; GLfloat color[4]; GLdouble depthClear,depthRange[2];
    glGetIntegerv(GL_VIEWPORT,viewport); glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&drawFbo); glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&readFbo);
    glGetIntegerv(GL_DEPTH_FUNC,&depthFunc); glGetIntegerv(GL_POLYGON_MODE,polygon); glGetIntegerv(GL_CULL_FACE_MODE,&cullMode);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&vao); glGetBooleanv(GL_DEPTH_WRITEMASK,&depthMask); glGetBooleanv(GL_COLOR_WRITEMASK,colorMask);
    glGetFloatv(GL_COLOR_CLEAR_VALUE,color); glGetDoublev(GL_DEPTH_CLEAR_VALUE,&depthClear); glGetDoublev(GL_DEPTH_RANGE,depthRange);
    const GLenum flags[]{GL_DEPTH_TEST,GL_BLEND,GL_DITHER,GL_MULTISAMPLE,GL_FRAMEBUFFER_SRGB,GL_CULL_FACE,GL_POLYGON_OFFSET_FILL,GL_SCISSOR_TEST,GL_STENCIL_TEST};
    GLboolean enabled[std::size(flags)]; for(size_t i=0;i<std::size(flags);++i) enabled[i]=glIsEnabled(flags[i]);
    const auto selection=scene->selecting;
    GLint packBuffer=0,pack[4];
    const GLenum packNames[]{GL_PACK_ALIGNMENT,GL_PACK_ROW_LENGTH,GL_PACK_SKIP_ROWS,GL_PACK_SKIP_PIXELS};
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING,&packBuffer);
    for(size_t i=0;i<std::size(packNames);++i) glGetIntegerv(packNames[i],&pack[i]);
    const auto restoreState=qScopeGuard([&](){
        renderer->stopProgram(); scene->selecting=selection;
        *renderer->globalUniforms=uniforms; renderer->setGlobalUniforms();
        renderer->fn->glBindFramebuffer(GL_DRAW_FRAMEBUFFER,GLuint(drawFbo)); renderer->fn->glBindFramebuffer(GL_READ_FRAMEBUFFER,GLuint(readFbo));
        renderer->fn->glBindVertexArray(GLuint(vao));
        renderer->fn->glBindBuffer(GL_PIXEL_PACK_BUFFER,GLuint(packBuffer));
        for(size_t i=0;i<std::size(packNames);++i) glPixelStorei(packNames[i],pack[i]);
        glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);
        glDepthFunc(GLenum(depthFunc)); glDepthMask(depthMask); glColorMask(colorMask[0],colorMask[1],colorMask[2],colorMask[3]);
        glClearColor(color[0],color[1],color[2],color[3]); glClearDepth(depthClear); glDepthRange(depthRange[0],depthRange[1]);
        glCullFace(GLenum(cullMode)); glPolygonMode(GL_FRONT_AND_BACK,GLenum(polygon[0]));
        for(size_t i=0;i<std::size(flags);++i) { if(enabled[i]) glEnable(flags[i]); else glDisable(flags[i]); }
        if(!previousProgram.empty()) renderer->useProgram(previousProgram);
        update();
    });
    QOpenGLFramebufferObjectFormat format; format.setTextureTarget(GL_TEXTURE_2D); format.setInternalTextureFormat(GL_RGBA8);
    format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
    QOpenGLFramebufferObject target(pixelWidth,pixelHeight,format);
    if(!target.isValid() || !target.bind()) return false;
    for(auto flag:flags) glDisable(flag);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glDepthMask(GL_TRUE); glDepthRange(0,1);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE); glClearColor(0,0,0,0); glClearDepth(1);
    glPolygonMode(GL_FRONT_AND_BACK,GL_FILL); glCullFace(GL_BACK);
    renderer->setViewport(0,0,pixelWidth,pixelHeight); glProjection();
    const auto camera=reloadMakerWorldToClip();
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    scene->selecting=Scene::SelObject;
    renderer->setGlobalUniforms(); renderer->setDefaultVertexAttribs(Scene::defaultAttrMask,Scene::defaultVertexAttrs);
    const QSet<int> allowed(allowedBlocks.cbegin(),allowedBlocks.cend());
    for(auto* shape:scene->shapes) if(shape && allowed.contains(shape->id())) shape->reloadMakerDrawSurface();
    renderer->stopProgram();
    renderer->fn->glBindBuffer(GL_PIXEL_PACK_BUFFER,0);
    glPixelStorei(GL_PACK_ALIGNMENT,1); glPixelStorei(GL_PACK_ROW_LENGTH,0); glPixelStorei(GL_PACK_SKIP_ROWS,0); glPixelStorei(GL_PACK_SKIP_PIXELS,0);
    const int x=std::clamp(int(std::floor(point.x()*devicePixelRatioF())),0,pixelWidth-1);
    const int y=std::clamp(pixelHeight-1-int(std::floor(point.y()*devicePixelRatioF())),0,pixelHeight-1);
    unsigned char rgba[4]{}; GLfloat depth=1;
    glReadPixels(x,y,1,1,GL_RGBA,GL_UNSIGNED_BYTE,rgba); glReadPixels(x,y,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&depth);
    const auto packed=std::uint32_t(rgba[0])|(std::uint32_t(rgba[1])<<8)|(std::uint32_t(rgba[2])<<16)|(std::uint32_t(rgba[3])<<24);
    const int selected=getIDFromColorKey(packed);
    QVector3D position;
    if(!allowed.contains(selected) || depth>=1 || !reloadmaker::unprojectGripPoint(camera,QSizeF(pixelWidth,pixelHeight),QPointF(x+.5,pixelHeight-y-.5),depth,position)) return false;
    block=selected; world=Vector3(position.x(),position.y(),position.z()); return true;
}
