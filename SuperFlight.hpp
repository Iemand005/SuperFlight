#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <algorithm>
#include <memory>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <EditableGame.hpp>
#include <Object.hpp>
#include <Primitives.hpp>

class SuperFlight : public fe::EditableGame
{
public:
	SuperFlight(fe::XRGameOptions options) : fe::EditableGame(options)
	{
		SetClearColor(0.05f, 0.07f, 0.12f);

		LoadShaderTexts(kVertexShader, kFragmentShader);
		renderDevice->EnableDepthTest();

		auto* lights = scene->GetLights();
		lights[0].position = glm::vec3(4.0f, 6.0f, 5.0f);
		lights[0].color = glm::vec3(1.0f);
		lights[0].intensity = 1.0f;
		lights[0].radius = 40.0f;

		cube_ = std::make_shared<fe::Object>();
		cube_->name = "Cube";
		cube_->color = glm::vec3(1.0f, 0.45f, 0.0f);
		cube_->PushMesh(fe::Primitives::GenerateCube(2.0f));
		scene->AddObject(cube_);

		camera->SetPos(glm::vec3(0.0f, 1.5f, 6.0f));
		camera->LookAt(glm::vec3(0.0f));
	}

	void Init() override
	{
		auto window = GetWindow<fe::SDLWindow>();
		window->Show();
		window->DisableVSync();
		camera->SetAspect(window->width, window->height);
	}

	void Step() override
	{
		const float dt = static_cast<float>(fpsCounter.deltaTime);
		if (cube_)
		{
			cube_->state.rotation.y += 45.0f * dt;
			cube_->state.rotation.x += 20.0f * dt;
			cube_->modelMatrixDirty = true;
		}
		Redraw();
	}

	void InitUI() override {}

	void DrawUI() override {}

private:
	std::shared_ptr<fe::Object> cube_;

	static constexpr const char* kVertexShader = R"glsl(#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aTexCoord;

out vec3 Normal;
out vec3 FragPos;
out vec2 TexCoord;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform float time;

void main()
{
    vec4 worldPos = model * vec4(aPos, 1.0);
    gl_Position = projection * view * worldPos;

    Normal = mat3(transpose(inverse(model))) * aNormal;
    FragPos = worldPos.xyz;
    TexCoord = aTexCoord;
}
)glsl";

	static constexpr const char* kFragmentShader = R"glsl(#version 330 core
out vec4 FragColor;

in vec3 Normal;
in vec3 FragPos;
in vec2 TexCoord;

struct PointLight {
    vec3 position;
    vec3 color;
    float intensity;
    float radius;
};

uniform int lightCount;
uniform PointLight pointLights[8];
uniform vec3 objectColor;

void main()
{
    vec3 n = normalize(Normal);
    vec3 lighting = vec3(0.15);

    for (int i = 0; i < lightCount; ++i) {
        vec3 L = pointLights[i].position - FragPos;
        float dist = length(L);
        if (dist < 0.0001) continue;
        vec3 ldir = L / dist;
        float diff = max(dot(n, ldir), 0.0);
        float radius = max(pointLights[i].radius, 0.001);
        float atten = 1.0 / (1.0 + (dist * dist) / (radius * radius));
        lighting += pointLights[i].color * pointLights[i].intensity * diff * atten;
    }

    FragColor = vec4(objectColor * lighting, 1.0);
}
)glsl";
};
