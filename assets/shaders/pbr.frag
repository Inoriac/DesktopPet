#version 330 core
out vec4 FragColor;

in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoord;

// 材质属性
uniform vec3 albedo;
uniform float metallic;
uniform float roughness;
uniform float ao;

void main() {
    // 简单的光照计算
    vec3 lightDir = normalize(vec3(1.0, 1.0, 1.0));
    float diff = max(dot(normalize(Normal), lightDir), 0.0);

    // 环境光 + 漫反射
    vec3 ambient = albedo * 0.3;
    vec3 diffuse = albedo * diff * 0.7;
    vec3 result = ambient + diffuse;

    FragColor = vec4(result, 1.0);
}