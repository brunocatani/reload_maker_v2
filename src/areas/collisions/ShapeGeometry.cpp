// Geometry of the native renderer's shapes for collision and world-model
// capture. These Shape members are declared by the staged native engine
// (tools/prepare_build.py); the native renderer remains the authority for
// skinning, LOD and transforms.
#include "gl/glshape.h"

#include <QJsonArray>
#include <QVector3D>
#include <algorithm>

// Skinned vertices in the pose the renderer last drew, blended from up to
// eight bone weights; rigid shapes keep their vertices.
QVector<Vector3> Shape::reloadMakerCollisionVertices() const
{
    auto result = verts;
    if (transformRigid || boneWeights0.size() != size_t(verts.size()) || boneTransforms.empty()) {
        return result;
    }
    for (qsizetype i = 0; i < result.size(); ++i) {
        Vector3 sum(0, 0, 0);
        float total = 0;
        const int count = boneWeights1.size() == size_t(verts.size()) ? 8 : 4;
        for (int j = 0; j < count; ++j) {
            const float encoded = j < 4 ? boneWeights0[i][j] : boneWeights1[i][j - 4];
            if (!(encoded > 0)) {
                break;
            }
            const int bone = int(encoded);
            if (bone < 0 || size_t(bone * 3 + 2) >= boneTransforms.size()) {
                continue;
            }
            const float w = encoded - float(bone);
            const auto* rows = boneTransforms.data() + bone * 3;
            for (int axis = 0; axis < 3; ++axis) {
                sum[axis] += (verts[i][0] * rows[axis][0] + verts[i][1] * rows[axis][1] + verts[i][2] * rows[axis][2] + rows[axis][3]) * w;
            }
            total += w;
        }
        if (total > 0) {
            result[i] = sum / total;
        }
    }
    return result;
}

// The triangles of the level of detail the renderer draws.
qsizetype Shape::reloadMakerCollisionTriangleCount() const
{
    return std::clamp<qsizetype>(lodTriangleCount, 0, triangles.size());
}

// The complete rigid render mesh at its highest detail as glTF-style
// arrays; empty when the shape is skinned, has more than 65535 vertices or
// lacks normals, tangents, UVs or consistent colors.
QJsonObject Shape::reloadMakerRigidMeshJson() const
{
    if (!transformRigid || verts.isEmpty() || verts.size() > 65535 || norms.size() != verts.size() || tangents.size() != verts.size()
        || bitangents.size() != verts.size() || coords.isEmpty() || coords[0].size() != verts.size() || lodTriangleCount != triangles.size()) {
        return {};
    }
    QJsonArray positions;
    QJsonArray normals;
    QJsonArray tangent;
    QJsonArray uvs;
    QJsonArray indices;
    QJsonArray vertexColors;
    for (qsizetype i = 0; i < verts.size(); ++i) {
        positions.append(QJsonArray{ verts[i][0], verts[i][1], verts[i][2] });
        normals.append(QJsonArray{ norms[i][0], norms[i][1], norms[i][2] });
        // Match the native glTF exporter: NIF's bitangent is glTF's tangent.
        const QVector3D n(norms[i][0], norms[i][1], norms[i][2]);
        const QVector3D t(bitangents[i][0], bitangents[i][1], bitangents[i][2]);
        const QVector3D b(tangents[i][0], tangents[i][1], tangents[i][2]);
        tangent.append(QJsonArray{ t.x(), t.y(), t.z(), QVector3D::dotProduct(QVector3D::crossProduct(n, t), b) >= 0 ? 1 : -1 });
        uvs.append(QJsonArray{ coords[0][i][0], coords[0][i][1] });
        if (hasVertexColors) {
            if (colors.size() != verts.size()) {
                return {};
            }
            vertexColors.append(QJsonArray{ colors[i][0], colors[i][1], colors[i][2], colors[i][3] });
        }
    }
    for (const auto& triangle : triangles) {
        for (int k = 0; k < 3; ++k) {
            if (triangle[k] >= verts.size()) {
                return {};
            }
            indices.append(int(triangle[k]));
        }
    }
    QJsonObject result{ { "positions", positions }, { "normals", normals }, { "tangents", tangent }, { "uvs", uvs }, { "indices", indices } };
    if (hasVertexColors) {
        result["colors"] = vertexColors;
    }
    if (coords.size() > 1) {
        if (coords[1].size() != verts.size()) {
            return {};
        }
        QJsonArray second;
        for (const auto& uv : coords[1]) {
            second.append(QJsonArray{ uv[0], uv[1] });
        }
        result["uvs2"] = second;
    }
    return result;
}
