#version 300 es
precision highp float;
precision highp sampler2D;
precision highp sampler2DArray;

in vec3 Normal;
in vec3 FragPos;
in vec3 TexCoord;
in float BlockLight;

out vec4 FragColor;

uniform sampler2D ourTexture;
uniform sampler2DArray ourTextureArray;
uniform bool useTextureArray;
uniform vec3 objectColor;
uniform int lightCount;

struct PointLight {
    vec3 position;
    vec3 color;
    float intensity;
    float radius;
};

uniform PointLight pointLights[8];

void main()
{
    vec4 texel = useTextureArray
        ? texture(ourTextureArray, TexCoord)
        : texture(ourTexture, TexCoord.xy);
    vec3 albedo = texel.rgb * objectColor;
    vec3 normal = normalize(Normal);
    vec3 sunDirection = normalize(vec3(0.5, 0.8, 0.3));
    float directional = max(dot(normal, sunDirection), 0.0);
    float lighting = useTextureArray
        ? max(0.25 + 0.75 * directional, BlockLight)
        : 0.2 + 0.8 * directional;

    for (int i = 0; i < lightCount; ++i) {
        vec3 toLight = pointLights[i].position - FragPos;
        float distanceToLight = length(toLight);
        float radius = max(pointLights[i].radius, 0.001);
        float attenuation = 1.0 / (1.0 + distanceToLight * distanceToLight / (radius * radius));
        lighting += max(dot(normal, normalize(toLight)), 0.0)
            * pointLights[i].intensity * attenuation;
    }

    FragColor = vec4(albedo * lighting, texel.a);
}
