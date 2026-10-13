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

		cube_ = std::make_shared<fe::Object>();
		cube_->name = "Cube";
		cube_->color = glm::vec3(1.0f, 0.45f, 0.0f);
		cube_->PushMesh(fe::Primitives::GeneratePlane(10.0f, 10.0f));
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
		ProcessInput();
		const float dt = static_cast<float>(fpsCounter.deltaTime);
		if (cube_)
		{
			// cube_->state.rotation.y += 45.0f * dt;
			// cube_->state.rotation.x += 20.0f * dt;
			// cube_->modelMatrixDirty = true;
		}
		Redraw();
	}

	void ProcessInput()
	{
		SDL_Event event;
		fe::SDLWindow *window = GetWindow<fe::SDLWindow>();
		while (window->PollSDLEvent(&event))
		{
			ImGui_ImplSDL3_ProcessEvent(&event);
			auto io = ImGui::GetIO();
			switch (event.type)
			{
			case SDL_EVENT_QUIT:
				window->PrepareClose();
				break;
			case SDL_EVENT_MOUSE_BUTTON_DOWN:
				if (event.button.which == SDL_TOUCH_MOUSEID)
					break; // touch handles its own events below
				if (event.button.button == SDL_BUTTON_LEFT && !io.WantCaptureMouse)
				{
					window->StartMouseCapture();
					RefreshJoysticks();
				}
				if (window->IsCapturingMouse())
				{
					// if (event.button.button == SDL_BUTTON_LEFT)
					// 	PlaceBlock(true);
					// else if (event.button.button == SDL_BUTTON_RIGHT)
					// 	PlaceBlock(false);
				}
				break;
			case SDL_EVENT_WINDOW_RESIZED:
			case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
				Redraw();
				break;
			case SDL_EVENT_MOUSE_MOTION:
			{
				if (event.motion.which == SDL_TOUCH_MOUSEID)
					break; // touch looks around via SDL_EVENT_FINGER_MOTION
				if (!window->IsCapturingMouse())
					break;
				float sensitivity = 0.1f;
				camera->yaw += event.motion.xrel * sensitivity;
				camera->pitch -= event.motion.yrel * sensitivity;
				camera->UpdateDirection();
				camera->pitch = std::clamp(camera->pitch, -89.0f, 89.0f);
				break;
			}
			case SDL_EVENT_FINGER_DOWN:
			{
				if (event.tfinger.fingerID != 0)
					break; // only the primary finger drives look/capture
				if (!io.WantCaptureMouse)
				{
					if (!window->IsCapturingMouse())
					{
						window->StartMouseCapture();
						RefreshJoysticks();
					}
					else
					{
						// PlaceBlock(true);
					}
				}
				break;
			}
			case SDL_EVENT_FINGER_MOTION:
			{
				if (event.tfinger.fingerID != 0)
					break;
				if (!window->IsCapturingMouse())
					break;
				float sensitivity = 0.1f;
				camera->yaw += event.tfinger.dx * window->width * sensitivity;
				camera->pitch -= event.tfinger.dy * window->height * sensitivity;
				camera->UpdateDirection();
				camera->pitch = std::clamp(camera->pitch, -89.0f, 89.0f);
				break;
			}
			case SDL_EVENT_FINGER_UP:
				if (event.tfinger.fingerID == 0 && !io.WantCaptureMouse)
					window->StopMouseCapture();
				break;
			case SDL_EVENT_KEY_DOWN:
				if (event.key.key == SDLK_F11)
				{
					window->ToggleFullscreen();
				}
				else if (event.key.key == SDLK_F3)
				{
					showDebugUI = !showDebugUI;
				}
				else if (event.key.key == SDLK_F2)
				{
					// freeCamera = !freeCamera;
					window->StartMouseCapture();
					RefreshJoysticks();
				}
				else if (event.key.key == SDLK_F4)
				{
					ToggleXR();
				}
				break;
			}
		}

		

		if (droneMode)
		{
			const float dt = static_cast<float>(fpsCounter.deltaTime);

			if (window->IsKeyDown(SDL_SCANCODE_LEFT))  camera->yaw -= droneLookSpeed * dt;
			if (window->IsKeyDown(SDL_SCANCODE_RIGHT)) camera->yaw += droneLookSpeed * dt;
			if (window->IsKeyDown(SDL_SCANCODE_UP))    camera->pitch += droneLookSpeed * dt;
			if (window->IsKeyDown(SDL_SCANCODE_DOWN))  camera->pitch -= droneLookSpeed * dt;
			camera->pitch = std::clamp(camera->pitch, -89.0f, 89.0f);
			camera->UpdateDirection();

			glm::vec3 front = camera->front;
			glm::vec3 right = glm::normalize(glm::cross(front, glm::vec3(0.0f, 1.0f, 0.0f)));
			glm::vec3 move(0.0f);

			if (window->IsKeyDown(SDL_SCANCODE_W)) move += front;
			if (window->IsKeyDown(SDL_SCANCODE_S)) move -= front;
			if (window->IsKeyDown(SDL_SCANCODE_D)) move += right;
			if (window->IsKeyDown(SDL_SCANCODE_A)) move -= right;
			if (window->IsKeyDown(SDL_SCANCODE_SPACE)) move += glm::vec3(0.0f, 1.0f, 0.0f);
			if (window->IsKeyDown(SDL_SCANCODE_LSHIFT)) move -= glm::vec3(0.0f, 1.0f, 0.0f);

			if (glm::length(move) > 0.0f)
				camera->SetPos(camera->GetPos() + glm::normalize(move) * droneSpeed * dt);
		}

		if (window->IsKeyDown(SDL_SCANCODE_ESCAPE))
			window->StopMouseCapture();
		if (ImGui::GetIO().WantCaptureMouse)
			window->StopMouseCapture();
	}

	void InitUI() override {}

	void DrawUI() override
	{
		if (!showDebugUI) return;
		BeginFrame();

		ImGui::Begin("SuperFlight");
		ImGui::Text("Drone Mode: %s", droneMode ? "ON" : "OFF");
		if (ImGui::Button(droneMode ? "Disable Drone Mode" : "Enable Drone Mode"))
			droneMode = !droneMode;
		ImGui::TextDisabled("WASD move, Space up, Shift down, Arrows rotate");
		ImGui::SliderFloat("Drone Speed", &droneSpeed, 1.0f, 50.0f);
		ImGui::SliderFloat("Rotate Speed", &droneLookSpeed, 10.0f, 360.0f);
		ImGui::End();

		DrawDebugUI();
		EndFrame();
	}

private:
	bool showDebugUI = false;
	bool droneMode = false;
	float droneSpeed = 8.0f;
	float droneLookSpeed = 90.0f;
	std::shared_ptr<fe::Object> cube_;
};
