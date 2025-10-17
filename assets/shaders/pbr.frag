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

// 纹理
uniform sampler2D albedoMap;
uniform sampler2D normalMap;
uniform sampler2D metallicMap;
uniform sampler2D roughnessMap;
uniform sampler2D aoMap;

// 光照
uniform vec3 lightPositions[4];
uniform vec3 lightColors[4];
uniform vec3 viewPos;

// 是否使用纹理
uniform bool useAlbedoMap;
uniform bool useNormalMap;
uniform bool useMetallicMap;
uniform bool useRoughnessMap;
uniform bool useAoMap;

// 时间uniform用于动画
uniform float time;

void main() {
    // 基于UV坐标创建彩虹色
    vec3 color1 = vec3(1.0, 0.0, 0.0); // 红色
    vec3 color2 = vec3(0.0, 1.0, 0.0); // 绿色
    vec3 color3 = vec3(0.0, 0.0, 1.0); // 蓝色

    // 使用UV坐标创建渐变
    vec3 rainbow = mix(color1, color2, TexCoord.x);
    rainbow = mix(rainbow, color3, TexCoord.y);

    // 添加时间动画
    rainbow = mix(rainbow, vec3(1.0), sin(time * 2.0) * 0.3);

    // 添加一些闪烁效果
    float sparkle = sin(TexCoord.x * 10.0 + time) * sin(TexCoord.y * 10.0 + time);
    rainbow += sparkle * 0.2;

    FragColor = vec4(rainbow, 1.0);
}