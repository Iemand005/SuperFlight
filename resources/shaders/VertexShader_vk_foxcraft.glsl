#version 450

layout (location = 0) in ivec3 aPos;
layout (location = 1) in uint aNormalLayer;

layout (location = 0) out vec3 Normal;
layout (location = 1) out vec3 FragPos;
layout (location = 2) out vec3 TexCoord;

layout(push_constant) uniform PushConstants {
    mat4 model;
} pc;

layout(binding = 0) uniform UBO {
    mat4 view;
    mat4 projection;
};

vec3 decodeNormal(uint idx) {
    vec3 dirs[6] = vec3[](
        vec3(1,0,0), vec3(-1,0,0),
        vec3(0,1,0), vec3(0,-1,0),
        vec3(0,0,1), vec3(0,0,-1)
    );
    return dirs[idx];
}

void main()
{
    vec3 pos = vec3(aPos);
    uint face = aNormalLayer & 0x7u;
    uint layer = aNormalLayer >> 3;
    vec3 aNormal = decodeNormal(face);

    vec2 uv;
    if (face < 2) uv = pos.zy;
    else if (face < 4) uv = pos.xz;
    else uv = pos.xy;

    vec4 worldPos = pc.model * vec4(pos, 1.0);
    gl_Position = projection * view * worldPos;

    mat3 normalMat = transpose(inverse(mat3(pc.model)));
    Normal = normalMat * aNormal;
    FragPos = worldPos.xyz;
    TexCoord = vec3(uv, float(layer));
}
