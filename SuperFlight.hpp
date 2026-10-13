#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <memory>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include <glm/glm.hpp>

#include <EditableGame.hpp>
#include <Object.hpp>
#include <Primitives.hpp>

// Resolve a runtime path against the folder that holds the executable, so the
// game finds its SPIR-V shaders no matter what working directory it is launched
// from (engine loads shaders relative to the process working directory).
static std::string SuperFlightExecutableDir()
{
#ifdef _WIN32
	char buffer[MAX_PATH] = {};
	const DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
	if (length == 0 || length >= MAX_PATH)
		return std::string();

	std::string path(buffer, length);
	const std::string::size_type slash = path.find_last_of("\\/");
	return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
#else
	return std::string();
#endif
}

class SuperFlight : public fe::EditableGame
{
public:
	SuperFlight(fe::XRGameOptions options) : fe::EditableGame(options)
	{
		SetClearColor(0.05f, 0.07f, 0.12f);

		const std::string base = SuperFlightExecutableDir();
		LoadVulkanShaders(base + "resources/shaders/VertexShader_vk.spv",
			base + "resources/shaders/FragmentShader_vk.spv");

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
