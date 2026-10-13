#version 450

layout (location = 0) out vec4 FragColor;

layout (location = 0) in vec3 Normal;
layout (location = 1) in vec3 FragPos;
layout (location = 2) in vec2 TexCoord;
layout (location = 3) in vec4 ObjectColor;

layout(binding = 0) uniform UBO {
    mat4 view;
    mat4 projection;
};

layout(binding = 1) uniform sampler2D ourTexture;

void main()
{
    vec4 baseColor = texture(ourTexture, TexCoord) * ObjectColor;

    vec3 norm = normalize(Normal);
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    float diff = max(dot(norm, lightDir), 0.0);

    vec3 ambient = 0.2 * baseColor.rgb;
    vec3 diffuse = diff * baseColor.rgb;

    FragColor = vec4(ambient + diffuse, baseColor.a);
}
