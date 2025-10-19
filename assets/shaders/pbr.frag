#version 330 core
out vec4 FragColor;

in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoord;

// ---------------------- 纹理采样器 ----------------------
uniform sampler2D albedoMap;
uniform sampler2D normalMap;
uniform sampler2D metallicRoughness;
uniform sampler2D aoTex;
uniform sampler2D emissiveMap;

// ---------------------- 材质参数 ----------------------
uniform vec3 baseColorFactor;
uniform float alphaFactor;
uniform float metallicFactor;
uniform float roughnessFactor;

// ---------------------- 光照参数 ----------------------
uniform vec3 lightPositions[4];
uniform vec3 lightColors[4];
uniform vec3 viewPos;

// ---------------------- 工具函数 ----------------------
const float PI = 3.14159265359;

vec3 getNormalFromMap()
{
    // 如果没有法线贴图，直接返回顶点法线
    vec4 normalSample = texture(normalMap, TexCoord);

    // 检查是否是默认纹理（白色）
    if (length(normalSample.rgb - vec3(1.0, 1.0, 1.0)) < 0.01) {
        return normalize(Normal);  // 使用顶点法线
    }

    vec3 tangentNormal = normalSample.xyz * 2.0 - 1.0;
    vec3 Q1 = dFdx(FragPos);
    vec3 Q2 = dFdy(FragPos);
    vec2 st1 = dFdx(TexCoord);
    vec2 st2 = dFdy(TexCoord);

    vec3 N = normalize(Normal);
    vec3 T = normalize(Q1 * st2.t - Q2 * st1.t);
    vec3 B = -normalize(cross(N, T));
    mat3 TBN = mat3(T, B, N);

    return normalize(TBN * tangentNormal);
}

float DistributionGGX(vec3 N, vec3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    return a2 / (PI * denom * denom);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
    float denom = NdotV * (1.0 - k) + k;
    return NdotV / denom;
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx1 = GeometrySchlickGGX(NdotV, roughness);
    float ggx2 = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

// ---------------------- 主着色逻辑 ----------------------
void main()
{
    // 采样基础色纹理（包含 alpha 通道）
    vec4 albedoSample = texture(albedoMap, TexCoord);
    vec3 albedo = pow(albedoSample.rgb * baseColorFactor, vec3(2.2)); // gamma 校正
    float alpha = albedoSample.a * alphaFactor;

    // Alpha 测试：如果透明度太低，直接丢弃片段
    if (alpha < 0.01) {
        discard;
    }

    // 获取法线（自动处理缺失的法线贴图）
    vec3 N = getNormalFromMap();
    vec3 V = normalize(viewPos - FragPos);

    // 从 metallicRoughness 贴图提取，如果没有贴图则使用 uniform 值
    vec4 mrSample = texture(metallicRoughness, TexCoord);
    bool hasMetallicRoughnessMap = length(mrSample.rgb - vec3(1.0, 1.0, 1.0)) > 0.01;

    float metallic = hasMetallicRoughnessMap ? (mrSample.b * metallicFactor) : metallicFactor;
    float roughness = hasMetallicRoughnessMap ? clamp(mrSample.g * roughnessFactor, 0.04, 1.0) : clamp(roughnessFactor, 0.04, 1.0);

    // AO 贴图（如果没有则默认为 1.0，即无遮蔽）
    vec4 aoSample = texture(aoTex, TexCoord);
    float ao = (length(aoSample.rgb - vec3(1.0, 1.0, 1.0)) > 0.01) ? aoSample.r : 1.0;

    // 基础反射率
    vec3 F0 = vec3(0.04);
    F0 = mix(F0, albedo, metallic);

    // 光照计算
    vec3 Lo = vec3(0.0);
    for(int i = 0; i < 4; ++i)
    {
        vec3 L = normalize(lightPositions[i] - FragPos);
        vec3 H = normalize(V + L);
        float distance = length(lightPositions[i] - FragPos);
        float attenuation = 1.0 / (distance * distance);
        vec3 radiance = lightColors[i] * attenuation;

        float NDF = DistributionGGX(N, H, roughness);
        float G   = GeometrySmith(N, V, L, roughness);
        vec3 F    = fresnelSchlick(max(dot(H, V), 0.0), F0);

        vec3 numerator    = NDF * G * F;
        float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.001;
        vec3 specular = numerator / denominator;

        vec3 kS = F;
        vec3 kD = vec3(1.0) - kS;
        kD *= 1.0 - metallic;

        float NdotL = max(dot(N, L), 0.0);
        Lo += (kD * albedo / PI + specular) * radiance * NdotL;
    }

    // Emissive 贴图（如果没有则为黑色）
    vec4 emissiveSample = texture(emissiveMap, TexCoord);
    vec3 emissive = (length(emissiveSample.rgb - vec3(1.0, 1.0, 1.0)) > 0.01) ? emissiveSample.rgb : vec3(0.0);

    // 增强环境光
    vec3 ambient = vec3(0.15) * albedo * ao;

    vec3 color = ambient + Lo + emissive;
    color = color / (color + vec3(1.0));       // HDR tone mapping
    color = pow(color, vec3(1.0 / 2.2));       // gamma 校正输出

    FragColor = vec4(color, alpha);
}