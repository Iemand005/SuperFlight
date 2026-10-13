#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <memory>
#include <string>

#include <glm/glm.hpp>

#include <EditableGame.hpp>
#include <Object.hpp>
#include <Primitives.hpp>

class SuperFlight : public fe::EditableGame
{
public:
	SuperFlight(fe::XRGameOptions options) : fe::EditableGame(options)
	{
		SetClearColor(0.05f, 0.07f, 0.12f);

		// Renders with the engine's default Vulkan shaders
		// (resources/shaders/VertexShader_vk.spv / FragmentShader_vk.spv), which
		// CMake compiles to SPIR-V and copies next to the executable.

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

	void DrawUI() override
	{
		BeginFrame();
		EndFrame();
	}

private:
	std::shared_ptr<fe::Object> cube_;
};
