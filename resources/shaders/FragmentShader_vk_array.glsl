#version 450

layout (location = 0) out vec4 FragColor;

layout (location = 0) in vec3 Normal;
layout (location = 1) in vec3 FragPos;
layout (location = 2) in vec3 TexCoord;

layout(binding = 0) uniform UBO {
    mat4 view;
    mat4 projection;
};

layout(binding = 1) uniform sampler2DArray ourTexture;

const vec3 sunDir = normalize(vec3(0.5, 0.8, 0.3));

const vec3 skyColor = vec3(0.6, 0.7, 1.0);
const vec3 groundColor = vec3(0.3, 0.2, 0.1);

void main()
{
    vec4 texColor = texture(ourTexture, TexCoord);

    vec3 normal = normalize(Normal);

    float NdotL = max(dot(normal, sunDir), 0.0);

    float hemisphere = 0.5 + 0.5 * dot(normal, vec3(0.0, 1.0, 0.0));
    vec3 ambient = mix(groundColor, skyColor, hemisphere);

    float light = 0.3 + 0.7 * NdotL;

    vec3 lit = texColor.rgb * light;

    lit = mix(lit, lit * ambient, 0.3);

    FragColor = vec4(lit, texColor.a);
}
