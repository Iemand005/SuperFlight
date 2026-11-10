#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <memory>
#include <string>
#include <vector>
#include <limits>
#include <algorithm>
#include <stdexcept>

#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

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
	SuperFlight(fe::XRGameOptions options) : fe::EditableGame(options)
	{
		SetClearColor(0.05f, 0.07f, 0.12f);

#if defined(__ANDROID__)
		LoadShaders("resources/shaders/VertexShader_gles300.glsl",
			"resources/shaders/FragmentShader_gles300.glsl");
#else
		if (!options.useVulkan)
		{
			LoadShaders("resources/shaders/VertexShader_gl330.glsl",
				"resources/shaders/FragmentShader_gl330.glsl");
		}
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
		case Mode::Drone:    if (drone_ && drone_->IsActive()) DroneUpdate(GetWindow<fe::SDLWindow>(), dt); break;
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

		// The spawn button is polled during the previous RedrawVR, so consume it
		// before the touch/grab pass runs.
		HandleXRSpawning();

		// Poses were polled during the previous RedrawVR; snap the controller
		// cubes to them before drawing again.
		UpdateControllerCubes(dt);


		switch (mode)
		{
		case Mode::Walking:  RespawnIfFell(); SyncCameraToPlayer(); break;
		case Mode::Drone:    if (drone_ && drone_->IsActive()) camera->SetPos(drone_->GetBody()->GetPosition()); break;
		case Mode::Airplane: if (airplaneBody_) AirplaneCameraUpdate(); break;
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

	void CreateThrowingLane()
	{
		const glm::vec3 laneCenter(0.0f, 0.0f, -24.0f);
		auto addLanePart = [&](const std::string& name, const glm::vec3& color,
							   const glm::vec3& size, const glm::vec3& position)
		{
			auto part = std::make_shared<fe::Object>(fe::Primitives::GenerateCube(1.0f));
			part->name = name;
			part->color = color;
			part->state.position = position;
			part->state.scale = size;
			part->isStatic = true;

			if (auto* physics = GetPhysicsFactory())
			{
				auto body = physics->CreateObject(size, false);
				if (body)
					body->SetPosition(position);
				part->SetPhysicsObject(std::move(body));
			}

			scene->AddObject(part);
		};

		addLanePart("ThrowingLaneFloor", glm::vec3(0.38f, 0.40f, 0.43f),
					glm::vec3(15.0f, 0.2f, 80.0f),
					laneCenter + glm::vec3(0.0f, 29.9f, 0.0f));
		addLanePart("ThrowingLaneLeftWall", glm::vec3(0.26f, 0.30f, 0.34f),
					glm::vec3(0.2f, 0.8f, 80.0f),
					laneCenter + glm::vec3(-7.4f, 30.4f, 0.0f));
		addLanePart("ThrowingLaneRightWall", glm::vec3(0.26f, 0.30f, 0.34f),
					glm::vec3(0.2f, 0.8f, 80.0f),
					laneCenter + glm::vec3(7.4f, 30.4f, 0.0f));
	}

	void CreateControllerCubes()
	{
		const fe::OpenXR::InputState input = GetWorldInputState();
		// Two small cubes that follow the XR controllers each frame (grip
		// pose) so hand tracking is visible both on screen and in the HMD.
		leftControllerObject_ = std::make_shared<fe::Object>(fe::Primitives::GenerateCube(
			{fe::PlaneDirection::Front, fe::PlaneDirection::Back,
			 fe::PlaneDirection::Left, fe::PlaneDirection::Right,
			 fe::PlaneDirection::Top, fe::PlaneDirection::Bottom},
			0.08f));
		leftControllerObject_->name = "LeftController";
		leftControllerObject_->color = glm::vec3(0.2f, 0.8f, 1.0f);
		leftControllerObject_->state.position = playerSpawn + glm::vec3(-0.25f, 1.2f, -0.3f);
		leftControllerObject_->isStatic = true;
		leftControllerObject_->visible = input.leftGripPoseActive;
		scene->AddObject(leftControllerObject_);

		rightControllerObject_ = std::make_shared<fe::Object>(fe::Primitives::GenerateCube(
			{fe::PlaneDirection::Front, fe::PlaneDirection::Back,
			 fe::PlaneDirection::Left, fe::PlaneDirection::Right,
			 fe::PlaneDirection::Top, fe::PlaneDirection::Bottom},
			0.08f));
		rightControllerObject_->name = "RightController";
		rightControllerObject_->color = glm::vec3(1.0f, 0.5f, 0.1f);
		rightControllerObject_->state.position = playerSpawn + glm::vec3(0.25f, 1.2f, -0.3f);
		rightControllerObject_->isStatic = true;
		rightControllerObject_->visible = input.rightGripPoseActive;
		scene->AddObject(rightControllerObject_);
	}

	void CreateBall()
	{
		ballObject_ = std::make_shared<fe::Object>(fe::Primitives::GenerateSphere(ballRadius_, 24, 16));
		ballObject_->name = "XRBall";
		ballObject_->color = glm::vec3(0.95f, 0.85f, 0.2f);
		ballObject_->state.position = playerSpawn + glm::vec3(0.0f, 1.05f, -0.55f);
		ballObject_->isStatic = false;

		if (auto *physics = GetPhysicsFactory())
		{
			auto body = physics->CreateSphereObject(ballRadius_, true);
			if (body)
			{
				body->SetPosition(ballObject_->state.position);
				body->SetLinearVelocity(glm::vec3(0.0f));
				body->SetAngularVelocity(glm::vec3(0.0f));
			}
			ballObject_->SetPhysicsObject(std::move(body));
		}

		scene->AddObject(ballObject_);
		grabbableObjects_.push_back(ballObject_);
	}

	void HandleXRSpawning()
	{
		if (GetOpenXR().ConsumeButtonPress(fe::OpenXR::Button::X))
			SpawnSphere();
	}

	void SpawnSphere()
	{
		auto *physics = GetPhysicsFactory();
		if (!physics)
			return;

		glm::vec3 forward = camera->front;
		forward.y = 0.0f;
		if (glm::length(forward) < 1e-4f)
			forward = glm::vec3(0.0f, 0.0f, -1.0f);
		forward = glm::normalize(forward);

		const glm::vec3 base = player ? player->state.position : playerSpawn;
		const glm::vec3 spawnPos = base + glm::vec3(0.0f, 1.5f, 0.0f) + forward * 0.8f;

		auto sphere = std::make_shared<fe::Object>(fe::Primitives::GenerateSphere(ballRadius_, 24, 16));
		sphere->name = "XRSphere";
		sphere->color = glm::vec3(0.3f, 0.85f, 0.45f);
		sphere->state.position = spawnPos;
		sphere->isStatic = false;

		auto body = physics->CreateSphereObject(ballRadius_, true);
		if (body)
		{
			body->SetPosition(spawnPos);
			body->SetLinearVelocity(glm::vec3(0.0f));
			body->SetAngularVelocity(glm::vec3(0.0f));
		}
		sphere->SetPhysicsObject(std::move(body));

		scene->AddObject(sphere);
		grabbableObjects_.push_back(sphere);

		GetOpenXR().TriggerHaptic(fe::OpenXR::Hand::Left, 0.5f, 0.08f);
	}

	void UpdateControllerCubes(float dt)
	{
		const fe::OpenXR::InputState input = GetWorldInputState();
		const float baseScale = 1.0f;
		const float growRate = 0.2f;
		const float shrinkRate = 0.5f;
		const float maxScale = 1.35f;
		const float cubeHalfExtent = 0.08f;

		// Track hand motion so a released object keeps the hand's momentum.
		auto updateHandMotion = [&](const glm::vec3& pos, const glm::quat& rot, bool active,
									glm::vec3& prevPos, glm::quat& prevRot, bool& hasPrev,
									glm::vec3& velocity, glm::vec3& angularVelocity)
		{
			if (!active)
			{
				hasPrev = false;
				velocity = glm::vec3(0.0f);
				angularVelocity = glm::vec3(0.0f);
				return;
			}

			if (hasPrev && dt > 1e-5f)
			{
				const glm::vec3 rawVelocity = (pos - prevPos) / dt;
				velocity = glm::mix(velocity, rawVelocity, 0.75f);

				glm::quat delta = rot * glm::inverse(prevRot);
				if (delta.w < 0.0f)
					delta = glm::quat(-delta.w, -delta.x, -delta.y, -delta.z);
				const float angle = 2.0f * std::acos(std::clamp(delta.w, -1.0f, 1.0f));
				const glm::vec3 axis(delta.x, delta.y, delta.z);
				const float axisLength = glm::length(axis);
				if (axisLength > 1e-6f && angle > 1e-6f)
				{
					const glm::vec3 rawAngular = (axis / axisLength) * (angle / dt);
					angularVelocity = glm::mix(angularVelocity, rawAngular, 0.5f);
				}
				else
				{
					angularVelocity = glm::vec3(0.0f);
				}
			}

			prevPos = pos;
			prevRot = rot;
			hasPrev = true;
		};

		updateHandMotion(input.leftGripPosition, input.leftGripOrientation, input.leftGripPoseActive,
						 prevLeftGripPosition_, prevLeftGripOrientation_, hasPrevLeftGrip_,
						 leftGripVelocity_, leftGripAngularVelocity_);
		updateHandMotion(input.rightGripPosition, input.rightGripOrientation, input.rightGripPoseActive,
						 prevRightGripPosition_, prevRightGripOrientation_, hasPrevRightGrip_,
						 rightGripVelocity_, rightGripAngularVelocity_);

		// Nearest grabbable sphere within reach of a hand.
		auto findTouched = [&](const glm::vec3& gripPos, float scale) -> std::shared_ptr<fe::Object> {
			std::shared_ptr<fe::Object> best;
			float bestDist = std::numeric_limits<float>::max();
			for (const auto& obj : grabbableObjects_)
			{
				if (!obj)
					continue;
				const float reach = cubeHalfExtent * scale + ballRadius_ * obj->state.scale.x;
				const float dist = glm::length(gripPos - obj->state.position);
				if (dist <= reach && dist < bestDist)
				{
					best = obj;
					bestDist = dist;
				}
			}
			return best;
		};

		auto updateCube = [&](const std::shared_ptr<fe::Object>& cube,
							  const glm::vec3& gripPos, const glm::quat& gripOrientation,
							  bool poseActive, float& scale, bool& touching,
							  int handIndex, fe::OpenXR::Hand hand)
		{
			if (!cube)
				return;

			cube->visible = poseActive;
			if (!poseActive)
			{
				touching = false;
				return;
			}

			cube->SetPosition(gripPos);
			cube->SetOrientation(gripOrientation);

			const bool holdingThis = grabbedObject_ && grabbedHand_ == handIndex;
			const bool hit = findTouched(gripPos, scale) != nullptr || holdingThis;

			if (hit && !holdingThis)
			{
				scale = std::min(scale + growRate * dt, maxScale);
				if (!touching)
					GetOpenXR().TriggerHaptic(hand, 0.7f, 0.08f);
			}
			else if (!hit)
			{
				scale = std::max(scale - shrinkRate * dt, baseScale);
			}

			touching = hit;
			cube->SetScale(glm::vec3(scale));
		};

		updateCube(leftControllerObject_, input.leftGripPosition, input.leftGripOrientation,
				   input.leftGripPoseActive, leftControllerScale_, leftTouchingObject_,
				   0, fe::OpenXR::Hand::Left);
		updateCube(rightControllerObject_, input.rightGripPosition, input.rightGripOrientation,
				   input.rightGripPoseActive, rightControllerScale_, rightTouchingObject_,
				   1, fe::OpenXR::Hand::Right);

		const bool leftGrip = GetOpenXR().IsGripPressed(fe::OpenXR::Hand::Left);
		const bool rightGrip = GetOpenXR().IsGripPressed(fe::OpenXR::Hand::Right);

		if (grabbedObject_)
		{
			const bool handGrip = grabbedHand_ == 0 ? leftGrip : rightGrip;
			const bool poseActive = grabbedHand_ == 0 ? input.leftGripPoseActive : input.rightGripPoseActive;
			if (!handGrip || !poseActive)
			{
				ReleaseGrabbedObject();
			}
			else
			{
				// Rigidly attach to the hand: the offset is stored in hand-local
				// space, so moving and rotating the hand moves and rotates the
				// object around the grip point.
				const glm::vec3 handPos = grabbedHand_ == 0 ? input.leftGripPosition : input.rightGripPosition;
				const glm::quat handRot = grabbedHand_ == 0 ? input.leftGripOrientation : input.rightGripOrientation;
				const glm::vec3 targetPos = handPos + handRot * grabLocalOffset_;
				const glm::quat targetRot = handRot * grabLocalRotation_;
				grabbedObject_->SetPosition(targetPos);
				grabbedObject_->SetOrientation(targetRot);
				if (grabbedObject_->physicsObject)
				{
					grabbedObject_->physicsObject->SetPosition(targetPos);
					grabbedObject_->physicsObject->SetRotation(targetRot);
				}
			}
			return;
		}

		// Grab the nearest object touched by a gripping hand.
		if (leftGrip && input.leftGripPoseActive)
		{
			if (auto obj = findTouched(input.leftGripPosition, leftControllerScale_))
				GrabObject(obj, 0, input.leftGripPosition, input.leftGripOrientation);
		}
		else if (rightGrip && input.rightGripPoseActive)
		{
			if (auto obj = findTouched(input.rightGripPosition, rightControllerScale_))
				GrabObject(obj, 1, input.rightGripPosition, input.rightGripOrientation);
		}
	}

	void GrabObject(const std::shared_ptr<fe::Object>& obj, int handIndex,
					const glm::vec3& handPos, const glm::quat& handRot)
	{
		if (!obj)
			return;

		const glm::quat invHand = glm::inverse(handRot);
		grabbedObject_ = obj;
		grabbedHand_ = handIndex;
		grabLocalOffset_ = invHand * (obj->state.position - handPos);
		grabLocalRotation_ = invHand * obj->state.orientation;
		grabWasStatic_ = obj->isStatic;
		obj->isStatic = true;

		GetOpenXR().TriggerHaptic(handIndex == 0 ? fe::OpenXR::Hand::Left : fe::OpenXR::Hand::Right,
								  0.8f, 0.1f);
	}

	void ReleaseGrabbedObject()
	{
		if (!grabbedObject_)
			return;

		if (grabbedObject_->physicsObject)
		{
			grabbedObject_->physicsObject->SetPosition(grabbedObject_->state.position);
			grabbedObject_->physicsObject->SetRotation(grabbedObject_->state.orientation);

			// Carry the hand's momentum into the object so it can be thrown.
			glm::vec3 throwVelocity = grabbedHand_ == 0 ? leftGripVelocity_ : rightGripVelocity_;
			throwVelocity *= 2.5f;
			const float maxThrowSpeed = 45.0f;
			const float throwSpeed = glm::length(throwVelocity);
			if (throwSpeed > maxThrowSpeed)
				throwVelocity = throwVelocity / throwSpeed * maxThrowSpeed;

			glm::vec3 spin = grabbedHand_ == 0 ? leftGripAngularVelocity_ : rightGripAngularVelocity_;
			const float maxSpin = 30.0f;
			const float spinLength = glm::length(spin);
			if (spinLength > maxSpin)
				spin = spin / spinLength * maxSpin;

			grabbedObject_->physicsObject->SetLinearVelocity(throwVelocity);
			grabbedObject_->physicsObject->SetAngularVelocity(spin);
		}
		grabbedObject_->isStatic = grabWasStatic_;
		grabbedObject_.reset();
		grabbedHand_ = -1;
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
		drone_ = std::make_unique<fe::Drone>();
		if (!drone_->Spawn(*scene, droneSpawn))
			throw std::runtime_error("Failed to spawn the SuperFlight demo drone");

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
		if (!physics || !drone_ || !drone_->IsSpawned())
			return;

		if (!drone_->Enter(*physics))
			return;
		mode = Mode::Drone;
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

		if (mode == Mode::Drone && drone_ && drone_->IsSpawned())
		{
			vehicle = drone_->GetDroneObject().get();
			drone_->Exit();
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
		if (window->IsKeyDown(SDL_SCANCODE_LEFT))  camera->yaw -= droneCameraYawSpeed * dt;
		if (window->IsKeyDown(SDL_SCANCODE_RIGHT)) camera->yaw += droneCameraYawSpeed * dt;
		if (window->IsKeyDown(SDL_SCANCODE_UP))    camera->pitch += droneCameraPitchSpeed * dt;
		if (window->IsKeyDown(SDL_SCANCODE_DOWN))  camera->pitch -= droneCameraPitchSpeed * dt;
		camera->pitch = std::clamp(camera->pitch, -droneCameraPitchLimit, droneCameraPitchLimit);
		camera->UpdateDirection();

		glm::vec2 horizontalInput(0.0f);
		if (window->IsKeyDown(SDL_SCANCODE_A)) horizontalInput.x -= 1.0f;
		if (window->IsKeyDown(SDL_SCANCODE_D)) horizontalInput.x += 1.0f;
		if (window->IsKeyDown(SDL_SCANCODE_S)) horizontalInput.y -= 1.0f;
		if (window->IsKeyDown(SDL_SCANCODE_W)) horizontalInput.y += 1.0f;

		float verticalInput = 0.0f;
		if (window->IsKeyDown(SDL_SCANCODE_SPACE))  verticalInput += 1.0f;
		if (window->IsKeyDown(SDL_SCANCODE_LSHIFT)) verticalInput -= 1.0f;
		drone_->Update(horizontalInput, verticalInput, glm::radians(camera->yaw));
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

		fe::Object *candidates[2] = {
			drone_ ? drone_->GetDroneObject().get() : nullptr,
			airplaneObject_.get()};
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

		if (drone_ && best == drone_->GetDroneObject().get())
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
		const bool showXRPoses = IsInstanceValid();
		if (!showDebugUI && !showXRPoses)
			return;
		BeginFrame();

		if (showXRPoses)
		{
			const fe::OpenXR::InputState input = GetWorldInputState();
			ImGui::Begin("XR Controllers");
			auto poseLine = [](const char* label, const glm::vec3& p, bool active)
			{
				if (active)
					ImGui::Text("%s  x %.3f  y %.3f  z %.3f", label, p.x, p.y, p.z);
				else
					ImGui::TextDisabled("%s  (not tracked)", label);
			};
			auto rotationLine = [](const char* label, const glm::quat& q, bool active)
			{
				if (active)
					ImGui::Text("%s  qx %.2f  qy %.2f  qz %.2f  qw %.2f", label, q.x, q.y, q.z, q.w);
				else
					ImGui::TextDisabled("%s  (not tracked)", label);
			};
			poseLine("Left grip ", input.leftGripPosition, input.leftGripPoseActive);
			poseLine("Left aim  ", input.leftAimPosition, input.leftAimPoseActive);
			poseLine("Right grip", input.rightGripPosition, input.rightGripPoseActive);
			poseLine("Right aim ", input.rightAimPosition, input.rightAimPoseActive);
			rotationLine("Left grip rot ", input.leftGripOrientation, input.leftGripPoseActive);
			rotationLine("Left aim rot  ", input.leftAimOrientation, input.leftAimPoseActive);
			rotationLine("Right grip rot", input.rightGripOrientation, input.rightGripPoseActive);
			rotationLine("Right aim rot ", input.rightAimOrientation, input.rightAimPoseActive);

			ImGui::Separator();
			ImGui::Text("Buttons");
			auto heldLine = [](const char* label, bool down)
			{
				if (down)
					ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s: DOWN", label);
				else
					ImGui::TextDisabled("%s: up", label);
			};
			heldLine("X (left, spawn)", GetOpenXR().IsButtonHeld(fe::OpenXR::Button::X));
			heldLine("Y (left)", GetOpenXR().IsButtonHeld(fe::OpenXR::Button::Y));
			heldLine("A (right)", GetOpenXR().IsButtonHeld(fe::OpenXR::Button::A));
			heldLine("B (right)", GetOpenXR().IsButtonHeld(fe::OpenXR::Button::B));
			heldLine("Left trigger", GetOpenXR().IsButtonHeld(fe::OpenXR::Button::LeftTrigger));
			heldLine("Right trigger", GetOpenXR().IsButtonHeld(fe::OpenXR::Button::RightTrigger));
			heldLine("Left squeeze (grip)", GetOpenXR().IsGripPressed(fe::OpenXR::Hand::Left));
			heldLine("Right squeeze (grip)", GetOpenXR().IsGripPressed(fe::OpenXR::Hand::Right));
			ImGui::Text("Holding: %s (%s)",
				grabbedObject_ ? grabbedObject_->name.c_str() : "none",
				grabbedHand_ < 0 ? "-" : (grabbedHand_ == 0 ? "left" : "right"));
			ImGui::End();
		}

		if (!showDebugUI)
		{
			EndFrame();
			return;
		}

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
			auto& settings = drone_->GetSettings();
			ImGui::Checkbox("Altitude hold", &settings.altitudeHold);
			ImGui::SliderFloat("Thrust", &settings.thrust, 0.0f, 80.0f, "%.1f m/s2");
			ImGui::SliderFloat("Lift", &settings.lift, 0.0f, 60.0f, "%.1f m/s2");
			ImGui::SliderFloat("Yaw Speed", &droneCameraYawSpeed, 10.0f, 360.0f, "%.0f deg/s");
			ImGui::SliderFloat("Pitch Speed", &droneCameraPitchSpeed, 10.0f, 360.0f, "%.0f deg/s");
			ImGui::SliderFloat("Linear Damping", &settings.linearDamping, 0.0f, 5.0f);
			ImGui::SliderFloat("Angular Damping", &settings.angularDamping, 0.0f, 10.0f);
			ImGui::TextDisabled("W/S forward-back (level), A/D strafe, Space up, Shift down");
			ImGui::TextDisabled("Arrows: yaw/pitch. Mouse: FPV look.");
			if (drone_ && drone_->GetBody())
			{
				glm::vec3 v = drone_->GetBody()->GetLinearVelocity();
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
	fe::OpenXR::InputState GetWorldInputState() const
	{
		const glm::vec3 playerPosition = player ? player->state.position : glm::vec3(0.0f);
		const glm::quat playerRotation = glm::angleAxis(playerYaw, glm::vec3(0.0f, 1.0f, 0.0f));
		return GetOpenXR().GetWorldInputState(playerPosition + positionOffset, playerRotation);
	}

	enum class Mode { Walking, Drone, Airplane };
	Mode mode = Mode::Walking;

	bool showDebugUI = false;

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

	std::shared_ptr<fe::Object> leftControllerObject_;
	std::shared_ptr<fe::Object> rightControllerObject_;
	std::shared_ptr<fe::Object> ballObject_;
	std::vector<std::shared_ptr<fe::Object>> grabbableObjects_;
	float ballRadius_ = 0.25f;
	float leftControllerScale_ = 1.0f;
	float rightControllerScale_ = 1.0f;
	bool leftTouchingObject_ = false;
	bool rightTouchingObject_ = false;
	std::shared_ptr<fe::Object> grabbedObject_;
	int grabbedHand_ = -1;
	glm::vec3 grabLocalOffset_{0.0f};
	glm::quat grabLocalRotation_{1.0f, 0.0f, 0.0f, 0.0f};
	bool grabWasStatic_ = false;

	glm::vec3 prevLeftGripPosition_{0.0f};
	glm::vec3 prevRightGripPosition_{0.0f};
	glm::quat prevLeftGripOrientation_{1.0f, 0.0f, 0.0f, 0.0f};
	glm::quat prevRightGripOrientation_{1.0f, 0.0f, 0.0f, 0.0f};
	bool hasPrevLeftGrip_ = false;
	bool hasPrevRightGrip_ = false;
	glm::vec3 leftGripVelocity_{0.0f};
	glm::vec3 rightGripVelocity_{0.0f};
	glm::vec3 leftGripAngularVelocity_{0.0f};
	glm::vec3 rightGripAngularVelocity_{0.0f};

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


};
