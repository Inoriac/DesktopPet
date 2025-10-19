//
// Created by Inoriac on 2025/10/15.
//

#include "shader_manager.h"
#include <QDebug>
#include <QFile>
#include <QTextStream>

ShaderManager::ShaderManager() : gl(nullptr) {
}

ShaderManager::~ShaderManager() {
    cleanup();
}

void ShaderManager::initialize(QOpenGLFunctions_3_3_Core *glFuncs) {
    gl = glFuncs;
    qDebug() << "ShaderManager initialized";
}

bool ShaderManager::loadShader(const std::string &name, const std::string &vertPath, const std::string& fragPath) {
    std::string vertSource = readFile(vertPath);
    std::string fragSource = readFile(fragPath);

    if (vertSource.empty() || fragSource.empty()) {
        qWarning() << "ShaderManager::loadShader: Failed to load shader from file " << vertPath.c_str() << fragPath.c_str();
        return false;
    }

    return loadShaderFromSource(name, vertSource, fragSource);
}

bool ShaderManager::loadShaderFromSource(const std::string &name, const std::string &vertSource,
    const std::string &fragSource) {
    auto program = std::make_unique<QOpenGLShaderProgram>();

    // 添加顶点着色器
    if (!program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertSource.c_str())) {     // c_str 获取c风格的char指针 即 const char*
        qWarning() << "Vertex shader compilation failed for" << name.c_str() << ":" << program->log();
    }

    // 添加片段着色器
    if (!program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragSource.c_str())) {
        qWarning() << "Fragment shader compilation failed for" << name.c_str() << ":" << program->log();
        return false;
    }

    // 将已编译的顶点着色器与片段着色器组合成可用的 shader program
    if (!program->link()) {
        qWarning() << "Shader program linking failed for" << name.c_str() << ":" << program->log();
        return false;
    }

    shaders[name] = std::move(program);
    qDebug() << "Shader loaded successfully:" << name.c_str();
    return true;
}

QOpenGLShaderProgram * ShaderManager::getShader(const std::string &name) {
    auto it = shaders.find(name);
    return (it != shaders.end() ? it->second.get() : nullptr);
}

bool ShaderManager::hasShader(const std::string &name) const {
    return shaders.find(name) != shaders.end();
}

std::string ShaderManager::readFile(const std::string &filepath) {
    QFile file(QString::fromStdString(filepath));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "Failed to open file " << filepath.c_str();
        return "";
    }
    QTextStream in(&file);
    return in.readAll().toStdString();
}

void ShaderManager::cleanup() {
    shaders.clear();
}

void ShaderManager::applyPBRUniforms(QOpenGLShaderProgram *shader,
    const QMatrix4x4 &model, const QMatrix4x4 &view,
    const QMatrix4x4 &projection,
    const QVector3D &baseColorFactor,
    float alphaFactor,
    float metallicFactor,
    float roughnessFactor,
    const QVector3D &viewPos,
    const QVector3D *lightPositions,
    const QVector3D *lightColors,
    int lightCount) {
    if (!shader) return;

    // ---- 矩阵 ----
    shader->setUniformValue("model", model);
    shader->setUniformValue("view", view);
    shader->setUniformValue("projection", projection);

    // ---- 材质属性 ----
    shader->setUniformValue("baseColorFactor", baseColorFactor);
    shader->setUniformValue("alphaFactor", alphaFactor);
    shader->setUniformValue("metallicFactor", metallicFactor);
    shader->setUniformValue("roughnessFactor", roughnessFactor);

    // ---- 视角 ----
    shader->setUniformValue("viewPos", viewPos);

    // ---- 光源 ----
    for (int i = 0; i < lightCount; ++i) {
        QString posName = QString("lightPositions[%1]").arg(i);
        QString colName = QString("lightColors[%1]").arg(i);
        shader->setUniformValue(posName.toUtf8().constData(), lightPositions[i]);
        shader->setUniformValue(colName.toUtf8().constData(), lightColors[i]);
    }
}

void ShaderManager::bindPBRTextures(const MaterialData &mat, QOpenGLShaderProgram *shader, GLuint defaultTexID) {
    if (!shader) return;

    int unit = 0;

    // 绑定纹理，如果纹理ID为0则使用默认纹理
    auto bindTexture = [&](GLuint texID, const char* uniformName) {
        gl->glActiveTexture(GL_TEXTURE0 + unit);
        gl->glBindTexture(GL_TEXTURE_2D, texID != 0 ? texID : defaultTexID);
        shader->setUniformValue(uniformName, unit++);
    };

    bindTexture(mat.albedoTexID, "albedoMap");
    bindTexture(mat.normalTexID, "normalMap");
    bindTexture(mat.metallicRoughnessTexID, "metallicRoughness");
    bindTexture(mat.aoTexID, "aoTex");
    bindTexture(mat.emissiveTexID, "emissiveMap");
}
