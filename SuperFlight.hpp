#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <memory>
#include <string>
#include <vector>
#include <limits>
#include <algorithm>

#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <EditableGame.hpp>
#include <Object.hpp>
#include <Character.hpp>
#include <Primitives.hpp>
#include <physics/PhysicsVehicle.hpp>

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

		SetupWorld();

		window->StartMouseCapture();
		RefreshJoysticks();
	}

	void Step() override
	{
		ProcessInput();

		const float dt = static_cast<float>(fpsCounter.deltaTime);

		switch (mode)
		{
		case Mode::Walking:  WalkingUpdate(GetWindow<fe::SDLWindow>(), dt); break;
		case Mode::Drone:    if (droneBody_)    DroneUpdate(GetWindow<fe::SDLWindow>(), dt); break;
		case Mode::Airplane: if (airplaneBody_) AirplaneUpdate(GetWindow<fe::SDLWindow>(), dt); break;
		}

		UpdateLoadedChunks();

		{
			glm::vec2 forward2D = glm::normalize(glm::vec2(camera->front.x, camera->front.z));
			chunkManager->UpdatePausedState(playerCenter_, forward2D);
		}

		chunkManager->Update(1, GetPhysicsFactory(), scene.get(),
							 playerCenter_, chunkLoadDistance, physicsDistance);

		Update();

		if (smokeBox_ && smokeFrame_ < 260)
		{
			std::cout << "[SMOKE] frame " << smokeFrame_ << " y=" << smokeBox_->GetPosition().y << " vy=" << smokeBox_->GetLinearVelocity().y << std::endl;
			++smokeFrame_;
		}

		switch (mode)
		{
		case Mode::Walking:  RespawnIfFell(); SyncCameraToPlayer(); break;
		case Mode::Drone:    if (droneBody_)    camera->SetPos(droneBody_->GetPosition()); break;
		case Mode::Airplane: if (airplaneBody_) AirplaneCameraUpdate(); break;
		}

		Redraw();
	}

	// ---------------------------------------------------------------- world

	void SetupWorld()
	{
		Chunk::landingStrip.enabled = true;
		Chunk::landingStrip.minX = -48.0f;
		Chunk::landingStrip.maxX = 48.0f;
		Chunk::landingStrip.minZ = -12.0f;
		Chunk::landingStrip.maxZ = 12.0f;
		Chunk::landingStrip.height = 30;

		if (auto *physics = GetPhysicsFactory())
			physics->SetGravity(glm::vec3(0.0f, -20.0f, 0.0f));

		CreatePlayer();
		CreateParkedVehicles();

		if (auto *physics = GetPhysicsFactory())
		{
			smokeBox_ = physics->CreateObject(glm::vec3(1.0f, 1.0f, 1.0f), true, true);
			if (smokeBox_)
			{
				smokeBox_->SetPosition(glm::vec3(0.0f, 45.0f, 0.0f));
				smokeBox_->SetDamping(0.0f, 0.0f);
				smokeBox_->SetLinearVelocity(glm::vec3(0.0f));
			}
		}
	}

	void RebuildPlayerPhysicsBody()
	{
		auto *physicsFactory = GetPhysicsFactory();
		if (!player || !physicsFactory)
			return;

		const glm::vec3 size(0.6f, 1.875f, 0.6f);
		float radius = std::min(size.x, size.z) * 0.5f;

		auto physicsCharacter = physicsFactory->CreateCharacter(size.y, radius, player->state.position, false);
		if (!physicsCharacter)
			return;

		player->SetPhysicsCharacter(std::move(physicsCharacter));
		if (player->physicsCharacter)
		{
			player->physicsCharacter->SetPosition(player->state.position);
			player->physicsCharacter->SetJumpSpeed(player->jumpSpeed);
		}
	}

	void CreatePlayer()
	{
		player = std::make_shared<fe::Character>();
		scene->AddObject(player);
		player->state.position = playerSpawn;
		player->gravityEnabled = true;
		player->jumpSpeed = 7.07f; // ~1.25 blocks at gravity -20
		player->moveSpeed = walkSpeed;
		RebuildPlayerPhysicsBody();
		UpdateLoadedChunks();
		SyncCameraToPlayer();
	}

	void CreateParkedVehicles()
	{
		// Parked drone: a plain cube, static until the player climbs in.
		droneObject_ = std::make_shared<fe::Object>(fe::Primitives::GenerateCube(
			{fe::PlaneDirection::Front, fe::PlaneDirection::Back,
			 fe::PlaneDirection::Left, fe::PlaneDirection::Right,
			 fe::PlaneDirection::Top, fe::PlaneDirection::Bottom},
			0.6f));
		droneObject_->name = "Drone";
		droneObject_->color = glm::vec3(1.0f, 0.45f, 0.0f);
		droneObject_->state.position = droneSpawn;
		droneObject_->isStatic = true;
		scene->AddObject(droneObject_);

		// Parked airplane: same mesh used when flying, just inert for now.
		airplaneObject_ = std::make_shared<fe::Object>(BuildAirplaneMesh());
		airplaneObject_->name = "Airplane";
		airplaneObject_->color = glm::vec3(0.85f, 0.20f, 0.20f);
		airplaneObject_->state.position = airplaneSpawn;
		airplaneObject_->isStatic = true;
		scene->AddObject(airplaneObject_);

		auto gear = std::make_unique<fe::Object>(BuildAirplaneGearMesh());
		gear->name = "AirplaneGear";
		gear->color = glm::vec3(0.15f, 0.15f, 0.17f);
		gear->visible = airplaneGearDown;
		airplaneGearObject_ = airplaneObject_->AddChild(std::move(gear));
	}

	// ------------------------------------------------------------ vehicle I/O

	void EnterDrone()
	{
		if (mode != Mode::Walking)
			ExitVehicle();
		auto *physics = GetPhysicsFactory();
		if (!physics || !droneObject_)
			return;

		if (!droneBody_)
		{
			droneObject_->SetPhysicsObject(physics->CreateSphereObject(0.3f, true));
			droneBody_ = droneObject_->physicsObject.get();
			if (droneBody_)
			{
				droneBody_->SetPosition(droneObject_->state.position);
				droneBody_->SetRotation(droneObject_->state.orientation);
				droneBody_->SetLinearVelocity(glm::vec3(0.0f));
				droneBody_->SetAngularVelocity(glm::vec3(0.0f));
				droneBody_->SetDamping(droneLinearDamping, droneAngularDamping);
			}
		}

		droneObject_->isStatic = false;
		mode = Mode::Drone;
		droneThrottle = 0.0f;
		GetWindow<fe::SDLWindow>()->StartMouseCapture();
	}

	void EnterAirplane()
	{
		if (mode != Mode::Walking)
			ExitVehicle();
		auto *physics = GetPhysicsFactory();
		if (!physics || !airplaneObject_)
			return;

		if (!airplaneBody_)
		{
			airplaneObject_->SetPhysicsObject(physics->CreateObject(glm::vec3(3.6f, 1.4f, 3.2f), true, true));
			airplaneBody_ = airplaneObject_->physicsObject.get();
			if (airplaneBody_)
			{
				airplaneBody_->SetPosition(airplaneObject_->state.position);
				airplaneBody_->SetRotation(airplaneObject_->state.orientation);
				airplaneBody_->SetDamping(0.0f, 0.0f);
				airplaneBody_->SetAngularVelocity(glm::vec3(0.0f));
				airplaneBody_->SetLinearVelocity(glm::vec3(0.0f));
			}
		}

		airplaneObject_->isStatic = false;
		mode = Mode::Airplane;
		airplaneThrottle = 0.0f;
		airplaneAirSpeed = 0.0f;
		airplaneGearDown = false;   // force SetAirplaneGear to actually apply
		SetAirplaneGear(true);      // start on the wheels, ready to taxi
		GetWindow<fe::SDLWindow>()->StartMouseCapture();
	}

	void ExitVehicle()
	{
		fe::Object *vehicle = nullptr;

		if (mode == Mode::Drone && droneObject_)
		{
			vehicle = droneObject_.get();
			droneObject_->SetPhysicsObject(nullptr); // destructor removes the Jolt body
			droneBody_ = nullptr;
		}
		else if (mode == Mode::Airplane && airplaneObject_)
		{
			vehicle = airplaneObject_.get();
			DestroyAirplaneVehicle();
			airplaneObject_->SetPhysicsObject(nullptr);
			airplaneBody_ = nullptr;
			airplaneGearDown = true; // parked plane shows its gear again
			if (airplaneGearObject_)
				airplaneGearObject_->visible = true;
		}

		if (vehicle)
			vehicle->isStatic = true;

		glm::vec3 vp = vehicle ? vehicle->state.position : playerSpawn;
		int surface = chunkManager->GetSurfaceHeight(static_cast<int>(std::floor(vp.x)),
													 static_cast<int>(std::floor(vp.z)));
		float groundY = surface > 0 ? static_cast<float>(surface) : vp.y;
		glm::vec3 place(vp.x + 3.0f, groundY + 1.2f, vp.z);

		if (player)
		{
			player->state.position = place;
			if (player->physicsCharacter)
			{
				player->physicsCharacter->SetPosition(place);
				player->physicsCharacter->SetLinearVelocity(glm::vec3(0.0f));
			}
		}

		mode = Mode::Walking;
		SyncCameraToPlayer();
	}

	// ------------------------------------------------------------- movement

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

	void WalkingUpdate(fe::SDLWindow *window, float dt)
	{
		if (!player)
			return;

		player->moveSpeed = window->IsKeyDown(SDL_SCANCODE_LCTRL) ? sprintSpeed : walkSpeed;

		if (window->IsKeyDown(SDL_SCANCODE_W)) player->Move(fe::Direction::Forwards, camera.get());
		if (window->IsKeyDown(SDL_SCANCODE_S)) player->Move(fe::Direction::Backwards, camera.get());
		if (window->IsKeyDown(SDL_SCANCODE_A)) player->Move(fe::Direction::Left, camera.get());
		if (window->IsKeyDown(SDL_SCANCODE_D)) player->Move(fe::Direction::Right, camera.get());
		if (window->IsKeyDown(SDL_SCANCODE_SPACE)) player->Move(fe::Direction::Up, camera.get());
		if (window->IsKeyDown(SDL_SCANCODE_LSHIFT)) player->Move(fe::Direction::Down, camera.get());
	}

	void SyncCameraToPlayer()
	{
		if (!player)
			return;
		const float characterHeight = player->physicsCharacter ? player->physicsCharacter->GetHeight() : 1.5f;
		camera->SetPos(player->state.position + glm::vec3(0.0f, 0.4f * characterHeight, 0.0f));
	}

	void RespawnIfFell()
	{
		if (!player || !player->physicsCharacter)
			return;
		glm::vec3 p = player->physicsCharacter->GetPosition();
		if (p.y >= -5.0f)
			return;

		int surface = chunkManager->GetSurfaceHeight(static_cast<int>(std::floor(p.x)),
													 static_cast<int>(std::floor(p.z)));
		if (surface <= 0)
			return;
		p.y = static_cast<float>(surface) + 1.2f;
		player->physicsCharacter->SetPosition(p);
		player->physicsCharacter->SetLinearVelocity(glm::vec3(0.0f));
		player->state.position = p;
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

	void AirplaneDriveUpdate(fe::SDLWindow *window)
	{
		float forward = 0.0f, steer = 0.0f, brake = 0.0f, handbrake = 0.0f;
		if (window->IsKeyDown(SDL_SCANCODE_W)) forward += 1.0f;
		if (window->IsKeyDown(SDL_SCANCODE_S)) forward -= 0.5f; // reverse
		if (window->IsKeyDown(SDL_SCANCODE_D)) steer += 1.0f;
		if (window->IsKeyDown(SDL_SCANCODE_A)) steer -= 1.0f;
		if (window->IsKeyDown(SDL_SCANCODE_SPACE)) handbrake = 1.0f;
		if (window->IsKeyDown(SDL_SCANCODE_LSHIFT)) brake = 1.0f;

		airplaneVehicle_->SetDriverInput(forward, steer, brake, handbrake);
		airplaneThrottle = forward > 0.0f ? forward : 0.0f;
		airplaneAirSpeed = glm::length(airplaneBody_->GetLinearVelocity());
	}

	void AirplaneUpdate(fe::SDLWindow *window, float dt)
	{
		// On the wheels the Jolt vehicle controller drives the plane; retract the
		// gear (G) to hand control over to the aerodynamic flight model below.
		if (airplaneGearDown && airplaneVehicle_)
		{
			AirplaneDriveUpdate(window);
			return;
		}

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
		glm::vec3 fwd   = rot * glm::vec3(0.0f, 0.0f, 1.0f);

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
		glm::vec3 fwd = rot * glm::vec3(0.0f, 0.0f, 1.0f);
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

	// --------------------------------------------------------------- picking

	void TryEnterVehicleAtScreenCenter()
	{
		auto *window = GetWindow<fe::SDLWindow>();
		if (!window)
			return;

		const int w = window->width;
		const int h = window->height;
		glm::vec3 origin, dir;
		BuildScreenRay(static_cast<float>(w) * 0.5f, static_cast<float>(h) * 0.5f, w, h, origin, dir);

		fe::Object *best = nullptr;
		float bestT = std::numeric_limits<float>::max();

		fe::Object *candidates[2] = {droneObject_.get(), airplaneObject_.get()};
		for (fe::Object *obj : candidates)
		{
			if (!obj)
				continue;

			glm::vec3 localCenter;
			float localRadius;
			GetObjectBoundingSphere(obj, localCenter, localRadius);

			glm::vec3 worldCenter = glm::vec3(obj->GetModelMatrix() * glm::vec4(localCenter, 1.0f));
			float t = RaySphere(origin, dir, worldCenter, localRadius);
			if (t >= 0.0f && t < bestT)
			{
				bestT = t;
				best = obj;
			}
		}

		if (best == droneObject_.get())
			EnterDrone();
		else if (best == airplaneObject_.get())
			EnterAirplane();
	}

	void BuildScreenRay(float screenX, float screenY, int w, int h, glm::vec3 &outOrigin, glm::vec3 &outDir)
	{
		glm::vec4 viewport(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
		glm::mat4 view = camera->GetViewMatrix();
		glm::mat4 proj = camera->GetProjectionMatrix();

		glm::vec3 nearP = glm::unProject(glm::vec3(screenX, static_cast<float>(h) - screenY, 0.0f), view, proj, viewport);
		glm::vec3 farP  = glm::unProject(glm::vec3(screenX, static_cast<float>(h) - screenY, 1.0f), view, proj, viewport);

		outOrigin = nearP;
		outDir = glm::normalize(farP - nearP);
	}

	static void GetObjectBoundingSphere(fe::Object *obj, glm::vec3 &center, float &radius)
	{
		glm::vec3 mn(std::numeric_limits<float>::max());
		glm::vec3 mx(-std::numeric_limits<float>::max());

		std::vector<glm::vec3> positions;
		for (const auto &mesh : obj->meshes)
		{
			positions.clear();
			mesh->GetPositions(positions);
			for (const auto &p : positions)
			{
				mn = glm::min(mn, p);
				mx = glm::max(mx, p);
			}
		}

		if (mn.x > mx.x)
		{
			center = glm::vec3(0.0f);
			radius = 1.0f;
			return;
		}

		center = (mn + mx) * 0.5f;
		radius = glm::length((mx - mn) * 0.5f);
	}

	static float RaySphere(const glm::vec3 &origin, const glm::vec3 &dir, const glm::vec3 &center, float radius)
	{
		glm::vec3 oc = origin - center;
		float b = glm::dot(oc, dir);
		float c = glm::dot(oc, oc) - radius * radius;
		float disc = b * b - c;
		if (disc < 0.0f)
			return -1.0f;

		float s = std::sqrt(disc);
		float t = -b - s;
		if (t < 0.0f)
			t = -b + s;
		return t >= 0.0f ? t : -1.0f;
	}

	// ---------------------------------------------------------------- input

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
					if (!window->IsCapturingMouse())
					{
						window->StartMouseCapture();
						RefreshJoysticks();
					}
					else if (mode == Mode::Walking)
					{
						TryEnterVehicleAtScreenCenter();
					}
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
				else if (event.key.key == SDLK_F)
				{
					if (mode != Mode::Walking)
						ExitVehicle();
				}
				else if (event.key.key == SDLK_G)
				{
					if (mode == Mode::Airplane)
						SetAirplaneGear(!airplaneGearDown);
				}
				break;
			}
		}

		if (window->IsKeyDown(SDL_SCANCODE_ESCAPE))
			window->StopMouseCapture();
		if (ImGui::GetIO().WantCaptureMouse)
			window->StopMouseCapture();
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

		// Local forward is +Z (matches Jolt's default wheel forward axis so the
		// plane drives toward its nose). Tail surfaces sit on -Z.
		addBox({0.0f, 0.0f, 0.0f},     {0.25f, 0.25f, 1.6f});  // fuselage
		addBox({0.0f, 0.0f, 0.2f},     {1.8f, 0.06f, 0.4f});   // main wings
		addBox({0.0f, 0.0f, -1.4f},    {0.6f, 0.05f, 0.25f});  // horizontal stabilizer
		addBox({0.0f, 0.35f, -1.4f},   {0.05f, 0.35f, 0.3f});  // vertical fin

		return fe::Mesh<fe::Vertex>(vertices, indices);
	}

	// Tricycle landing gear drawn in body-local space (nose at +Z). Kept as a
	// separate object so it can be hidden when the gear is retracted.
	static fe::Mesh<fe::Vertex> BuildAirplaneGearMesh()
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

		addBox({0.0f, -0.45f, 1.1f},  {0.06f, 0.30f, 0.06f}); // nose strut
		addBox({0.0f, -0.72f, 1.1f},  {0.10f, 0.10f, 0.22f}); // nose wheel
		addBox({-0.6f, -0.45f, -0.3f}, {0.06f, 0.30f, 0.06f}); // left strut
		addBox({-0.6f, -0.72f, -0.3f}, {0.10f, 0.10f, 0.22f}); // left wheel
		addBox({0.6f, -0.45f, -0.3f},  {0.06f, 0.30f, 0.06f}); // right strut
		addBox({0.6f, -0.72f, -0.3f},  {0.10f, 0.10f, 0.22f}); // right wheel

		return fe::Mesh<fe::Vertex>(vertices, indices);
	}

	// Wheels line up with the landing gear above. Nose wheel steers, the two
	// rear wheels drive (first two driven wheels form the differential).
	static std::vector<fe::PhysicsVehicle::WheelConfig> BuildAirplaneWheels()
	{
		std::vector<fe::PhysicsVehicle::WheelConfig> wheels;
		auto wheel = [](glm::vec3 pos, float radius, bool steer, bool driven)
		{
			fe::PhysicsVehicle::WheelConfig w;
			w.position = pos;
			w.radius = radius;
			w.width = 0.2f;
			w.suspensionMaxLength = 0.3f;
			w.suspensionFrequency = 1.5f;
			w.suspensionDamping = 0.5f;
			w.isSteering = steer;
			w.isDriven = driven;
			return w;
		};
		wheels.push_back(wheel({0.0f, -0.45f, 1.1f}, 0.22f, true, false));   // nose
		wheels.push_back(wheel({-0.6f, -0.45f, -0.3f}, 0.28f, false, true));  // left
		wheels.push_back(wheel({0.6f, -0.45f, -0.3f}, 0.28f, false, true));   // right
		return wheels;
	}

	void CreateAirplaneVehicle()
	{
		auto *physics = GetPhysicsFactory();
		if (!physics || !airplaneBody_)
			return;
		if (airplaneVehicle_)
			return;
		airplaneVehicle_ = physics->CreateVehicle(airplaneBody_, BuildAirplaneWheels());
		if (airplaneVehicle_)
			airplaneVehicle_->SetMaxPitchRollAngle(45.0f);
	}

	void DestroyAirplaneVehicle()
	{
		if (!airplaneVehicle_)
			return;
		if (auto *physics = GetPhysicsFactory())
			physics->DestroyVehicle(airplaneVehicle_);
		airplaneVehicle_ = nullptr;
	}

	void SetAirplaneGear(bool down)
	{
		if (down == airplaneGearDown)
			return;
		airplaneGearDown = down;
		if (airplaneGearObject_)
			airplaneGearObject_->visible = down;

		if (down)
		{
			CreateAirplaneVehicle();
		}
		else
		{
			// Switching from taxiing to flight: seed the airspeed from whatever
			// ground speed we have so the plane does not stop dead.
			if (airplaneBody_)
				airplaneAirSpeed = glm::length(airplaneBody_->GetLinearVelocity());
			DestroyAirplaneVehicle();
		}
	}

	void InitUI() override {}

	void DrawUI() override
	{
		if (!showDebugUI) return;
		BeginFrame();

		ImGui::Begin("SuperFlight");
		ImGui::Text("Mode: %s", mode == Mode::Walking ? "Walking" : (mode == Mode::Drone ? "Drone" : "Airplane"));
		if (mode != Mode::Walking && ImGui::Button("Exit vehicle (F)"))
			ExitVehicle();
		if (mode == Mode::Walking)
			ImGui::TextDisabled("WASD move, Space jump, Shift down, Ctrl sprint");
		ImGui::TextDisabled("Left-click the plane/drone to fly it. F to exit.");
		ImGui::Separator();

		if (ImGui::CollapsingHeader("Drone", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Checkbox("Altitude hold", &droneAltitudeHold);
			ImGui::SliderFloat("Thrust", &droneThrust, 0.0f, 80.0f, "%.1f m/s2");
			ImGui::SliderFloat("Lift", &droneLift, 0.0f, 60.0f, "%.1f m/s2");
			ImGui::SliderFloat("Yaw Speed", &droneYawSpeed, 10.0f, 360.0f, "%.0f deg/s");
			ImGui::SliderFloat("Pitch Speed", &dronePitchSpeed, 10.0f, 360.0f, "%.0f deg/s");
			ImGui::SliderFloat("Linear Damping", &droneLinearDamping, 0.0f, 5.0f);
			ImGui::SliderFloat("Angular Damping", &droneAngularDamping, 0.0f, 10.0f);
			ImGui::TextDisabled("W/S forward-back (level), A/D strafe, Space up, Shift down");
			ImGui::TextDisabled("Arrows: yaw/pitch. Mouse: FPV look.");
			if (droneBody_)
			{
				glm::vec3 v = droneBody_->GetLinearVelocity();
				ImGui::Text("Speed: %.1f m/s", glm::length(v));
			}
		}

		if (ImGui::CollapsingHeader("Airplane", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Text("Gear: %s  (%s)", airplaneGearDown ? "DOWN" : "UP",
				airplaneGearDown ? "taxi/vehicle mode" : "flight mode");
			if (ImGui::Button(airplaneGearDown ? "Retract gear (G)" : "Extend gear (G)"))
				SetAirplaneGear(!airplaneGearDown);
			if (airplaneGearDown)
				ImGui::TextDisabled("W throttle, S reverse, A/D steer, Shift brake, Space handbrake");
			else
			{
				ImGui::TextDisabled("W/S pitch, A/D roll, Q/E yaw rudder");
				ImGui::TextDisabled("Shift throttle up, Ctrl throttle down, Space level out");
			}
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
		}

		if (ImGui::CollapsingHeader("Terrain"))
		{
			ImGui::DragInt("Render Distance", &chunkLoadDistance);
			ImGui::DragInt("Physics Distance", &physicsDistance, 0.5f, 0, 20);
			ImGui::SliderInt("Terrain Pre-gen", &chunkOutgenDistance, 1, 10);
		}
		ImGui::End();

		DrawDebugUI();
		EndFrame();
	}

private:
	enum class Mode { Walking, Drone, Airplane };
	Mode mode = Mode::Walking;

	bool showDebugUI = false;

	const glm::vec3 playerSpawn{0.0f, 33.0f, 10.0f};
	const glm::vec3 droneSpawn{6.0f, 30.6f, 0.0f};
	const glm::vec3 airplaneSpawn{-6.0f, 31.2f, 0.0f};

	float walkSpeed = 25.0f;
	float sprintSpeed = 40.0f;

	float droneThrust = 30.0f;
	float droneLift = 18.0f;
	float droneYawSpeed = 120.0f;
	float dronePitchSpeed = 90.0f;
	float dronePitchLimit = 60.0f;
	float droneLinearDamping = 1.2f;
	float droneAngularDamping = 4.0f;
	float droneGravity = 20.0f; // counters the world gravity used for the strip
	float droneThrottle = 0.0f;
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
	fe::PhysicsObject *droneBody_ = nullptr;

	float airplaneThrottle = 0.0f;
	float airplaneAirSpeed = 0.0f;
	float airplaneMaxSpeed = 90.0f;
	float airplaneAccel = 6.0f;
	float airplaneThrottleRate = 1.5f;
	float airplanePitchRate = 60.0f;
	float airplaneRollRate = 120.0f;
	float airplaneYawRate = 45.0f;
	float airplaneGravitySag = 8.0f;
	float airplaneStallSpeed = 25.0f;
	float airplaneLevelStrength = 4.0f;
	float airplaneCamDistance = 12.0f;
	float airplaneCamHeight = 4.0f;
	glm::vec2 airplaneCockpitOffset{0.6f, 0.4f};
	bool airplaneInvertPitch = false;
	bool airplaneChaseCam = true;

	std::shared_ptr<fe::Object> airplaneObject_;
	fe::PhysicsObject *airplaneBody_ = nullptr;
	fe::Object *airplaneGearObject_ = nullptr;
	fe::PhysicsVehicle *airplaneVehicle_ = nullptr;
	bool airplaneGearDown = true;

	std::unique_ptr<fe::PhysicsObject> smokeBox_;
	int smokeFrame_ = 0;
};
