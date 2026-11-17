#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <EditableGame.hpp>
#include <Object.hpp>
#include <Character.hpp>
#include <Primitives.hpp>
#include <components/Drone.hpp>
#include <physics/PhysicsVehicle.hpp>
#include <vr/OpenXR.hpp>

#include "ChunkManager.hpp"
#ifdef FE_HAS_VULKAN
#include "ChunkMesher.hpp"
#include "ChunkBatcher.hpp"
#endif

class SuperFlight : public fe::EditableGame
{
public:
	explicit SuperFlight(fe::XRGameOptions options) : fe::EditableGame(options)
	{
		SetClearColor(0.05f, 0.07f, 0.12f);
#if defined(__ANDROID__)
		LoadShaders("resources/shaders/VertexShader_gles300.glsl",
			"resources/shaders/FragmentShader_gles300.glsl");
#else
		if (!options.useVulkan)
			LoadShaders("resources/shaders/VertexShader_gl330.glsl",
				"resources/shaders/FragmentShader_gl330.glsl");
#endif
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
		auto *window = GetWindow<fe::SDLWindow>();
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
		auto *window = GetWindow<fe::SDLWindow>();

		switch (mode)
		{
		case Mode::Walking: WalkingUpdate(window, dt); break;
		case Mode::Drone: if (drone_ && drone_->IsActive()) DroneUpdate(window, dt); break;
		case Mode::Airplane: if (plane_.body) AirplaneUpdate(window, dt); break;
		}

		if (window->IsKeyDown(SDL_SCANCODE_B)) CreateBall();

		UpdateLoadedChunks();
		chunkManager->UpdatePausedState(playerCenter_, glm::normalize(glm::vec2(camera->front.x, camera->front.z)));
		chunkManager->Update(1, GetPhysicsFactory(), scene.get(), playerCenter_, chunkLoadDistance, physicsDistance);
		Update();

		HandleXRInputs();
		UpdateControllerCubes(dt);

		switch (mode)
		{
		case Mode::Walking: RespawnIfFell(); SyncCameraToPlayer(); break;
		case Mode::Drone: if (drone_ && drone_->IsActive()) camera->SetPos(drone_->GetBody()->GetPosition()); break;
		case Mode::Airplane: if (plane_.body) AirplaneCameraUpdate(); break;
		}
		Redraw();
	}

	void SetupWorld()
	{
		Chunk::landingStrip.enabled = true;
		Chunk::landingStrip.minX = -48.0f;
		Chunk::landingStrip.maxX = 48.0f;
		Chunk::landingStrip.minZ = -64.0f;
		Chunk::landingStrip.maxZ = 16.0f;
		Chunk::landingStrip.height = 30;

		if (auto *physics = GetPhysicsFactory())
			physics->SetGravity(glm::vec3(0.0f, -20.0f, 0.0f));

		CreatePlayer();
		CreateParkedVehicles();
		CreateControllerCubes();
		CreateBall();
		CreateThrowingLane();
	}

	std::shared_ptr<fe::Object> AddCube(const char *name, glm::vec3 color, float size, glm::vec3 position)
	{
		auto object = std::make_shared<fe::Object>(fe::Primitives::GenerateCube(size));
		object->name = name;
		object->color = color;
		object->state.position = position;
		object->isStatic = true;
		scene->AddObject(object);
		return object;
	}

	std::shared_ptr<fe::Object> AddSphere(const char *name, glm::vec3 color, glm::vec3 position)
	{
		auto object = std::make_shared<fe::Object>(fe::Primitives::GenerateSphere(ballRadius_, 24, 16));
		object->name = name;
		object->color = color;
		object->state.position = position;
		object->isStatic = false;
		if (auto *physics = GetPhysicsFactory())
		{
			auto body = physics->CreateSphereObject(ballRadius_, true);
			if (body)
			{
				body->SetPosition(position);
				body->SetLinearVelocity(glm::vec3(0.0f));
				body->SetAngularVelocity(glm::vec3(0.0f));
			}
			object->SetPhysicsObject(std::move(body));
		}
		scene->AddObject(object);
		return object;
	}

	void CreateThrowingLane()
	{
		const glm::vec3 base(0.0f, 0.0f, -24.0f);
		struct Part { const char *name; glm::vec3 color; glm::vec3 size; glm::vec3 offset; };
		const Part parts[] = {
			{"ThrowingLaneFloor", {0.38f, 0.40f, 0.43f}, {15.0f, 0.2f, 80.0f}, {0.0f, 30.1f, 0.10f}},
			{"ThrowingLaneLeftWall", {0.26f, 0.30f, 0.34f}, {0.2f, 0.8f, 80.0f}, {-7.4f, 30.4f, 0.0f}},
			{"ThrowingLaneRightWall", {0.26f, 0.30f, 0.34f}, {0.2f, 0.8f, 80.0f}, {7.4f, 30.4f, 0.0f}},
		};
		for (const auto &part : parts)
		{
			auto object = AddCube(part.name, part.color, 1.0f, base + part.offset);
			object->state.scale = part.size;
		}
	}

	void CreateControllerCubes()
	{
		const auto input = GetWorldInputState();
		const char *names[2] = {"LeftController", "RightController"};
		const glm::vec3 colors[2] = {{0.2f, 0.8f, 1.0f}, {1.0f, 0.5f, 0.1f}};
		const glm::vec3 offsets[2] = {{-0.25f, 1.2f, -0.3f}, {0.25f, 1.2f, -0.3f}};
		const bool active[2] = {input.leftGripPoseActive, input.rightGripPoseActive};
		for (size_t i = 0; i < 2; ++i)
		{
			hands_[i].cube = AddCube(names[i], colors[i], 0.08f, playerSpawn + offsets[i]);
			hands_[i].cube->visible = active[i];
		}
	}

	void CreateHandJointCubes()
	{
		// constexpr size_t count = static_cast<size_t>(fe::OpenXR::HandJoint::Count);
		// handJointCubes_.reserve(count * 2);
		// const glm::vec3 colors[2] = {{0.2f, 0.8f, 1.0f}, {1.0f, 0.5f, 0.1f}};
		// const char *names[2] = {"Left", "Right"};
		// for (size_t hand = 0; hand < 2; ++hand)
		// 	for (size_t joint = 0; joint < count; ++joint)
		// 	{
		// 		auto cube = AddCube(
		// 			(std::string(names[hand]) + "HandJoint" + std::to_string(joint)).c_str(),
		// 			colors[hand], 0.012f, glm::vec3(0.0f));
		// 		cube->visible = false;
		// 		handJointCubes_.push_back(cube);
		// 	}
	}

	void UpdateHandJointCubes(const fe::OpenXR::InputState &input)
	{
		// if (handJointCubes_.empty() && GetOpenXR().IsHandTrackingSupported())
		// 	CreateHandJointCubes();
		// if (handJointCubes_.empty())
		// 	return;

		// constexpr size_t count = static_cast<size_t>(fe::OpenXR::HandJoint::Count);
		// const fe::OpenXR::HandState *hands[2] = {&input.leftHand, &input.rightHand};
		// for (size_t hand = 0; hand < 2; ++hand)
		// 	for (size_t joint = 0; joint < count; ++joint)
		// 	{
		// 		auto &cube = handJointCubes_[hand * count + joint];
		// 		const auto &pose = hands[hand]->joints[joint];
		// 		cube->visible = hands[hand]->active && pose.active;
		// 		if (cube->visible)
		// 		{
		// 			cube->SetPosition(pose.position);
		// 			cube->SetOrientation(pose.orientation);
		// 		}
		// 	}
	}

	void CreateBall()
	{
		ball_ = AddSphere("XRBall", {0.95f, 0.85f, 0.2f}, playerSpawn + glm::vec3(0.0f, 1.05f, -0.55f));
		grabbable_.push_back(ball_);
	}

	void HandleXRInputs()
	{
		if (GetOpenXR().ConsumeButtonPress(fe::OpenXR::Button::X))
			SpawnSphere();
	}

	void SpawnSphere()
	{
		glm::vec3 forward = camera->front;
		forward.y = 0.0f;
		if (glm::length(forward) < 1e-4f)
			forward = glm::vec3(0.0f, 0.0f, -1.0f);
		forward = glm::normalize(forward);

		const glm::vec3 base = player ? player->state.position : playerSpawn;
		const glm::vec3 position = base + glm::vec3(0.0f, 1.5f, 0.0f) + forward * 0.8f;
		auto sphere = AddSphere("XRSphere", {0.3f, 0.85f, 0.45f}, position);
		grabbable_.push_back(sphere);
		GetOpenXR().TriggerHaptic(fe::OpenXR::Hand::Left, 0.5f, 0.08f);
	}

	static fe::OpenXR::Hand XRHand(size_t index)
	{
		return index == 0 ? fe::OpenXR::Hand::Left : fe::OpenXR::Hand::Right;
	}

	void UpdateControllerCubes(float dt)
	{
		const auto input = GetWorldInputState();
		UpdateHandJointCubes(input);

		const glm::vec3 gripPos[2] = {input.leftGripPosition, input.rightGripPosition};
		const glm::quat gripRot[2] = {input.leftGripOrientation, input.rightGripOrientation};
		const bool poseActive[2] = {input.leftGripPoseActive, input.rightGripPoseActive};
		auto &xr = GetOpenXR();
		const bool gripDown[2] = {
			xr.IsGripPressed(fe::OpenXR::Hand::Left), xr.IsGripPressed(fe::OpenXR::Hand::Right)};
		const float cubeExtent = 0.08f;

		auto findTouched = [&](const glm::vec3 &pos, float scale) {
			std::shared_ptr<fe::Object> best;
			float bestDist = std::numeric_limits<float>::max();
			for (const auto &object : grabbable_)
			{
				if (!object)
					continue;
				const float reach = cubeExtent * scale + ballRadius_ * object->state.scale.x;
				const float dist = glm::length(pos - object->state.position);
				if (dist <= reach && dist < bestDist)
				{
					best = object;
					bestDist = dist;
				}
			}
			return best;
		};

		for (size_t i = 0; i < 2; ++i)
		{
			auto &hand = hands_[i];
			if (!poseActive[i])
			{
				hand.hasPrev = false;
				hand.velocity = glm::vec3(0.0f);
				hand.angularVelocity = glm::vec3(0.0f);
			}
			else
			{
				if (hand.hasPrev && dt > 1e-5f)
				{
					hand.velocity = glm::mix(hand.velocity, (gripPos[i] - hand.prevPos) / dt, 0.75f);
					glm::quat delta = gripRot[i] * glm::inverse(hand.prevRot);
					if (delta.w < 0.0f)
						delta = glm::quat(-delta.w, -delta.x, -delta.y, -delta.z);
					const float angle = 2.0f * std::acos(std::clamp(delta.w, -1.0f, 1.0f));
					const glm::vec3 axis(delta.x, delta.y, delta.z);
					const float axisLength = glm::length(axis);
					hand.angularVelocity = (axisLength > 1e-6f && angle > 1e-6f)
						? glm::mix(hand.angularVelocity, axis / axisLength * (angle / dt), 0.5f)
						: glm::vec3(0.0f);
				}
				hand.prevPos = gripPos[i];
				hand.prevRot = gripRot[i];
				hand.hasPrev = true;
			}

			if (!hand.cube)
				continue;
			hand.cube->visible = poseActive[i];
			if (!poseActive[i])
			{
				hand.touching = false;
				continue;
			}
			hand.cube->SetPosition(gripPos[i]);
			hand.cube->SetOrientation(gripRot[i]);

			const bool holding = grabbed_ && grabbedHand_ == static_cast<int>(i);
			const bool hit = holding || findTouched(gripPos[i], hand.scale) != nullptr;
			if (hit && !holding)
			{
				hand.scale = std::min(hand.scale + 0.2f * dt, 1.35f);
				if (!hand.touching)
					xr.TriggerHaptic(XRHand(i), 0.7f, 0.08f);
			}
			else if (!hit)
			{
				hand.scale = std::max(hand.scale - 0.5f * dt, 1.0f);
			}
			hand.touching = hit;
			hand.cube->SetScale(glm::vec3(hand.scale));
		}

		if (grabbed_)
		{
			const size_t i = static_cast<size_t>(grabbedHand_);
			if (!gripDown[i] || !poseActive[i])
				ReleaseGrabbedObject();
			else
			{
				const glm::vec3 position = gripPos[i] + gripRot[i] * grabOffset_;
				const glm::quat rotation = gripRot[i] * grabRotation_;
				grabbed_->SetPosition(position);
				grabbed_->SetOrientation(rotation);
				if (grabbed_->physicsObject)
				{
					grabbed_->physicsObject->SetPosition(position);
					grabbed_->physicsObject->SetRotation(rotation);
				}
			}
			return;
		}

		for (size_t i = 0; i < 2; ++i)
			if (gripDown[i] && poseActive[i])
				if (auto object = findTouched(gripPos[i], hands_[i].scale))
				{
					GrabObject(object, static_cast<int>(i), gripPos[i], gripRot[i]);
					break;
				}
	}

	void GrabObject(const std::shared_ptr<fe::Object> &object, int handIndex,
		const glm::vec3 &handPos, const glm::quat &handRot)
	{
		if (!object)
			return;
		const glm::quat inverse = glm::inverse(handRot);
		grabbed_ = object;
		grabbedHand_ = handIndex;
		grabOffset_ = inverse * (object->state.position - handPos);
		grabRotation_ = inverse * object->state.orientation;
		grabWasStatic_ = object->isStatic;
		object->isStatic = true;
		GetOpenXR().TriggerHaptic(XRHand(static_cast<size_t>(handIndex)), 0.8f, 0.1f);
	}

	void ReleaseGrabbedObject()
	{
		if (!grabbed_)
			return;
		if (grabbed_->physicsObject)
		{
			grabbed_->physicsObject->SetPosition(grabbed_->state.position);
			grabbed_->physicsObject->SetRotation(grabbed_->state.orientation);
			const size_t i = static_cast<size_t>(grabbedHand_);
			glm::vec3 throwVelocity = hands_[i].velocity * 2.5f;
			const float speed = glm::length(throwVelocity);
			if (speed > 45.0f)
				throwVelocity = throwVelocity / speed * 45.0f;
			glm::vec3 spin = hands_[i].angularVelocity;
			const float spinLength = glm::length(spin);
			if (spinLength > 30.0f)
				spin = spin / spinLength * 30.0f;
			grabbed_->physicsObject->SetLinearVelocity(throwVelocity);
			grabbed_->physicsObject->SetAngularVelocity(spin);
		}
		grabbed_->isStatic = grabWasStatic_;
		grabbed_.reset();
		grabbedHand_ = -1;
	}

	void CreatePlayer()
	{
		player = std::make_shared<fe::Character>();
		scene->AddObject(player);
		player->state.position = playerSpawn;
		player->gravityEnabled = true;
		player->jumpSpeed = 7.07f;
		player->moveSpeed = walkSpeed;
		if (auto *physics = GetPhysicsFactory())
		{
			if (auto body = physics->CreateCharacter(1.875f, 0.3f, player->state.position, false))
			{
				player->SetPhysicsCharacter(std::move(body));
				player->physicsCharacter->SetPosition(player->state.position);
				player->physicsCharacter->SetJumpSpeed(player->jumpSpeed);
			}
		}
		UpdateLoadedChunks();
		SyncCameraToPlayer();
	}

	void CreateParkedVehicles()
	{
		drone_ = std::make_unique<fe::Drone>();
		if (!drone_->Spawn(*scene, droneSpawn))
			throw std::runtime_error("Failed to spawn the SuperFlight demo drone");

		plane_.object = std::make_shared<fe::Object>(BuildAirplaneMesh());
		plane_.object->name = "Airplane";
		plane_.object->color = glm::vec3(0.85f, 0.20f, 0.20f);
		plane_.object->state.position = airplaneSpawn;
		plane_.object->isStatic = true;
		scene->AddObject(plane_.object);

		auto gear = std::make_unique<fe::Object>(BuildAirplaneGearMesh());
		gear->name = "AirplaneGear";
		gear->color = glm::vec3(0.15f, 0.15f, 0.17f);
		gear->visible = plane_.gearDown;
		plane_.gear = plane_.object->AddChild(std::move(gear));
	}

	void EnterDrone()
	{
		if (mode != Mode::Walking)
			ExitVehicle();
		auto *physics = GetPhysicsFactory();
		if (physics && drone_ && drone_->IsSpawned() && drone_->Enter(*physics))
		{
			mode = Mode::Drone;
			GetWindow<fe::SDLWindow>()->StartMouseCapture();
		}
	}

	void EnterAirplane()
	{
		if (mode != Mode::Walking)
			ExitVehicle();
		auto *physics = GetPhysicsFactory();
		if (!physics || !plane_.object)
			return;

		if (!plane_.body)
		{
			plane_.object->SetPhysicsObject(physics->CreateObject(glm::vec3(3.6f, 1.4f, 3.2f), true, true));
			plane_.body = plane_.object->physicsObject.get();
			if (plane_.body)
			{
				plane_.body->SetPosition(plane_.object->state.position);
				plane_.body->SetRotation(plane_.object->state.orientation);
				plane_.body->SetDamping(0.0f, 0.0f);
				plane_.body->SetAngularVelocity(glm::vec3(0.0f));
				plane_.body->SetLinearVelocity(glm::vec3(0.0f));
			}
		}
		plane_.object->isStatic = false;
		mode = Mode::Airplane;
		plane_.throttle = 0.0f;
		plane_.airSpeed = 0.0f;
		plane_.gearDown = false;
		SetAirplaneGear(true);
		GetWindow<fe::SDLWindow>()->StartMouseCapture();
	}

	void ExitVehicle()
	{
		fe::Object *vehicle = nullptr;
		if (mode == Mode::Drone && drone_ && drone_->IsSpawned())
		{
			vehicle = drone_->GetDroneObject().get();
			drone_->Exit();
		}
		else if (mode == Mode::Airplane && plane_.object)
		{
			vehicle = plane_.object.get();
			DestroyAirplaneVehicle();
			plane_.object->SetPhysicsObject(nullptr);
			plane_.body = nullptr;
			plane_.gearDown = true;
			if (plane_.gear)
				plane_.gear->visible = true;
		}
		if (vehicle)
			vehicle->isStatic = true;

		glm::vec3 position = vehicle ? vehicle->state.position : playerSpawn;
		const int surface = chunkManager->GetSurfaceHeight(
			static_cast<int>(std::floor(position.x)), static_cast<int>(std::floor(position.z)));
		const float groundY = surface > 0 ? static_cast<float>(surface) : position.y;
		const glm::vec3 place(position.x + 3.0f, groundY + 1.2f, position.z);
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

	void UpdateLoadedChunks()
	{
		const glm::vec3 position = camera->GetPos();
		playerCenter_ = {
			static_cast<int>(std::floor(position.x / static_cast<float>(Chunk::WIDTH))),
			static_cast<int>(std::floor(position.z / static_cast<float>(Chunk::DEPTH)))};
		const int distance = chunkLoadDistance + chunkOutgenDistance;
		chunkManager->LoadChunksInsideRange(playerCenter_, distance,
			glm::normalize(glm::vec2(camera->front.x, camera->front.z)));
		chunkManager->UnloadChunksOutsideRange(playerCenter_, distance);
	}

	void WalkingUpdate(fe::SDLWindow *window, float)
	{
		if (!player)
			return;
		player->moveSpeed = window->IsKeyDown(SDL_SCANCODE_LCTRL) ? sprintSpeed : walkSpeed;
		const std::pair<SDL_Scancode, fe::Direction> moves[] = {
			{SDL_SCANCODE_W, fe::Direction::Forwards}, {SDL_SCANCODE_S, fe::Direction::Backwards},
			{SDL_SCANCODE_A, fe::Direction::Left}, {SDL_SCANCODE_D, fe::Direction::Right},
			{SDL_SCANCODE_SPACE, fe::Direction::Up}, {SDL_SCANCODE_LSHIFT, fe::Direction::Down}};
		for (const auto &move : moves)
			if (window->IsKeyDown(move.first))
				player->Move(move.second, camera.get());
	}

	void SyncCameraToPlayer()
	{
		if (!player)
			return;
		const float height = player->physicsCharacter ? player->physicsCharacter->GetHeight() : 1.5f;
		camera->SetPos(player->state.position + glm::vec3(0.0f, 0.4f * height, 0.0f));
	}

	void RespawnIfFell()
	{
		if (!player || !player->physicsCharacter)
			return;
		glm::vec3 position = player->physicsCharacter->GetPosition();
		if (position.y >= -5.0f)
			return;
		const int surface = chunkManager->GetSurfaceHeight(
			static_cast<int>(std::floor(position.x)), static_cast<int>(std::floor(position.z)));
		if (surface <= 0)
			return;
		position.y = static_cast<float>(surface) + 1.2f;
		player->physicsCharacter->SetPosition(position);
		player->physicsCharacter->SetLinearVelocity(glm::vec3(0.0f));
		player->state.position = position;
	}

	void DroneUpdate(fe::SDLWindow *window, float dt)
	{
		if (window->IsKeyDown(SDL_SCANCODE_LEFT)) camera->yaw -= droneCameraYawSpeed * dt;
		if (window->IsKeyDown(SDL_SCANCODE_RIGHT)) camera->yaw += droneCameraYawSpeed * dt;
		if (window->IsKeyDown(SDL_SCANCODE_UP)) camera->pitch += droneCameraPitchSpeed * dt;
		if (window->IsKeyDown(SDL_SCANCODE_DOWN)) camera->pitch -= droneCameraPitchSpeed * dt;
		camera->pitch = std::clamp(camera->pitch, -droneCameraPitchLimit, droneCameraPitchLimit);
		camera->UpdateDirection();

		glm::vec2 horizontal(0.0f);
		if (window->IsKeyDown(SDL_SCANCODE_A)) horizontal.x -= 1.0f;
		if (window->IsKeyDown(SDL_SCANCODE_D)) horizontal.x += 1.0f;
		if (window->IsKeyDown(SDL_SCANCODE_S)) horizontal.y -= 1.0f;
		if (window->IsKeyDown(SDL_SCANCODE_W)) horizontal.y += 1.0f;
		float vertical = 0.0f;
		if (window->IsKeyDown(SDL_SCANCODE_SPACE)) vertical += 1.0f;
		if (window->IsKeyDown(SDL_SCANCODE_LSHIFT)) vertical -= 1.0f;
		drone_->Update(horizontal, vertical, glm::radians(camera->yaw));
	}

	void AirplaneDriveUpdate(fe::SDLWindow *window)
	{
		float forward = 0.0f, steer = 0.0f, brake = 0.0f, handbrake = 0.0f;
		if (window->IsKeyDown(SDL_SCANCODE_W)) forward += 1.0f;
		if (window->IsKeyDown(SDL_SCANCODE_S)) forward -= 0.5f;
		if (window->IsKeyDown(SDL_SCANCODE_D)) steer += 1.0f;
		if (window->IsKeyDown(SDL_SCANCODE_A)) steer -= 1.0f;
		if (window->IsKeyDown(SDL_SCANCODE_SPACE)) handbrake = 1.0f;
		if (window->IsKeyDown(SDL_SCANCODE_LSHIFT)) brake = 1.0f;
		plane_.vehicle->SetDriverInput(forward, steer, brake, handbrake);
		plane_.throttle = std::max(forward, 0.0f);
		plane_.airSpeed = glm::length(plane_.body->GetLinearVelocity());
	}

	void AirplaneUpdate(fe::SDLWindow *window, float dt)
	{
		if (plane_.gearDown && plane_.vehicle)
		{
			AirplaneDriveUpdate(window);
			return;
		}
		plane_.body->SetDamping(0.0f, 0.0f);

		float throttleInput = 0.0f;
		if (window->IsKeyDown(SDL_SCANCODE_LSHIFT)) throttleInput += 1.0f;
		if (window->IsKeyDown(SDL_SCANCODE_LCTRL)) throttleInput -= 1.0f;
		plane_.throttle = glm::clamp(plane_.throttle + throttleInput * plane_.throttleRate * dt, 0.0f, 1.0f);

		float pitch = 0.0f, roll = 0.0f, yaw = 0.0f;
		if (window->IsKeyDown(SDL_SCANCODE_W)) pitch += 1.0f;
		if (window->IsKeyDown(SDL_SCANCODE_S)) pitch -= 1.0f;
		if (window->IsKeyDown(SDL_SCANCODE_A)) roll += 1.0f;
		if (window->IsKeyDown(SDL_SCANCODE_D)) roll -= 1.0f;
		if (window->IsKeyDown(SDL_SCANCODE_Q)) yaw += 1.0f;
		if (window->IsKeyDown(SDL_SCANCODE_E)) yaw -= 1.0f;
		if (plane_.invertPitch) pitch = -pitch;

		const glm::quat rotation = plane_.body->GetRotation();
		const glm::vec3 right = rotation * glm::vec3(1.0f, 0.0f, 0.0f);
		const glm::vec3 up = rotation * glm::vec3(0.0f, 1.0f, 0.0f);
		const glm::vec3 forward = rotation * glm::vec3(0.0f, 0.0f, 1.0f);

		glm::vec3 omega = right * glm::radians(pitch * plane_.pitchRate)
			+ forward * glm::radians(roll * plane_.rollRate)
			+ up * glm::radians(yaw * plane_.yawRate);
		if (window->IsKeyDown(SDL_SCANCODE_SPACE))
		{
			const glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
			omega += glm::cross(up, worldUp) * plane_.levelStrength;
			const glm::vec3 flatForward(forward.x, 0.0f, forward.z);
			if (glm::length(flatForward) > 1e-4f)
				omega -= glm::cross(forward, glm::normalize(flatForward)) * plane_.levelStrength;
		}
		plane_.body->SetAngularVelocity(omega);

		const float target = plane_.throttle * plane_.maxSpeed;
		plane_.airSpeed += (target - plane_.airSpeed) * glm::clamp(plane_.accel * dt, 0.0f, 1.0f);
		const float lift = glm::clamp(plane_.airSpeed / plane_.stallSpeed, 0.0f, 1.0f);
		glm::vec3 velocity = forward * plane_.airSpeed;
		velocity.y -= plane_.gravitySag * (1.0f - lift);
		plane_.body->SetLinearVelocity(velocity);
	}

	void AirplaneCameraUpdate()
	{
		const glm::quat rotation = plane_.body->GetRotation();
		const glm::vec3 position = plane_.body->GetPosition();
		const glm::vec3 forward = rotation * glm::vec3(0.0f, 0.0f, 1.0f);
		const glm::vec3 up = rotation * glm::vec3(0.0f, 1.0f, 0.0f);

		glm::vec3 camPos;
		glm::vec3 direction;
		glm::vec3 camUp;
		if (plane_.chaseCam)
		{
			camPos = position - forward * plane_.camDistance + glm::vec3(0.0f, plane_.camHeight, 0.0f);
			direction = glm::normalize(position - camPos);
			camUp = glm::vec3(0.0f, 1.0f, 0.0f);
		}
		else
		{
			camPos = position + forward * plane_.cockpitOffset.x + up * plane_.cockpitOffset.y;
			direction = forward;
			camUp = up;
		}
		camera->front = direction;
		camera->up = camUp;
		camera->updateView(camPos, direction, camUp);
		camera->pitch = glm::degrees(glm::asin(glm::clamp(direction.y, -1.0f, 1.0f)));
		camera->yaw = glm::degrees(glm::atan(direction.z, direction.x));
	}

	void TryEnterVehicleAtScreenCenter()
	{
		auto *window = GetWindow<fe::SDLWindow>();
		if (!window)
			return;
		glm::vec3 origin, direction;
		BuildScreenRay(static_cast<float>(window->width) * 0.5f,
			static_cast<float>(window->height) * 0.5f, window->width, window->height, origin, direction);

		fe::Object *objects[2] = {
			drone_ ? drone_->GetDroneObject().get() : nullptr, plane_.object.get()};
		fe::Object *best = nullptr;
		float bestDistance = std::numeric_limits<float>::max();
		for (fe::Object *object : objects)
		{
			if (!object)
				continue;
			glm::vec3 center;
			float radius;
			GetObjectBoundingSphere(object, center, radius);
			const glm::vec3 worldCenter = glm::vec3(object->GetModelMatrix() * glm::vec4(center, 1.0f));
			const float distance = RaySphere(origin, direction, worldCenter, radius);
			if (distance >= 0.0f && distance < bestDistance)
			{
				bestDistance = distance;
				best = object;
			}
		}
		if (drone_ && best == drone_->GetDroneObject().get())
			EnterDrone();
		else if (best == plane_.object.get())
			EnterAirplane();
	}

	void BuildScreenRay(float x, float y, int width, int height, glm::vec3 &origin, glm::vec3 &direction)
	{
		const glm::vec4 viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
		const glm::mat4 view = camera->GetViewMatrix();
		const glm::mat4 projection = camera->GetProjectionMatrix();
		const float flippedY = static_cast<float>(height) - y;
		origin = glm::unProject(glm::vec3(x, flippedY, 0.0f), view, projection, viewport);
		const glm::vec3 farPoint = glm::unProject(glm::vec3(x, flippedY, 1.0f), view, projection, viewport);
		direction = glm::normalize(farPoint - origin);
	}

	static void GetObjectBoundingSphere(fe::Object *object, glm::vec3 &center, float &radius)
	{
		glm::vec3 minimum(std::numeric_limits<float>::max());
		glm::vec3 maximum(-std::numeric_limits<float>::max());
		std::vector<glm::vec3> positions;
		for (const auto &mesh : object->meshes)
		{
			mesh->GetPositions(positions);
			for (const auto &position : positions)
			{
				minimum = glm::min(minimum, position);
				maximum = glm::max(maximum, position);
			}
		}
		if (minimum.x > maximum.x)
		{
			center = glm::vec3(0.0f);
			radius = 1.0f;
			return;
		}
		center = (minimum + maximum) * 0.5f;
		radius = glm::length((maximum - minimum) * 0.5f);
	}

	static float RaySphere(const glm::vec3 &origin, const glm::vec3 &direction,
		const glm::vec3 &center, float radius)
	{
		const glm::vec3 offset = origin - center;
		const float b = glm::dot(offset, direction);
		const float c = glm::dot(offset, offset) - radius * radius;
		const float discriminant = b * b - c;
		if (discriminant < 0.0f)
			return -1.0f;
		const float root = std::sqrt(discriminant);
		const float nearHit = -b - root;
		if (nearHit >= 0.0f)
			return nearHit;
		const float farHit = -b + root;
		return farHit >= 0.0f ? farHit : -1.0f;
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
		auto *window = GetWindow<fe::SDLWindow>();
		auto capture = [&] {
			window->StartMouseCapture();
			RefreshJoysticks();
		};
		auto look = [&](float dx, float dy) {
			camera->yaw += dx * 0.1f;
			camera->pitch -= dy * 0.1f;
			camera->UpdateDirection();
			camera->pitch = std::clamp(camera->pitch, -89.0f, 89.0f);
		};

		while (window->PollSDLEvent(&event))
		{
			ImGui_ImplSDL3_ProcessEvent(&event);
			const bool uiCaptures = ImGui::GetIO().WantCaptureMouse;
			switch (event.type)
			{
			case SDL_EVENT_QUIT:
				window->PrepareClose();
				break;
			case SDL_EVENT_MOUSE_BUTTON_DOWN:
				if (event.button.which != SDL_TOUCH_MOUSEID && event.button.button == SDL_BUTTON_LEFT && !uiCaptures)
				{
					if (!window->IsCapturingMouse())
						capture();
					else if (mode == Mode::Walking)
						TryEnterVehicleAtScreenCenter();
				}
				break;
			case SDL_EVENT_WINDOW_RESIZED:
			case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
				Redraw();
				break;
			case SDL_EVENT_MOUSE_MOTION:
				if (event.motion.which != SDL_TOUCH_MOUSEID && window->IsCapturingMouse())
					look(static_cast<float>(event.motion.xrel), static_cast<float>(event.motion.yrel));
				break;
			case SDL_EVENT_FINGER_DOWN:
				if (event.tfinger.fingerID == 0 && !uiCaptures && !window->IsCapturingMouse())
					capture();
				break;
			case SDL_EVENT_FINGER_MOTION:
				if (event.tfinger.fingerID == 0 && window->IsCapturingMouse())
					look(event.tfinger.dx * window->width, event.tfinger.dy * window->height);
				break;
			case SDL_EVENT_FINGER_UP:
				if (event.tfinger.fingerID == 0 && !uiCaptures)
					window->StopMouseCapture();
				break;
			case SDL_EVENT_KEY_DOWN:
				switch (event.key.key)
				{
				case SDLK_F11: window->ToggleFullscreen(); break;
				case SDLK_F3: showDebugUI = !showDebugUI; break;
				case SDLK_F2: capture(); break;
				case SDLK_F4: ToggleXR(); break;
				case SDLK_F: if (mode != Mode::Walking) ExitVehicle(); break;
				case SDLK_G: if (mode == Mode::Airplane) SetAirplaneGear(!plane_.gearDown); break;
				default: break;
				}
				break;
			default: break;
			}
		}
		if (window->IsKeyDown(SDL_SCANCODE_ESCAPE) || ImGui::GetIO().WantCaptureMouse)
			window->StopMouseCapture();
	}

	static fe::Mesh<fe::Vertex> BuildBoxMesh(
		std::initializer_list<std::pair<glm::vec3, glm::vec3>> boxes)
	{
		std::vector<fe::Vertex> vertices;
		std::vector<uint32_t> indices;
		for (const auto &box : boxes)
		{
			const auto cube = fe::Primitives::GenerateCube(1.0f);
			const uint32_t base = static_cast<uint32_t>(vertices.size());
			for (auto vertex : cube.vertices)
			{
				vertex.position = box.first + vertex.position * (box.second * 2.0f);
				vertices.push_back(vertex);
			}
			for (uint32_t index : cube.indices)
				indices.push_back(base + index);
		}
		return fe::Mesh<fe::Vertex>(vertices, indices);
	}

	static fe::Mesh<fe::Vertex> BuildAirplaneMesh()
	{
		return BuildBoxMesh({
			{{0.0f, 0.0f, 0.0f}, {0.25f, 0.25f, 1.6f}},
			{{0.0f, 0.0f, 0.2f}, {1.8f, 0.06f, 0.4f}},
			{{0.0f, 0.0f, -1.4f}, {0.6f, 0.05f, 0.25f}},
			{{0.0f, 0.35f, -1.4f}, {0.05f, 0.35f, 0.3f}}});
	}

	static fe::Mesh<fe::Vertex> BuildAirplaneGearMesh()
	{
		return BuildBoxMesh({
			{{0.0f, -0.45f, 1.1f}, {0.06f, 0.30f, 0.06f}},
			{{0.0f, -0.72f, 1.1f}, {0.10f, 0.10f, 0.22f}},
			{{-0.6f, -0.45f, -0.3f}, {0.06f, 0.30f, 0.06f}},
			{{-0.6f, -0.72f, -0.3f}, {0.10f, 0.10f, 0.22f}},
			{{0.6f, -0.45f, -0.3f}, {0.06f, 0.30f, 0.06f}},
			{{0.6f, -0.72f, -0.3f}, {0.10f, 0.10f, 0.22f}}});
	}

	static std::vector<fe::PhysicsVehicle::WheelConfig> BuildAirplaneWheels()
	{
		using Wheel = fe::PhysicsVehicle::WheelConfig;
		return {
			{{0.0f, -0.45f, 1.1f}, 0.22f, 0.2f, 0.3f, 1.5f, 0.5f, 0.8f, true, false},
			{{-0.6f, -0.45f, -0.3f}, 0.28f, 0.2f, 0.3f, 1.5f, 0.5f, 0.8f, false, true},
			{{0.6f, -0.45f, -0.3f}, 0.28f, 0.2f, 0.3f, 1.5f, 0.5f, 0.8f, false, true}};
	}

	void CreateAirplaneVehicle()
	{
		auto *physics = GetPhysicsFactory();
		if (physics && plane_.body && !plane_.vehicle)
		{
			plane_.vehicle = physics->CreateVehicle(plane_.body, BuildAirplaneWheels());
			if (plane_.vehicle)
				plane_.vehicle->SetMaxPitchRollAngle(45.0f);
		}
	}

	void DestroyAirplaneVehicle()
	{
		if (!plane_.vehicle)
			return;
		if (auto *physics = GetPhysicsFactory())
			physics->DestroyVehicle(plane_.vehicle);
		plane_.vehicle = nullptr;
	}

	void SetAirplaneGear(bool down)
	{
		if (down == plane_.gearDown)
			return;
		plane_.gearDown = down;
		if (plane_.gear)
			plane_.gear->visible = down;
		if (down)
			CreateAirplaneVehicle();
		else
		{
			if (plane_.body)
				plane_.airSpeed = glm::length(plane_.body->GetLinearVelocity());
			DestroyAirplaneVehicle();
		}
	}

	void InitUI() override {}

	void DrawUI() override
	{
		const bool showXR = IsInstanceValid();
		if (!showDebugUI && !showXR)
			return;
		BeginFrame();

		if (showXR)
			DrawXRUI();
		if (showDebugUI)
			DrawDebugUI();

		fe::EditableGame::DrawDebugUI();

		EndFrame();
	}

	void DrawXRUI()
	{
		const auto input = GetWorldInputState();
		auto &xr = GetOpenXR();
		ImGui::Begin("XR Controllers");

		const char *poseNames[4] = {"Left grip", "Left aim", "Right grip", "Right aim"};
		const glm::vec3 positions[4] = {
			input.leftGripPosition, input.leftAimPosition, input.rightGripPosition, input.rightAimPosition};
		const glm::quat rotations[4] = {
			input.leftGripOrientation, input.leftAimOrientation,
			input.rightGripOrientation, input.rightAimOrientation};
		const bool active[4] = {
			input.leftGripPoseActive, input.leftAimPoseActive,
			input.rightGripPoseActive, input.rightAimPoseActive};
		for (size_t i = 0; i < 4; ++i)
		{
			if (active[i])
				ImGui::Text("%s  x %.3f  y %.3f  z %.3f", poseNames[i], positions[i].x, positions[i].y, positions[i].z);
			else
				ImGui::TextDisabled("%s  (not tracked)", poseNames[i]);
		}
		ImGui::Separator();
		for (size_t i = 0; i < 4; ++i)
		{
			if (!active[i])
				continue;
			const glm::quat &q = rotations[i];
			ImGui::Text("%s rot  %.2f %.2f %.2f %.2f", poseNames[i], q.x, q.y, q.z, q.w);
		}
		ImGui::Separator();
		ImGui::Text("Buttons");
		const char *buttonNames[8] = {"X (left, spawn)", "Y (left)", "A (right)", "B (right)",
			"Left trigger", "Right trigger", "Left squeeze", "Right squeeze"};
		const bool buttons[8] = {
			xr.IsButtonHeld(fe::OpenXR::Button::X), xr.IsButtonHeld(fe::OpenXR::Button::Y),
			xr.IsButtonHeld(fe::OpenXR::Button::A), xr.IsButtonHeld(fe::OpenXR::Button::B),
			xr.IsButtonHeld(fe::OpenXR::Button::LeftTrigger), xr.IsButtonHeld(fe::OpenXR::Button::RightTrigger),
			xr.IsGripPressed(fe::OpenXR::Hand::Left), xr.IsGripPressed(fe::OpenXR::Hand::Right)};
		for (size_t i = 0; i < 8; ++i)
			if (buttons[i])
				ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s: DOWN", buttonNames[i]);
			else
				ImGui::TextDisabled("%s: up", buttonNames[i]);
		ImGui::Text("Holding: %s (%s)",
			grabbed_ ? grabbed_->name.c_str() : "none",
			grabbedHand_ < 0 ? "-" : (grabbedHand_ == 0 ? "left" : "right"));
		ImGui::End();
	}

	void DrawDebugUI()
	{
		ImGui::Begin("SuperFlight");
		ImGui::Text("Mode: %s", mode == Mode::Walking ? "Walking" : (mode == Mode::Drone ? "Drone" : "Airplane"));
		if (mode != Mode::Walking && ImGui::Button("Exit vehicle (F)"))
			ExitVehicle();
		ImGui::TextDisabled("Left-click the plane/drone to fly it. F to exit.");

		auto slider = [](const char *label, float &value, float min, float max, const char *format = "%.1f") {
			ImGui::SliderFloat(label, &value, min, max, format);
		};
		if (ImGui::CollapsingHeader("Drone", ImGuiTreeNodeFlags_DefaultOpen))
		{
			auto &settings = drone_->GetSettings();
			ImGui::Checkbox("Altitude hold", &settings.altitudeHold);
			slider("Thrust", settings.thrust, 0.0f, 80.0f, "%.1f m/s2");
			slider("Lift", settings.lift, 0.0f, 60.0f, "%.1f m/s2");
			slider("Yaw Speed", droneCameraYawSpeed, 10.0f, 360.0f, "%.0f deg/s");
			slider("Pitch Speed", droneCameraPitchSpeed, 10.0f, 360.0f, "%.0f deg/s");
			slider("Linear Damping", settings.linearDamping, 0.0f, 5.0f);
			slider("Angular Damping", settings.angularDamping, 0.0f, 10.0f);
			ImGui::TextDisabled("WASD move, Space up, Shift down, arrows yaw/pitch");
			if (drone_->GetBody())
				ImGui::Text("Speed: %.1f m/s", glm::length(drone_->GetBody()->GetLinearVelocity()));
		}

		if (ImGui::CollapsingHeader("Airplane", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Text("Gear: %s (%s)", plane_.gearDown ? "DOWN" : "UP",
				plane_.gearDown ? "taxi/vehicle mode" : "flight mode");
			if (ImGui::Button(plane_.gearDown ? "Retract gear (G)" : "Extend gear (G)"))
				SetAirplaneGear(!plane_.gearDown);
			ImGui::TextDisabled(plane_.gearDown
				? "W throttle, S reverse, A/D steer, Shift brake, Space handbrake"
				: "W/S pitch, A/D roll, Q/E yaw, Shift/Ctrl throttle, Space level");
			ImGui::Checkbox("Chase camera", &plane_.chaseCam);
			ImGui::Checkbox("Invert pitch", &plane_.invertPitch);
			ImGui::Text("Throttle: %.0f%%", plane_.throttle * 100.0f);
			slider("Max Speed", plane_.maxSpeed, 10.0f, 200.0f, "%.0f m/s");
			slider("Acceleration", plane_.accel, 0.5f, 20.0f, "%.1f /s");
			slider("Throttle Rate", plane_.throttleRate, 0.1f, 5.0f, "%.1f /s");
			slider("Pitch Rate", plane_.pitchRate, 10.0f, 180.0f, "%.0f deg/s");
			slider("Roll Rate", plane_.rollRate, 10.0f, 270.0f, "%.0f deg/s");
			slider("Yaw Rate", plane_.yawRate, 10.0f, 120.0f, "%.0f deg/s");
			slider("Gravity Sag", plane_.gravitySag, 0.0f, 30.0f, "%.1f m/s");
			slider("Stall Speed", plane_.stallSpeed, 5.0f, 80.0f, "%.0f m/s");
			slider("Level Strength", plane_.levelStrength, 0.0f, 10.0f);
			slider("Cam Distance", plane_.camDistance, 2.0f, 40.0f);
			slider("Cam Height", plane_.camHeight, 0.0f, 20.0f);
			if (plane_.body)
			{
				ImGui::Text("Airspeed: %.1f m/s", plane_.airSpeed);
				const glm::vec3 velocity = plane_.body->GetLinearVelocity();
				ImGui::Text("Velocity: %.2f, %.2f, %.2f", velocity.x, velocity.y, velocity.z);
			}
		}

		if (ImGui::CollapsingHeader("Terrain"))
		{
			ImGui::DragInt("Render Distance", &chunkLoadDistance);
			ImGui::DragInt("Physics Distance", &physicsDistance, 0.5f, 0, 20);
			ImGui::SliderInt("Terrain Pre-gen", &chunkOutgenDistance, 1, 10);
		}
		ImGui::End();
	}

private:
	fe::OpenXR::InputState GetWorldInputState() const
	{
		const glm::vec3 origin = player ? player->state.position : glm::vec3(0.0f);
		const glm::quat rotation = glm::angleAxis(playerYaw, glm::vec3(0.0f, 1.0f, 0.0f));
		return GetOpenXR().GetWorldInputState(origin + positionOffset, rotation);
	}

	enum class Mode { Walking, Drone, Airplane };
	Mode mode = Mode::Walking;
	bool showDebugUI = false;

	struct HandRig
	{
		std::shared_ptr<fe::Object> cube;
		float scale = 1.0f;
		bool touching = false;
		glm::vec3 prevPos{0.0f};
		glm::quat prevRot{1.0f, 0.0f, 0.0f, 0.0f};
		bool hasPrev = false;
		glm::vec3 velocity{0.0f};
		glm::vec3 angularVelocity{0.0f};
	};

	struct Plane
	{
		std::shared_ptr<fe::Object> object;
		fe::PhysicsObject *body = nullptr;
		fe::Object *gear = nullptr;
		fe::PhysicsVehicle *vehicle = nullptr;
		bool gearDown = true;
		bool invertPitch = false;
		bool chaseCam = true;
		float throttle = 0.0f;
		float airSpeed = 0.0f;
		float maxSpeed = 90.0f;
		float accel = 6.0f;
		float throttleRate = 1.5f;
		float pitchRate = 60.0f;
		float rollRate = 120.0f;
		float yawRate = 45.0f;
		float gravitySag = 8.0f;
		float stallSpeed = 25.0f;
		float levelStrength = 4.0f;
		float camDistance = 12.0f;
		float camHeight = 4.0f;
		glm::vec2 cockpitOffset{0.6f, 0.4f};
	};

	const glm::vec3 playerSpawn{0.0f, 33.0f, 10.0f};
	const glm::vec3 droneSpawn{6.0f, 30.6f, 0.0f};
	const glm::vec3 airplaneSpawn{-6.0f, 31.2f, 0.0f};
	float walkSpeed = 25.0f;
	float sprintSpeed = 40.0f;
	float droneCameraYawSpeed = 120.0f;
	float droneCameraPitchSpeed = 90.0f;
	float droneCameraPitchLimit = 60.0f;

	std::unique_ptr<ChunkManager> chunkManager = std::make_unique<ChunkManager>(4);
#ifdef FE_HAS_VULKAN
	std::unique_ptr<ChunkBatcher> chunkBatcher_;
#endif
	glm::ivec2 playerCenter_{0, 0};
	int chunkLoadDistance = 6;
	int chunkOutgenDistance = 1;
	int physicsDistance = 2;

	std::unique_ptr<fe::Drone> drone_;
	Plane plane_;
	HandRig hands_[2];
	std::vector<std::shared_ptr<fe::Object>> handJointCubes_;
	std::shared_ptr<fe::Object> ball_;
	std::vector<std::shared_ptr<fe::Object>> grabbable_;
	float ballRadius_ = 0.25f;
	std::shared_ptr<fe::Object> grabbed_;
	int grabbedHand_ = -1;
	glm::vec3 grabOffset_{0.0f};
	glm::quat grabRotation_{1.0f, 0.0f, 0.0f, 0.0f};
	bool grabWasStatic_ = false;
};
