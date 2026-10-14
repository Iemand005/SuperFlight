#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <memory>
#include <string>

#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <EditableGame.hpp>
#include <Object.hpp>
#include <Primitives.hpp>

#include "ChunkManager.hpp"
#ifdef FE_HAS_VULKAN
#include "ChunkMesher.hpp"
#include "ChunkBatcher.hpp"
#endif

class SuperFlight : public fe::EditableGame
{
public:
	SuperFlight(fe::XRGameOptions options) : fe::EditableGame(options)
	{
		SetClearColor(0.05f, 0.07f, 0.12f);

#ifdef FE_HAS_VULKAN
		if (options.useVulkan)
		{
			LoadArrayShaders("resources/shaders/VertexShader_vk_array.spv", "resources/shaders/FragmentShader_vk_array.spv");
			LoadFoxcraftShaders("resources/shaders/VertexShader_vk_foxcraft.spv", "resources/shaders/FragmentShader_vk_array.spv");
			chunkBatcher_ = std::make_unique<ChunkBatcher>(
				static_cast<fe::VulkanDevice *>(renderDevice.get()),
				ChunkMesher::BuildBlockTextureImages());
			chunkManager->SetBatcher(chunkBatcher_.get());
			chunkManager->SetUseBatcherPath(true);
		}
#endif

		camera->SetPos(glm::vec3(0.0f, 70.0f, 0.0f));
		camera->LookAt(glm::vec3(0.0f, 70.0f, -10.0f));
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

		if (airplaneMode && airplaneBody_)
			AirplaneUpdate(GetWindow<fe::SDLWindow>(), dt);

		UpdateLoadedChunks();

		{
			glm::vec2 forward2D = glm::normalize(glm::vec2(camera->front.x, camera->front.z));
			chunkManager->UpdatePausedState(playerCenter_, forward2D);
		}

		chunkManager->Update(1, GetPhysicsFactory(), scene.get(),
							 playerCenter_, chunkLoadDistance, physicsDistance);

		Update();

		if (droneMode && droneBody_)
			camera->SetPos(droneBody_->GetPosition());

		if (airplaneMode && airplaneBody_)
			AirplaneCameraUpdate();

		Redraw();
	}

	void UpdateLoadedChunks()
	{
		glm::vec3 pos = camera->GetPos();
		int chunkX = static_cast<int>(std::floor(pos.x / static_cast<float>(Chunk::WIDTH)));
		int chunkZ = static_cast<int>(std::floor(pos.z / static_cast<float>(Chunk::DEPTH)));
		playerCenter_ = {chunkX, chunkZ};

		int terrainDist = chunkLoadDistance + chunkOutgenDistance;
		glm::vec2 forward2D = glm::normalize(glm::vec2(camera->front.x, camera->front.z));
		chunkManager->LoadChunksInsideRange(playerCenter_, terrainDist, forward2D);
		chunkManager->UnloadChunksOutsideRange(playerCenter_, terrainDist);
	}

	void OnDraw() override
	{
		fe::EditableGame::OnDraw();
#ifdef FE_HAS_VULKAN
		if (chunkBatcher_ && useVulkan)
		{
			chunkBatcher_->Update(camera->GetPos());
			chunkBatcher_->Draw();
		}
#endif
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
		droneObject_->state.position = glm::vec3(0.0f, 70.0f, 0.0f);

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

	void AirplaneUpdate(fe::SDLWindow *window, float dt)
	{
		airplaneBody_->SetDamping(0.0f, 0.0f);

		float throttleInput = 0.0f;
		if (window->IsKeyDown(SDL_SCANCODE_LSHIFT)) throttleInput += 1.0f;
		if (window->IsKeyDown(SDL_SCANCODE_LCTRL))  throttleInput -= 1.0f;
		airplaneThrottle = glm::clamp(airplaneThrottle + throttleInput * airplaneThrottleRate * dt, 0.0f, 1.0f);

		float pitchIn = 0.0f, rollIn = 0.0f, yawIn = 0.0f;
		if (window->IsKeyDown(SDL_SCANCODE_W)) pitchIn += 1.0f;
		if (window->IsKeyDown(SDL_SCANCODE_S)) pitchIn -= 1.0f;
		if (window->IsKeyDown(SDL_SCANCODE_A)) rollIn  += 1.0f;
		if (window->IsKeyDown(SDL_SCANCODE_D)) rollIn  -= 1.0f;
		if (window->IsKeyDown(SDL_SCANCODE_Q)) yawIn   += 1.0f;
		if (window->IsKeyDown(SDL_SCANCODE_E)) yawIn   -= 1.0f;
		if (airplaneInvertPitch) pitchIn = -pitchIn;

		glm::quat rot = airplaneBody_->GetRotation();
		glm::vec3 right = rot * glm::vec3(1.0f, 0.0f, 0.0f);
		glm::vec3 up    = rot * glm::vec3(0.0f, 1.0f, 0.0f);
		glm::vec3 fwd   = rot * glm::vec3(0.0f, 0.0f, -1.0f);

		// Rate-based yoke: input commands an angular velocity around the body axes.
		glm::vec3 omega(0.0f);
		omega += right * glm::radians(pitchIn * airplanePitchRate);
		omega += fwd   * glm::radians(rollIn  * airplaneRollRate);
		omega += up    * glm::radians(yawIn   * airplaneYawRate);

		// Space: gently roll to level and bring the nose back to the horizon.
		if (window->IsKeyDown(SDL_SCANCODE_SPACE))
		{
			glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
			omega += glm::cross(up, worldUp) * airplaneLevelStrength;
			glm::vec3 fwdH(fwd.x, 0.0f, fwd.z);
			if (glm::length(fwdH) > 1e-4f)
				omega -= glm::cross(fwd, glm::normalize(fwdH)) * airplaneLevelStrength;
		}
		airplaneBody_->SetAngularVelocity(omega);

		// Throttle drives the airspeed toward a target; gravity "sags" the plane
		// when it is too slow (no lift), so stalls sink toward the ground.
		float targetSpeed = airplaneThrottle * airplaneMaxSpeed;
		airplaneAirSpeed += (targetSpeed - airplaneAirSpeed) * glm::clamp(airplaneAccel * dt, 0.0f, 1.0f);

		glm::vec3 velocity = fwd * airplaneAirSpeed;
		float liftFactor = glm::clamp(airplaneAirSpeed / airplaneStallSpeed, 0.0f, 1.0f);
		velocity.y -= airplaneGravitySag * (1.0f - liftFactor);
		airplaneBody_->SetLinearVelocity(velocity);
	}

	void AirplaneCameraUpdate()
	{
		glm::quat rot = airplaneBody_->GetRotation();
		glm::vec3 pos = airplaneBody_->GetPosition();
		glm::vec3 fwd = rot * glm::vec3(0.0f, 0.0f, -1.0f);
		glm::vec3 up  = rot * glm::vec3(0.0f, 1.0f, 0.0f);

		if (airplaneChaseCam)
		{
			glm::vec3 camPos = pos - fwd * airplaneCamDistance + glm::vec3(0.0f, airplaneCamHeight, 0.0f);
			glm::vec3 dir = glm::normalize(pos - camPos);
			camera->front = dir;
			camera->up = glm::vec3(0.0f, 1.0f, 0.0f);
			camera->updateView(camPos, dir, camera->up);
			camera->pitch = glm::degrees(glm::asin(glm::clamp(dir.y, -1.0f, 1.0f)));
			camera->yaw = glm::degrees(glm::atan(dir.z, dir.x));
		}
		else
		{
			glm::vec3 camPos = pos + fwd * airplaneCockpitOffset.x + up * airplaneCockpitOffset.y;
			camera->front = fwd;
			camera->up = up;
			camera->updateView(camPos, fwd, up);
			camera->pitch = glm::degrees(glm::asin(glm::clamp(fwd.y, -1.0f, 1.0f)));
			camera->yaw = glm::degrees(glm::atan(fwd.z, fwd.x));
		}
	}

	static fe::Mesh<fe::Vertex> BuildAirplaneMesh()
	{
		std::vector<fe::Vertex> vertices;
		std::vector<uint32_t> indices;
		auto addBox = [&](glm::vec3 center, glm::vec3 half)
		{
			auto cube = fe::Primitives::GenerateCube(1.0f);
			uint32_t base = static_cast<uint32_t>(vertices.size());
			for (auto& v : cube.vertices)
			{
				v.position = center + v.position * (half * 2.0f);
				vertices.push_back(v);
			}
			for (auto i : cube.indices)
				indices.push_back(base + i);
		};

		// Local forward is -Z (matches the camera's -Z convention), so the nose
		// and tail are mirrored on Z relative to a naive layout.
		addBox({0.0f, 0.0f, 0.0f},     {0.25f, 0.25f, 1.6f});  // fuselage
		addBox({0.0f, 0.0f, -0.2f},    {1.8f, 0.06f, 0.4f});   // main wings
		addBox({0.0f, 0.0f, 1.4f},     {0.6f, 0.05f, 0.25f});  // horizontal stabilizer
		addBox({0.0f, 0.35f, 1.4f},    {0.05f, 0.35f, 0.3f});  // vertical fin

		return fe::Mesh<fe::Vertex>(vertices, indices);
	}

	void EnsureAirplane()
	{
		if (airplaneObject_)
			return;
		auto* physics = GetPhysicsFactory();
		if (!physics)
			return;

		airplaneObject_ = std::make_shared<fe::Object>(BuildAirplaneMesh());
		airplaneObject_->name = "Airplane";
		airplaneObject_->color = glm::vec3(0.85f, 0.20f, 0.20f);
		glm::vec3 spawn(0.0f, 80.0f, 0.0f);
		airplaneObject_->state.position = spawn;

		airplaneObject_->SetPhysicsObject(physics->CreateObject(glm::vec3(3.6f, 1.4f, 3.2f), true, true));
		airplaneBody_ = airplaneObject_->physicsObject.get();
		if (airplaneBody_)
		{
			airplaneBody_->SetPosition(spawn);
			airplaneBody_->SetDamping(0.0f, 0.0f);
			airplaneBody_->SetAngularVelocity(glm::vec3(0.0f));
			airplaneBody_->SetLinearVelocity(glm::vec3(0.0f));
		}

		scene->AddObject(airplaneObject_);
		airplaneThrottle = 0.0f;
		airplaneAirSpeed = 0.0f;
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
			if (droneMode) { airplaneMode = false; EnsureDrone(); }
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
		ImGui::Separator();
		ImGui::Text("Airplane Mode: %s", airplaneMode ? "ON" : "OFF");
		if (ImGui::Button(airplaneMode ? "Disable Airplane Mode" : "Enable Airplane Mode"))
		{
			airplaneMode = !airplaneMode;
			if (airplaneMode) { droneMode = false; EnsureAirplane(); }
		}
		ImGui::TextDisabled("W/S pitch, A/D roll, Q/E yaw rudder");
		ImGui::TextDisabled("Shift throttle up, Ctrl throttle down, Space level out");
		ImGui::Checkbox("Chase camera", &airplaneChaseCam);
		ImGui::Checkbox("Invert pitch", &airplaneInvertPitch);
		ImGui::Text("Throttle: %.0f%%", airplaneThrottle * 100.0f);
		ImGui::SliderFloat("Max Speed", &airplaneMaxSpeed, 10.0f, 200.0f, "%.0f m/s");
		ImGui::SliderFloat("Acceleration", &airplaneAccel, 0.5f, 20.0f, "%.1f /s");
		ImGui::SliderFloat("Throttle Rate", &airplaneThrottleRate, 0.1f, 5.0f, "%.1f /s");
		ImGui::SliderFloat("Pitch Rate", &airplanePitchRate, 10.0f, 180.0f, "%.0f deg/s");
		ImGui::SliderFloat("Roll Rate", &airplaneRollRate, 10.0f, 270.0f, "%.0f deg/s");
		ImGui::SliderFloat("Yaw Rate", &airplaneYawRate, 10.0f, 120.0f, "%.0f deg/s");
		ImGui::SliderFloat("Gravity Sag", &airplaneGravitySag, 0.0f, 30.0f, "%.1f m/s");
		ImGui::SliderFloat("Stall Speed", &airplaneStallSpeed, 5.0f, 80.0f, "%.0f m/s");
		ImGui::SliderFloat("Level Strength", &airplaneLevelStrength, 0.0f, 10.0f);
		ImGui::SliderFloat("Cam Distance", &airplaneCamDistance, 2.0f, 40.0f);
		ImGui::SliderFloat("Cam Height", &airplaneCamHeight, 0.0f, 20.0f);
		if (airplaneBody_)
		{
			glm::vec3 v = airplaneBody_->GetLinearVelocity();
			ImGui::Text("Airspeed: %.1f m/s", airplaneAirSpeed);
			ImGui::Text("Velocity: %.2f, %.2f, %.2f", v.x, v.y, v.z);
		}
		ImGui::Separator();
		ImGui::Text("Terrain");
		ImGui::DragInt("Render Distance", &chunkLoadDistance);
		ImGui::DragInt("Physics Distance", &physicsDistance, 0.5f, 0, 20);
		ImGui::SliderInt("Terrain Pre-gen", &chunkOutgenDistance, 1, 10);
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

	std::unique_ptr<ChunkManager> chunkManager = std::make_unique<ChunkManager>(4);
#ifdef FE_HAS_VULKAN
	std::unique_ptr<ChunkBatcher> chunkBatcher_;
#endif
	glm::ivec2 playerCenter_{0, 0};
	int chunkLoadDistance = 6;
	int chunkOutgenDistance = 1;
	int physicsDistance = 2;

	std::shared_ptr<fe::Object> droneObject_;
	fe::PhysicsObject* droneBody_ = nullptr;
};
