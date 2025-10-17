//
// Created by Inoriac on 2025/10/15.
//

#ifndef DESKTOP_PET_SHADER_MANAGER_H
#define DESKTOP_PET_SHADER_MANAGER_H

#include <QOpenGLShaderProgram>
#include <QOpenGLFunctions_3_3_Core>
#include <string>
#include <unordered_map>
#include <memory>

class ShaderManager {
public:
    ShaderManager();
    ~ShaderManager();

    // 初始化着色器管理器
    void initialize(QOpenGLFunctions_3_3_Core* glFuncs);

    // 从文件加载着色器
    bool loadShader(const std::string& name, const std::string& vertPath, const std::string& fragPath);

    // 从源码字符串加载着色器
    bool loadShaderFromSource(const std::string& name, const std::string& vertSource, const std::string& fragSource);

    // 获取着色器程序
    QOpenGLShaderProgram* getShader(const std::string& name);

    // 检查着色器是否存在
    bool hasShader(const std::string& name) const;

    // 清理所有着色器
    void cleanup();

private:
    QOpenGLFunctions_3_3_Core* gl;
    std::unordered_map<std::string, std::unique_ptr<QOpenGLShaderProgram>> shaders;

    // 从文件读取字符串
    std::string readFile(const std::string& filepath);
};


#endif //DESKTOP_PET_SHADER_MANAGER_H