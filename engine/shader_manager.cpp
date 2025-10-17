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
