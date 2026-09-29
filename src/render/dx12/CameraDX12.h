#ifndef CAMERA_DX12_H
#define CAMERA_DX12_H

#include "PlayerInput.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

using namespace DirectX;

class Camera {
public:
    XMFLOAT3 Position;
    XMFLOAT3 Front;
    XMFLOAT3 Up;
    float Yaw;
    float Pitch;
    float MovementSpeed;
    float MouseSensitivity;
    bool BodycamAiming = false;
    bool BodycamActive = false;
    bool InvertY = false;
    // Scales the walking view bob; 0 is off. Set from GameSettings.
    float MovementViewIntensity = 1.0f;
    float BodycamFollowSpeed = 6.0f;
    float AimYawOffset = 0.0f;
    float AimPitchOffset = 0.0f;
    XMFLOAT3 AimFront{};

    const XMFLOAT3& GetAimFront() const {
        return BodycamActive ? AimFront : Front;
    }

    // How far the gun may lead the camera before the body has to catch up.
    static constexpr float kAimYawLeash = 18.0f;
    static constexpr float kAimPitchLeash = 12.0f;

    // Returns the body turn that brings an over-extended lead back inside its
    // leash. Applied where the input lands rather than at the next frame, so
    // the gun never visibly reaches past its limit and gets pulled back.
    static float ClampToLeash(float& offset, float leash) {
        const float clamped = std::clamp(offset, -leash, leash);
        const float excess = offset - clamped;
        offset = clamped;
        return excess;
    }

    void UpdateBodycamAim(float dt, bool allowed) {
        BodycamActive = BodycamAiming && allowed && FPSMode;
        if (!BodycamActive) {
            AimYawOffset = AimPitchOffset = 0.0f;
            return;
        }
        // Exponential follow, frame-rate independent: one constant rate at any
        // lead distance. A critically damped spring was tried here and it reads
        // as a different control -- it carries momentum and its rate varies
        // with how far the gun is leading, which changes the handling rather
        // than just smoothing it. This is the original curve; the rate below is
        // what softens it.
        const float blend =
            1.0f - std::exp(-BodycamFollowSpeed * (std::max)(dt, 0.0f));
        const float yawStep = AimYawOffset * blend;
        const float pitchStep = AimPitchOffset * blend;
        Yaw += yawStep;
        Pitch += pitchStep;
        AimYawOffset -= yawStep;
        AimPitchOffset -= pitchStep;
        updateCameraVectors();
    }
    
    // FPS mode settings
    bool FPSMode;
    float PlayerHeight;
    float FloorY;
    bool IsCrouching;
    bool IsSliding;
    XMFLOAT3 SlideDirection;
    float SlideSpeed;
    float SlideTimeRemaining;
    
    // Jump mechanics
    bool IsGrounded;
    float VerticalVelocity;
    float Gravity;
    float JumpStrength;

    // Swimming. WaterSurfaceY is pushed in each frame by the caller, which owns
    // the ocean; the camera only compares against it. NoWater parks the surface
    // below every level so the check is a no-op until someone sets it.
    static constexpr float NoWater = -1e9f;
    float WaterSurfaceY = NoWater;
    bool IsSwimming = false;
    // Vertical swim input for this frame: +1 rising, -1 diving, 0 coasting.
    float SwimInput = 0.0f;

    // Swim tuning. Water this shallow is waded, not swum -- as a fraction of
    // player height, so crouching does not change where the beach becomes sea.
    static constexpr float kWadeDepthFraction = 0.75f;
    // Floor this far below a grounded player is walked down onto, not fallen
    // to. Covers a stair riser and a walkable slope at sprint speed.
    static constexpr float kMaxStepDown = 0.5f;
    // Rate the stair-step view offset decays back to the real eye (1/s).
    static constexpr float kStepViewSettle = 14.0f;
    // How far the eyes ride below the surface while treading water. Small, so
    // the waterline sits at chin height and the view stays clear.
    static constexpr float kEyesUnderSurface = 0.22f;
    static constexpr float kBuoyancyStiffness = 7.0f;   // 1/s^2 toward float line
    // Gentler restoring pull once the head clears the surface -- a body out of
    // the water is not being buoyed, it is just falling back to the line.
    static constexpr float kAboveSurfaceBuoyancyScale = 0.55f;
    static constexpr float kSwimVerticalAccel = 9.0f;   // m/s^2 from Space/Q
    static constexpr float kWaterDrag = 3.4f;           // 1/s velocity decay
    static constexpr float kMaxSwimSpeed = 3.2f;        // m/s vertical clamp
    // Swimming is slower than running, and the stroke carries you level rather
    // than letting the look direction drive depth.
    static constexpr float kSwimSpeedScale = 0.55f;

    Camera(XMFLOAT3 position = XMFLOAT3(0.0f, 5.0f, 10.0f))
        : Position(position), Front(XMFLOAT3(0.0f, 0.0f, -1.0f)), Up(XMFLOAT3(0.0f, 1.0f, 0.0f)),
          Yaw(-90.0f), Pitch(-5.0f), MovementSpeed(5.0f), MouseSensitivity(0.1f),
          FPSMode(true), PlayerHeight(1.7f), FloorY(0.0f),
          IsCrouching(false), IsSliding(false),
          SlideDirection(0.0f, 0.0f, -1.0f), SlideSpeed(0.0f),
          SlideTimeRemaining(0.0f),
          IsGrounded(true), VerticalVelocity(0.0f), Gravity(9.8f), JumpStrength(5.0f) {
        updateCameraVectors();
    }

    // Rendering alone gets the walking offset; collision and shot origins keep
    // the stable player position.
    XMFLOAT3 VisualPosition() const {
        if (!FPSMode) return Position;
        return { Position.x + movementViewOffset_.x,
                 Position.y + movementViewOffset_.y + stepViewOffset_,
                 Position.z + movementViewOffset_.z };
    }

    void UpdateMovementView(float dt, float horizontalSpeed, float adsBlend,
                            bool walking) {
        if (dt <= 0.0f) return;
        const float speed = walking ? std::clamp(horizontalSpeed, 0.0f, 8.0f)
                                    : 0.0f;
        const float target = std::clamp((speed - 0.7f) / 4.3f, 0.0f, 1.0f);
        const float settle = 1.0f - std::exp(-12.0f * dt);
        movementViewStrength_ += (target - movementViewStrength_) * settle;
        if (target > 0.0f)
            movementViewPhase_ = std::fmod(
                movementViewPhase_ + speed * dt * (XM_2PI / 2.8f),
                XM_2PI * 2.0f);

        const float aimScale = 1.0f - 0.85f *
            std::clamp(adsBlend, 0.0f, 1.0f);
        const float side = std::sin(movementViewPhase_ * 0.5f) *
            0.018f * movementViewStrength_ * aimScale * MovementViewIntensity;
        const float rise = std::sin(movementViewPhase_) *
            0.045f * movementViewStrength_ * aimScale * MovementViewIntensity;
        XMVECTOR right = XMVector3Cross(
            XMLoadFloat3(&Up), XMLoadFloat3(&Front));
        if (XMVectorGetX(XMVector3LengthSq(right)) < 1e-6f)
            right = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
        else
            right = XMVector3Normalize(right);
        XMFLOAT3 rightAxis;
        XMStoreFloat3(&rightAxis, right);
        movementViewOffset_ = { rightAxis.x * side, rise,
                                rightAxis.z * side };
    }

    XMMATRIX GetViewMatrix() {
        const XMFLOAT3 visualPosition = VisualPosition();
        XMVECTOR pos = XMLoadFloat3(&visualPosition);
        XMVECTOR front = XMLoadFloat3(&Front);
        XMVECTOR up = XMLoadFloat3(&Up);
        if (explosionTrauma_ > 0.001f) {
            const float strength = explosionTrauma_ * explosionTrauma_;
            const float yaw = std::sin(explosionShakeTime_ * 37.0f + 0.7f) *
                              XMConvertToRadians(1.45f) * strength;
            const float pitch = std::sin(explosionShakeTime_ * 53.0f + 2.1f) *
                                XMConvertToRadians(1.05f) * strength;
            const XMVECTOR right = XMVector3Normalize(XMVector3Cross(front, up));
            front = XMVector3Normalize(front + right * std::tan(yaw) +
                                       up * std::tan(pitch));
            pos += right * (std::sin(explosionShakeTime_ * 43.0f) * 0.055f * strength);
            pos += up * (std::sin(explosionShakeTime_ * 61.0f + 1.3f) * 0.035f * strength);
        }
        if (firePitchOffset_ != 0.0f || fireYawOffset_ != 0.0f) {
            const float yaw = XMConvertToRadians(fireYawOffset_);
            const float pitch = XMConvertToRadians(firePitchOffset_);
            const XMVECTOR right = XMVector3Normalize(XMVector3Cross(front, up));
            front = XMVector3Normalize(front + right * std::tan(yaw) +
                                       up * std::tan(pitch));
        }
        return XMMatrixLookAtLH(pos, XMVectorAdd(pos, front), up);
    }

    void Update(float deltaTime) {
        explosionShakeTime_ += deltaTime;
        explosionTrauma_ = (std::max)(0.0f, explosionTrauma_ - deltaTime * 1.8f);
        if (firePitchOffset_ != 0.0f || firePitchVelocity_ != 0.0f ||
            fireYawOffset_ != 0.0f || fireYawVelocity_ != 0.0f) {
            constexpr float kSettleRate = 24.0f;
            const float dt = (std::max)(0.0f, deltaTime);
            const float decay = std::exp(-kSettleRate * dt);
            const auto settle = [&](float& offset, float& velocity,
                                    float maxAngle) {
                const float travel = velocity + kSettleRate * offset;
                offset = (offset + travel * dt) * decay;
                velocity = (velocity - kSettleRate * travel * dt) * decay;
                if (offset > maxAngle) {
                    offset = maxAngle;
                    velocity = (std::min)(velocity, 0.0f);
                } else if (offset < -maxAngle) {
                    offset = -maxAngle;
                    velocity = (std::max)(velocity, 0.0f);
                }
                if (std::abs(offset) < 0.0001f &&
                    std::abs(velocity) < 0.001f) {
                    offset = 0.0f;
                    velocity = 0.0f;
                }
            };
            settle(firePitchOffset_, firePitchVelocity_, 6.0f);
            settle(fireYawOffset_, fireYawVelocity_, 2.0f);
        }
        explosionFovKick_ *= std::exp(-8.5f * (std::max)(0.0f, deltaTime));
        if (explosionFovKick_ < 0.01f) explosionFovKick_ = 0.0f;
        if (FPSMode) {
            if (IsSliding) {
                Position.x += SlideDirection.x * SlideSpeed * deltaTime;
                Position.z += SlideDirection.z * SlideSpeed * deltaTime;
                SlideSpeed = (std::max)(0.0f, SlideSpeed - 9.0f * deltaTime);
                SlideTimeRemaining -= deltaTime;
                if (SlideTimeRemaining <= 0.0f ||
                    SlideSpeed <= MovementSpeed || !IsGrounded) {
                    IsSliding = false;
                    SlideSpeed = 0.0f;
                }
            }

            const float groundLevel = FloorY + PlayerHeight;

            // Swimming starts once the feet are under the surface and the
            // seabed is too deep to stand on. Wading in the shallows keeps
            // normal walking, which is what stops the player from breaking into
            // a swim while still ankle-deep on the beach.
            const float feetY = Position.y - PlayerHeight;
            const bool standingDepth =
                WaterSurfaceY - FloorY < PlayerHeight * kWadeDepthFraction;
            IsSwimming = feetY < WaterSurfaceY && !standingDepth;

            if (IsSwimming) {
                // Buoyancy toward a float line that keeps the eyes just above
                // the surface, so the default state is treading water with the
                // head out rather than bobbing under it.
                const float floatLine =
                    WaterSurfaceY + PlayerHeight - kEyesUnderSurface;
                const float toSurface = floatLine - Position.y;
                // Two-sided spring, so treading water settles on the float line
                // instead of coasting to a stop wherever drag happens to win.
                // Submerged, buoyancy is full strength; above the line it only
                // has to cancel the overshoot, and it yields entirely to a dive
                // so holding crouch actually takes the player under.
                float buoyancy = toSurface * kBuoyancyStiffness;
                if (toSurface < 0.0f) buoyancy *= kAboveSurfaceBuoyancyScale;
                if (SwimInput < 0.0f) buoyancy = (std::min)(buoyancy, 0.0f);
                VerticalVelocity += (buoyancy + SwimInput * kSwimVerticalAccel) *
                                    deltaTime;
                // Water drag. Heavy enough that vertical motion settles instead
                // of oscillating around the float line.
                VerticalVelocity *= std::exp(-kWaterDrag * deltaTime);
                VerticalVelocity = (std::max)(-kMaxSwimSpeed,
                    (std::min)(kMaxSwimSpeed, VerticalVelocity));
                Position.y += VerticalVelocity * deltaTime;

                // The seabed still stops the player, so diving to the bottom
                // stands on it rather than sinking through.
                if (Position.y <= groundLevel) {
                    Position.y = groundLevel;
                    if (VerticalVelocity < 0.0f) VerticalVelocity = 0.0f;
                }
                // Never grounded while swimming: jump, slide and step-up all
                // gate on IsGrounded and none of them should fire mid-water.
                IsGrounded = false;
            } else {
                const bool wasGrounded = IsGrounded;
                // Last frame's settled pose: horizontal walking has already
                // been applied by ProcessKeyboard before this runs.
                const XMFLOAT3 before = lastSettledPosition_;
                // Apply gravity
                VerticalVelocity -= Gravity * deltaTime;
                Position.y += VerticalVelocity * deltaTime;

                // Ground collision. A grounded player walking downhill or down
                // a stair stays glued to floor within step reach; gravity alone
                // left them airborne every frame on the way down a slope.
                const bool snapDown = wasGrounded && VerticalVelocity <= 0.0f &&
                                      Position.y - groundLevel <= kMaxStepDown;
                if (Position.y <= groundLevel || snapDown) {
                    Position.y = groundLevel;
                    VerticalVelocity = 0.0f;
                    IsGrounded = true;
                } else {
                    IsGrounded = false;
                }

                // Stair steps change height faster than any walkable slope
                // would for the distance moved. Absorb that jump into a
                // decaying view offset so the eye glides over the step.
                if (wasGrounded && IsGrounded) {
                    const float dx = Position.x - before.x;
                    const float dz = Position.z - before.z;
                    const float rise = Position.y - before.y;
                    if (std::abs(rise) <= kMaxStepDown &&
                        std::abs(rise) > std::sqrt(dx * dx + dz * dz) * 1.1f + 0.02f)
                        stepViewOffset_ = std::clamp(stepViewOffset_ - rise,
                                                     -kMaxStepDown, kMaxStepDown);
                }
                stepViewOffset_ *= std::exp(-kStepViewSettle * deltaTime);
                lastSettledPosition_ = Position;
            }
            SwimInput = 0.0f;
        }
    }

    void ProcessKeyboard(char direction, float deltaTime, float speedMultiplier = 1.0f) {
        float velocity = MovementSpeed * speedMultiplier * deltaTime;
        
        if (FPSMode) {
            // In water the vertical keys swim instead of jumping. Space rises,
            // Q dives; Jump() is suppressed because there is no ground to push
            // off and it would fling the player out of the water.
            if (IsSwimming) {
                if (direction == ' ') { SwimInput += 1.0f; return; }
                if (direction == 'Q') { SwimInput -= 1.0f; return; }
                velocity *= kSwimSpeedScale;
            } else if (direction == ' ') {
                Jump();
                return;
            }
            // FPS walking mode - movement constrained to XZ plane
            XMFLOAT3 frontXZ = XMFLOAT3(Front.x, 0.0f, Front.z);
            XMVECTOR frontXZVec = XMVector3Normalize(XMLoadFloat3(&frontXZ));
            XMStoreFloat3(&frontXZ, frontXZVec);
            
            XMVECTOR upVec = XMLoadFloat3(&Up);
            XMVECTOR rightXZVec = XMVector3Normalize(XMVector3Cross(frontXZVec, upVec));
            XMFLOAT3 rightXZ;
            XMStoreFloat3(&rightXZ, rightXZVec);
            
            if (direction == 'W') {
                Position.x += frontXZ.x * velocity;
                Position.z += frontXZ.z * velocity;
            }
            if (direction == 'S') {
                Position.x -= frontXZ.x * velocity;
                Position.z -= frontXZ.z * velocity;
            }
            if (direction == 'D') {
                Position.x -= rightXZ.x * velocity;
                Position.z -= rightXZ.z * velocity;
            }
            if (direction == 'A') {
                Position.x += rightXZ.x * velocity;
                Position.z += rightXZ.z * velocity;
            }
        } else {
            // Free fly mode
            XMVECTOR pos = XMLoadFloat3(&Position);
            XMVECTOR front = XMLoadFloat3(&Front);
            XMVECTOR up = XMLoadFloat3(&Up);
            XMVECTOR right = XMVector3Normalize(XMVector3Cross(front, up));
            
            if (direction == 'W') pos = XMVectorAdd(pos, XMVectorScale(front, velocity));
            if (direction == 'S') pos = XMVectorSubtract(pos, XMVectorScale(front, velocity));
            if (direction == 'D') pos = XMVectorSubtract(pos, XMVectorScale(right, velocity));
            if (direction == 'A') pos = XMVectorAdd(pos, XMVectorScale(right, velocity));
            
            XMStoreFloat3(&Position, pos);
        }
    }

    // Applies one sampled frame of intent. Deliberately delegates to
    // ProcessKeyboard rather than reimplementing the movement maths: the two
    // must stay identical, and the only way to guarantee that is to have one
    // implementation. Axes are thresholded rather than scaled so a full-tilt
    // stick reproduces the old key-held behaviour exactly; partial deflection
    // scales the speed through the existing multiplier argument.
    //
    // Look angles are absolute (see PlayerInput), so they are assigned rather
    // than accumulated -- that is what makes a dropped packet cost one stale
    // frame instead of a permanent aim desync.
    void ApplyInput(const PlayerInput& input) {
        const float dt = input.deltaTime;
        if (input.forward != 0.0f) {
            const float scale = std::abs(input.forward) * input.movementMultiplier;
            ProcessKeyboard(input.forward > 0.0f ? 'W' : 'S', dt, scale);
        }
        if (input.strafe != 0.0f) {
            const float scale = std::abs(input.strafe) * input.movementMultiplier;
            ProcessKeyboard(input.strafe > 0.0f ? 'A' : 'D', dt, scale);
        }
        if (input.Held(PlayerInput::Jump) || input.Held(PlayerInput::Swim))
            ProcessKeyboard(' ', dt);
        if (input.Held(PlayerInput::SwimDown)) ProcessKeyboard('Q', dt);
    }

    // Points the camera at an absolute orientation, for a remote player whose
    // angles arrive over the wire. Local look still goes through
    // ProcessMouseMovement, which owns sensitivity and pitch clamping.
    void SetViewAngles(float yawDegrees, float pitchDegrees) {
        AimYawOffset = AimPitchOffset = 0.0f;
        Yaw = yawDegrees;
        Pitch = std::clamp(pitchDegrees, -89.0f, 89.0f);
        updateCameraVectors();
    }

    void ProcessMouseMovement(float xoffset, float yoffset) {
        xoffset *= MouseSensitivity;
        yoffset *= MouseSensitivity;
        if (InvertY) yoffset = -yoffset;
        if (BodycamActive) {
            // Accumulate into the lead only -- the body is turned by the spring
            // in UpdateBodycamAim and nowhere else.
            //
            // This used to clamp here and add the excess straight to Yaw. That
            // made the smoothing frame-rate dependent in the worst direction:
            // mouse messages arrive at the device rate while the spring drains
            // once per frame, so at 20 fps the lead had 50 ms to fill, spent
            // most of it pinned at the leash, and dumped the overflow into the
            // camera unsmoothed. Low frame rates therefore stuttered far worse
            // than the frame rate alone accounted for.
            //
            // Aim is unaffected by the change: updateCameraVectors builds
            // AimFront from Yaw + AimYawOffset, so the point of aim still
            // tracks the mouse 1:1 and with no delay however far the camera
            // trails behind it.
            AimYawOffset -= xoffset;
            const float aimPitch =
                std::clamp(Pitch + AimPitchOffset + yoffset, -89.0f, 89.0f);
            AimPitchOffset = aimPitch - Pitch;
            // Enforce the leash here, at the moment the input lands, not at the
            // next frame's spring step. updateCameraVectors runs on every mouse
            // message, so a lead left over-extended until the frame ends is a
            // lead the player actually sees the gun reach before it is pulled
            // back. Turning the body by the excess keeps the point of aim
            // exactly 1:1 with the mouse while the lead stays inside its limit.
            //
            // This is the same instant path that used to make low frame rates
            // stutter, but it is no longer the one carrying the turn: the
            // progressive stiffness in UpdateBodycamAim has the body already
            // moving before the leash is reached, so only the part of a flick
            // faster than the body can follow arrives this way.
            Yaw += ClampToLeash(AimYawOffset, kAimYawLeash);
            Pitch += ClampToLeash(AimPitchOffset, kAimPitchLeash);
            updateCameraVectors();
            return;
        }
        Yaw -= xoffset;
        Pitch += yoffset;
        if (Pitch > 89.0f) Pitch = 89.0f;
        if (Pitch < -89.0f) Pitch = -89.0f;
        updateCameraVectors();
    }

    // `downed` drops the eye to the floor and outranks crouching: a downed
    // player is on the ground, and it is the one unambiguous way to tell them
    // so from inside their own head. Driven through the same lerp as crouch so
    // the grounded position correction below applies unchanged.
    void SetCrouching(bool crouching, float deltaTime, bool downed = false) {
        if (!FPSMode) return;
        constexpr float standingHeight = 1.7f;
        constexpr float crouchingHeight = 0.95f;
        constexpr float downedHeight = 0.40f;
        constexpr float transitionSpeed = 5.0f;
        IsCrouching = crouching;
        const float target = downed ? downedHeight
                           : (crouching ? crouchingHeight : standingHeight);
        const float oldHeight = PlayerHeight;
        const float step = transitionSpeed * deltaTime;
        if (PlayerHeight < target)
            PlayerHeight = (std::min)(target, PlayerHeight + step);
        else
            PlayerHeight = (std::max)(target, PlayerHeight - step);
        if (IsGrounded) Position.y += PlayerHeight - oldHeight;
    }

    bool StartSlide(float forwardInput, float strafeInput) {
        if (!FPSMode || !IsGrounded || IsSliding) return false;

        XMVECTOR forward = XMVectorSet(Front.x, 0.0f, Front.z, 0.0f);
        if (XMVectorGetX(XMVector3LengthSq(forward)) <= 1e-6f) return false;
        forward = XMVector3Normalize(forward);
        const XMVECTOR right = XMVector3Normalize(XMVector3Cross(
            forward, XMLoadFloat3(&Up)));
        XMVECTOR direction = XMVectorAdd(
            XMVectorScale(forward, forwardInput),
            XMVectorScale(right, strafeInput));
        if (XMVectorGetX(XMVector3LengthSq(direction)) <= 1e-6f) return false;

        direction = XMVector3Normalize(direction);
        XMStoreFloat3(&SlideDirection, direction);
        SlideSpeed = MovementSpeed * 2.2f;
        SlideTimeRemaining = 0.65f;
        IsSliding = true;
        return true;
    }

    // Instant angular kick from weapon recoil. Unlike mouse input this is
    // already expressed in degrees, so sensitivity must not scale it.
    void ApplyRecoil(float pitchDegrees, float yawDegrees) {
        Pitch = (std::max)(-89.0f, (std::min)(89.0f, Pitch + pitchDegrees));
        if (BodycamActive)
            AimPitchOffset = std::clamp(Pitch + AimPitchOffset, -89.0f, 89.0f) - Pitch;
        Yaw += yawDegrees;
        updateCameraVectors();
    }

    void ApplyExplosionImpulse(const XMFLOAT3& source, float visualSize) {
        const float dx = source.x - Position.x;
        const float dy = source.y - Position.y;
        const float dz = source.z - Position.z;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float reach = (std::max)(12.0f, visualSize * 5.5f);
        const float falloff = (std::max)(0.0f, 1.0f - distance / reach);
        if (falloff <= 0.0f) return;
        explosionTrauma_ = (std::min)(1.0f, explosionTrauma_ +
            falloff * (0.45f + visualSize * 0.055f));
        explosionFovKick_ = (std::max)(explosionFovKick_, 3.2f * falloff);
    }

    // Sharp jolt from taking a hit. Feeds the same shake the explosion uses so
    // there is one place that moves the view, but capped well below a blast:
    // being shot should punch the aim off, not blind the player.
    void AddHitTrauma(float amount) {
        explosionTrauma_ = (std::min)(0.72f, explosionTrauma_ + amount);
    }

    // Add velocity to a short, view-only spring. Automatic fire builds on the
    // current motion instead of restarting a waveform every round; blast and
    // hit trauma stay on their separate channel.
    void AddFireTrauma(float amount, float shakeScale = 1.0f) {
        const float scale = std::clamp(shakeScale, 0.0f, 30.0f);
        if (scale == 0.0f) {
            firePitchOffset_ = firePitchVelocity_ = 0.0f;
            fireYawOffset_ = fireYawVelocity_ = 0.0f;
            return;
        }
        const float kick = amount * scale;
        const float yawSign = fireYawRight_ ? 1.0f : -1.0f;
        fireYawRight_ = !fireYawRight_;
        firePitchOffset_ = std::clamp(firePitchOffset_ + kick * 0.65f,
                                      -6.0f, 6.0f);
        firePitchVelocity_ = std::clamp(firePitchVelocity_ + kick * 120.0f,
                                        -300.0f, 300.0f);
        fireYawOffset_ = std::clamp(fireYawOffset_ + yawSign * kick * 0.18f,
                                    -2.0f, 2.0f);
        fireYawVelocity_ = std::clamp(fireYawVelocity_ + yawSign * kick * 28.0f,
                                      -90.0f, 90.0f);
    }

    float ExplosionFovKick() const { return explosionFovKick_; }

    void Jump() {
        if (FPSMode && IsGrounded && !IsCrouching) {
            VerticalVelocity = JumpStrength;
            IsGrounded = false;
        }
    }

private:
    XMFLOAT3 movementViewOffset_{};
    float stepViewOffset_ = 0.0f;
    XMFLOAT3 lastSettledPosition_{};
    float movementViewPhase_ = 0.0f;
    float movementViewStrength_ = 0.0f;
    float explosionTrauma_ = 0.0f;
    float explosionShakeTime_ = 0.0f;
    float firePitchOffset_ = 0.0f;
    float firePitchVelocity_ = 0.0f;
    float fireYawOffset_ = 0.0f;
    float fireYawVelocity_ = 0.0f;
    bool fireYawRight_ = false;
    float explosionFovKick_ = 0.0f;

    void updateCameraVectors() {
        float yawRad = XMConvertToRadians(Yaw);
        float pitchRad = XMConvertToRadians(Pitch);
        
        XMFLOAT3 front;
        front.x = cosf(yawRad) * cosf(pitchRad);
        front.y = sinf(pitchRad);
        front.z = sinf(yawRad) * cosf(pitchRad);
        
        XMVECTOR frontVec = XMVector3Normalize(XMLoadFloat3(&front));
        XMStoreFloat3(&Front, frontVec);
        const float aimYaw = XMConvertToRadians(Yaw + AimYawOffset);
        const float aimPitch = XMConvertToRadians(std::clamp(Pitch + AimPitchOffset, -89.0f, 89.0f));
        AimFront = { cosf(aimYaw) * cosf(aimPitch), sinf(aimPitch),
                     sinf(aimYaw) * cosf(aimPitch) };
    }
};

#endif

