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

		auto groundMesh = fe::Primitives::GeneratePlane(fe::PlaneDirection::Top, 400.0f, 400.0f);
		ground_ = std::make_shared<fe::Object>(groundMesh);
		ground_->name = "Ground";
		ground_->color = glm::vec3(0.12f, 0.13f, 0.16f);
		ground_->isStatic = true;
		if (auto* physics = GetPhysicsFactory())
		{
			ground_->SetPhysicsObject(physics->CreateObject(groundMesh.vertices, groundMesh.indices));
			if (ground_->physicsObject)
				ground_->physicsObject->SetPosition(glm::vec3(0.0f));
		}
		scene->AddObject(ground_);

		camera->SetPos(glm::vec3(0.0f, 2.0f, 0.0f));
		camera->LookAt(glm::vec3(0.0f, 2.0f, -10.0f));
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

		if (droneMode && droneBody_)
			DroneUpdate(GetWindow<fe::SDLWindow>(), dt);

		Update();

		if (droneMode && droneBody_)
			camera->SetPos(droneBody_->GetPosition());

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
				if (!io.WantCaptureMouse && !window->IsCapturingMouse())
				{
					window->StartMouseCapture();
					RefreshJoysticks();
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

		if (window->IsKeyDown(SDL_SCANCODE_ESCAPE))
			window->StopMouseCapture();
		if (ImGui::GetIO().WantCaptureMouse)
			window->StopMouseCapture();
	}

	void DroneUpdate(fe::SDLWindow *window, float dt)
	{
		droneBody_->SetDamping(droneLinearDamping, droneAngularDamping);

		if (window->IsKeyDown(SDL_SCANCODE_LEFT))  camera->yaw -= droneYawSpeed * dt;
		if (window->IsKeyDown(SDL_SCANCODE_RIGHT)) camera->yaw += droneYawSpeed * dt;
		if (window->IsKeyDown(SDL_SCANCODE_UP))    camera->pitch += dronePitchSpeed * dt;
		if (window->IsKeyDown(SDL_SCANCODE_DOWN))  camera->pitch -= dronePitchSpeed * dt;
		camera->pitch = std::clamp(camera->pitch, -dronePitchLimit, dronePitchLimit);
		camera->UpdateDirection();

		float yawRad = glm::radians(camera->yaw);
		glm::vec3 forward(cosf(yawRad), 0.0f, sinf(yawRad));
		forward = glm::normalize(forward);
		glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));

		glm::vec3 accel(0.0f);
		if (window->IsKeyDown(SDL_SCANCODE_W)) accel += forward;
		if (window->IsKeyDown(SDL_SCANCODE_S)) accel -= forward;
		if (window->IsKeyDown(SDL_SCANCODE_D)) accel += right;
		if (window->IsKeyDown(SDL_SCANCODE_A)) accel -= right;

		float horizontal = glm::length(accel);
		if (horizontal > 0.0f)
			accel = (accel / horizontal) * droneThrust;

		float vertical = 0.0f;
		if (droneAltitudeHold) vertical += droneGravity;
		if (window->IsKeyDown(SDL_SCANCODE_SPACE))  vertical += droneLift;
		if (window->IsKeyDown(SDL_SCANCODE_LSHIFT)) vertical -= droneLift;

		glm::vec3 force = accel + glm::vec3(0.0f, vertical, 0.0f);
		droneBody_->AddForce(force);
	}

	void EnsureDrone()
	{
		if (droneObject_)
			return;
		auto* physics = GetPhysicsFactory();
		if (!physics)
			return;

		auto mesh = fe::Primitives::GenerateCube(
			{fe::PlaneDirection::Front, fe::PlaneDirection::Back,
			 fe::PlaneDirection::Left, fe::PlaneDirection::Right,
			 fe::PlaneDirection::Top, fe::PlaneDirection::Bottom},
			0.4f);
		droneObject_ = std::make_shared<fe::Object>(mesh);
		droneObject_->name = "Drone";
		droneObject_->color = glm::vec3(1.0f, 0.45f, 0.0f);
		droneObject_->state.position = glm::vec3(0.0f, 2.0f, 0.0f);

		droneObject_->SetPhysicsObject(physics->CreateSphereObject(0.25f, true));
		droneBody_ = droneObject_->physicsObject.get();
		if (droneBody_)
		{
			droneBody_->SetPosition(droneObject_->state.position);
			droneBody_->SetDamping(droneLinearDamping, droneAngularDamping);
			droneBody_->SetAngularVelocity(glm::vec3(0.0f));
		}

		scene->AddObject(droneObject_);
		camera->SetPos(droneObject_->state.position);
	}

	void InitUI() override {}

	void DrawUI() override
	{
		if (!showDebugUI) return;
		BeginFrame();

		ImGui::Begin("SuperFlight");
		ImGui::Text("Drone Mode: %s", droneMode ? "ON" : "OFF");
		if (ImGui::Button(droneMode ? "Disable Drone Mode" : "Enable Drone Mode"))
		{
			droneMode = !droneMode;
			if (droneMode) EnsureDrone();
		}
		ImGui::TextDisabled("W/S forward-back (level), A/D strafe, Space up, Shift down");
		ImGui::TextDisabled("Arrows: yaw/pitch. Mouse: FPV look.");
		ImGui::Separator();
		ImGui::Checkbox("Altitude hold", &droneAltitudeHold);
		ImGui::SliderFloat("Thrust", &droneThrust, 0.0f, 80.0f, "%.1f m/s2");
		ImGui::SliderFloat("Lift", &droneLift, 0.0f, 60.0f, "%.1f m/s2");
		ImGui::SliderFloat("Yaw Speed", &droneYawSpeed, 10.0f, 360.0f, "%.0f deg/s");
		ImGui::SliderFloat("Pitch Speed", &dronePitchSpeed, 10.0f, 360.0f, "%.0f deg/s");
		ImGui::SliderFloat("Linear Damping", &droneLinearDamping, 0.0f, 5.0f);
		ImGui::SliderFloat("Angular Damping", &droneAngularDamping, 0.0f, 10.0f);
		if (droneBody_)
		{
			glm::vec3 v = droneBody_->GetLinearVelocity();
			ImGui::Text("Speed: %.1f m/s", glm::length(v));
			ImGui::Text("Velocity: %.2f, %.2f, %.2f", v.x, v.y, v.z);
		}
		ImGui::End();

		DrawDebugUI();
		EndFrame();
	}

private:
	bool showDebugUI = false;
	bool droneMode = false;

	float droneThrust = 30.0f;
	float droneLift = 18.0f;
	float droneYawSpeed = 120.0f;
	float dronePitchSpeed = 90.0f;
	float dronePitchLimit = 60.0f;
	float droneLinearDamping = 1.2f;
	float droneAngularDamping = 4.0f;
	float droneGravity = 9.81f;
	bool droneAltitudeHold = true;

	std::shared_ptr<fe::Object> ground_;
	std::shared_ptr<fe::Object> droneObject_;
	fe::PhysicsObject* droneBody_ = nullptr;
};
