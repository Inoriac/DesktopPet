#pragma once

// 代表一个几何体
struct GpuMesh {
    // 具体值是 GPU 内部资源的句柄
    unsigned int vao {0};   // 配置文件位置，用于指导如何使用资源
    unsigned int vbo {0};   // 数据存储位置
    unsigned int ebo {0};   // 网格中顶点的连接方式
    int indexCount {0};
    int materialIndex {0};  // 材质索引
};
