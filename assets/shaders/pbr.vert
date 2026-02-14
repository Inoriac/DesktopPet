#version 330 core

layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aTexCoord;
layout (location = 3) in vec4 aBoneIds;  // 新增
layout (location = 4) in vec4 aWeights;  // 新增

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

const int MAX_BONES = 200;
uniform mat4 finalBonesMatrices[200]; // 必须与 C++ 上传数量一致

out vec3 FragPos;
out vec3 Normal;
out vec2 TexCoord;

void main()
{
    vec4 totalPosition = vec4(0.0f);
    vec3 localNormal = vec3(0.0f);

    float weightSum = aWeights.x + aWeights.y + aWeights.z + aWeights.w;

    // 保护逻辑：只有当权重总和 > 0.001 时才应用骨骼变换
    // 否则视为静态物体（Static Mesh），直接使用原始坐标
    if (weightSum > 0.001) {
        for(int i = 0 ; i < 4 ; i++)
        {
            //增加 0.1 的偏移，确保 5.0 不会被识别为 4
            int index = int(aBoneIds[i] + 0.1);

            if (index < 0 || index >= MAX_BONES) continue;// 增加对 < 0 的检查

            totalPosition += (finalBonesMatrices[index] * vec4(aPos, 1.0f)) * aWeights[i];
            localNormal   += (mat3(finalBonesMatrices[index]) * aNormal) * aWeights[i];
        }
    } else {
        totalPosition = vec4(aPos, 1.0f);
        localNormal = aNormal;
    }

    FragPos = vec3(model * totalPosition);

    if (weightSum > 0.001) {
        Normal = normalize(mat3(model) * localNormal);
    } else {
        Normal = normalize(mat3(transpose(inverse(model))) * aNormal);
    }

    TexCoord = aTexCoord;
    gl_Position = projection * view * vec4(FragPos, 1.0);
}