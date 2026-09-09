#pragma once

// Private application implementation; included once by main.cpp in dependency order.

static void ToggleHumveeDriving() {
    XMFLOAT4X4 pose;
    XMFLOAT3 position, forward;

    if (!g_drivingHumvee) {
        float nearestDistanceSq = 25.0f;
        size_t nearestIndex = kNoHumvee;
        for (size_t index = 0; index < g_destruction.VehicleCount(); ++index) {
            XMFLOAT3 candidatePosition;
            if (!g_destruction.GetVehicleTransform(
                    index, pose, &candidatePosition, &forward)) continue;
            const XMVECTOR offset = XMLoadFloat3(&scene.camera.Position) -
                                    XMLoadFloat3(&candidatePosition);
            const float distanceSq = XMVectorGetX(XMVector3LengthSq(offset));
            if (distanceSq > nearestDistanceSq) continue;
            nearestDistanceSq = distanceSq;
            nearestIndex = index;
            position = candidatePosition;
        }
        if (nearestIndex == kNoHumvee) return;
        g_activeHumveeIndex = nearestIndex;
        g_drivingHumvee = true;
        g_savedGunVisible = scene.gun.visible;
        scene.gun.visible = false;
        scene.camera.FPSMode = false;
        scene.camera.VerticalVelocity = 0.0f;
        return;
    }

    if (!g_destruction.GetVehicleTransform(
            g_activeHumveeIndex, pose, &position, &forward)) return;

    g_drivingHumvee = false;
    g_destruction.SetVehicleInput(g_activeHumveeIndex, 0.0f, 0.0f, true);
    scene.gun.visible = g_savedGunVisible;
    scene.camera.FPSMode = true;
    const XMVECTOR forwardVector = XMLoadFloat3(&forward);
    const XMVECTOR right = XMVector3Normalize(XMVector3Cross(
        forwardVector, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)));
    XMVECTOR exitPosition = XMLoadFloat3(&position) + right * 2.3f;
    exitPosition = XMVectorSetY(exitPosition, XMVectorGetY(exitPosition) + 1.3f);
    XMStoreFloat3(&scene.camera.Position, exitPosition);
    g_activeHumveeIndex = kNoHumvee;
}

// Advances the idle hover/spin on every live pickup. Collection itself is on E
// (see CollectNearbyWeaponPickup) -- walking over one does nothing, so a pickup
// can be walked past without silently costing the weapon in hand.
//
// Skipped while driving: the camera is the chase camera out behind the chassis,
// so proximity would measure from the vehicle rather than the player.
// Picks a footstep variant, avoiding an immediate repeat of the last one, and
// plays it with a randomised pitch.
//
// `positional` is false for the player: their steps originate at the listener,
// where panning is meaningless and the rolloff curve would put them at full
// volume anyway.
//
// Pitch jitter is what makes two samples sound like many. +/-12% is wide enough
// to disguise a repeat without the step sounding like a different surface or a
// different-sized person.
static void PlayFootstep(float volume, bool positional,
                         const XMFLOAT3& position = {}, float range = 22.0f) {
    int variant = std::rand() % kFootstepVariantCount;
    // One retry rather than a loop: with two samples a single nudge is enough to
    // break a repeat, and a while-loop here could spin on a degenerate rand.
    if (variant == g_lastFootstepVariant)
        variant = (variant + 1) % kFootstepVariantCount;
    g_lastFootstepVariant = variant;

    const float pitch = 0.88f + ((float)std::rand() / RAND_MAX) * 0.24f;
    // Small per-step gain wobble too, so an unvarying stride does not read as a
    // metronome even at a constant walking speed.
    const float gain = volume * (0.88f + ((float)std::rand() / RAND_MAX) * 0.24f);
    if (positional)
        g_footstepAudio[variant].PlayAt(position.x, position.y, position.z,
                                        gain, pitch, range);
    else
        g_footstepAudio[variant].Play(gain, pitch);
}

// Footsteps for the player and every live enemy.
//
// Driven by distance travelled rather than a timer, so a footfall always
// corresponds to a step actually taken: walking into a wall makes no sound, and
// sprinting raises the cadence for free because the distance accumulates faster.
//
// The player's own steps are deliberately NOT spatialised -- they come from the
// listener's own position, where panning is meaningless and X3DAudio's rolloff
// would put them at full volume anyway. Enemy steps are positional, which is the
// point: hearing a patrol move behind a building is the cue the stealth systems
// have been missing.
static void UpdateFootsteps(float dt) {
    if (dt <= 0.0f || g_emptyLevelMode) return;
    if (g_game.session.Screen() != GameScreen::Level1 && !IsEditorPlaying())
        return;

    // --- Player ---
    // Skipped while riding or driving anything: the camera moves without the
    // player taking a step, exactly as with the sprint animation.
    const bool onFoot = !g_drivingHumvee &&
                        !g_game.vehicles.blackHawkCarryingPlayer &&
                        !g_insertionChoicePending &&
                        scene.camera.FPSMode &&
                        scene.player.health > 0.0f;
    if (onFoot && scene.camera.IsGrounded) {
        const float speed = g_game.playerMovement.HorizontalSpeed();
        // Below a slow walk there is no step to place; this also stops a
        // stationary player from creeping over the threshold on drift alone.
        if (speed > 0.9f) {
            g_playerStepDistance += speed * dt;
            const float stride = g_playerSprinting ? kStepStrideSprint
                                                   : kStepStrideWalk;
            const float crouchScale = scene.camera.IsCrouching ? 1.6f : 1.0f;
            if (g_playerStepDistance >= stride * crouchScale) {
                g_playerStepDistance = 0.0f;
                // Crouching is near-silent, sprinting is heavier. This is the
                // player's own boot, so it is not spatialised.
                const float volume = scene.camera.IsCrouching ? 0.22f
                                   : (g_playerSprinting ? 0.55f : 0.40f);
                PlayFootstep(volume, false);
            }
        } else {
            // Bleed the accumulator away when stopped so the next step after a
            // pause lands on a fresh stride rather than immediately.
            g_playerStepDistance =
                (std::max)(0.0f, g_playerStepDistance - dt * 1.5f);
        }
    }

    // --- Sprint stamina and exhaustion breathing ---
    //
    // The meter drains only while the sprint is actually doing something: shift
    // held against a wall costs nothing, which is why this repeats the same
    // speed threshold the footsteps use rather than trusting the key state.
    //
    // Breathing fires when the meter empties, then keeps repeating on a
    // cooldown for as long as the player stays exhausted. It is retriggered
    // rather than looped so it ends on its own once they have recovered.
    const bool drainingStamina = onFoot && g_playerSprinting &&
                                 scene.camera.IsGrounded &&
                                 g_game.playerMovement.HorizontalSpeed() > 0.9f;
    g_breathingCooldown = (std::max)(0.0f, g_breathingCooldown - dt);
    if (drainingStamina) {
        g_staminaRecoveryDelay = kStaminaRecoveryDelay;
        g_staminaSeconds = (std::max)(0.0f,
            g_staminaSeconds - dt * kStaminaDrainRate);
        if (g_staminaSeconds <= 0.0f) g_staminaExhausted = true;
    } else {
        // The delay only counts down once the player has stopped sprinting, so
        // a burst of sprint-stop-sprint does not quietly refill the meter.
        g_staminaRecoveryDelay =
            (std::max)(0.0f, g_staminaRecoveryDelay - dt);
        if (g_staminaRecoveryDelay <= 0.0f)
            g_staminaSeconds = (std::min)(kStaminaMaxSeconds,
                g_staminaSeconds + dt * kStaminaRecoveryRate);
        // Sprint unlocks again only after a real margin has rebuilt, so the
        // player cannot chain single-frame sprints off an empty meter.
        if (g_staminaExhausted && g_staminaSeconds >= kStaminaRecoveredToSprint)
            g_staminaExhausted = false;
    }
    // Winded: heard from the moment the meter drops under half, through empty,
    // and all the way back up until it has recovered past half again. A player
    // who never spent more than half their stamina is not winded.
    //
    // The exhausted flag is kept in the test rather than relying on the
    // fraction alone. It clears at kStaminaRecoveredToSprint (25%), which is
    // below this threshold, so on its own it would stop the breathing while the
    // meter was still visibly low; together they mean "low OR still spent".
    const bool winded = onFoot &&
        (g_staminaExhausted || g_staminaSeconds < kStaminaBreathingSeconds);
    if (winded && g_breathingCooldown <= 0.0f) {
        g_breathingCooldown = kBreathingRepeatSeconds;
        // Slight pitch jitter around the pitched-down base, so a long recovery
        // does not repeat one identical breath. Centred on kBreathingPitch
        // rather than on unity, or the jitter would undo the tuning above.
        const float pitch = kBreathingPitch + (std::rand() % 100) * 0.0008f;
        g_breathingAudio.Play(kBreathingGain, pitch);
    }

    // --- Enemies ---
    if (!g_banditLoaded) return;
    for (auto& bandit : g_bandits) {
        if (!bandit || bandit->Dead()) continue;
        // Rappelling and mounted actors are not walking.
        if (bandit->Rappelling() || bandit->turretGunner) continue;

        const XMFLOAT3 position = bandit->position;
        if (!bandit->stepTrackingStarted) {
            bandit->lastStepPosition = position;
            bandit->stepTrackingStarted = true;
            continue;
        }
        const float dx = position.x - bandit->lastStepPosition.x;
        const float dz = position.z - bandit->lastStepPosition.z;
        bandit->lastStepPosition = position;
        const float moved = std::sqrt(dx * dx + dz * dz);
        // A spawn or a teleport is not a stride. Anything past a sprint's worth
        // of movement in one frame is discarded rather than counted.
        if (moved > 1.2f) continue;
        bandit->stepDistance += moved;
        if (bandit->stepDistance < kStepStrideWalk) continue;
        bandit->stepDistance = 0.0f;

        // Shorter range than gunfire: a footstep should be a close-quarters
        // tell, not something heard across the island.
        //
        // Six times quieter than the player's own boots. Their steps are at the
        // listener and should dominate; another man's are a faint scuff that
        // has to be listened for, which is what makes them worth listening for.
        // Positional, so that scuff also says which side he is on.
        PlayFootstep(0.11f, true, position, 22.0f);
    }
}
