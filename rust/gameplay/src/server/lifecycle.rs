// Lifecycle transitions driven by network events. Port of the
// quic.ClientConnected / quic.ClientDisconnected handlers in
// csharp_arch_server/Program.cs (lines 82–102) plus the Resetting branch of
// the main loop (lines 162–171).
//
// Transitions:
//   Idle      → Running   first client connects (and AutoStartMatch fires KickoffMatch)
//   Running   → Resetting last client disconnects
//   Resetting → Idle      one-frame: wipe enemies, clear history, reseed RNG

use std::f32::consts::PI;

use avian2d::prelude::*;
use bevy::prelude::*;

use crate::game::constants::{PLAYER_LINEAR_DAMPING, PLAYER_RADIUS};
use crate::game::sim::components::{
    Aim2D, Alive, DisableTimer, Enemy, FireCooldown, FireEvents, Lifecycle, NextEnemyId,
    PendingFires, Player, PlayerRtt, PlayerSlot, RoundState, Seed, SimRng, WorldClock,
};
use crate::game::sim::rewind::RewindRing;
use crate::game::spatial::Pos2D;
use crate::game::types::RoundPhase;
use crate::game::DeterministicRng;
use crate::game::INTER_ROUND_DELAY_SEC;

use super::capture::SnapshotScratch;
use super::history::WorldStateHistory;
use super::recipient::RecipientStates;

/// Spawn a player entity at `(cos θ, sin θ) · ArenaRadius·0.3` where θ depends
/// on slot id. Mirror of Program.cs::SpawnPlayer.
pub fn spawn_player(commands: &mut Commands, slot: u8) {
    let angle = slot as f32 / crate::game::MAX_PLAYERS as f32 * PI * 2.0;
    let radius = crate::game::ARENA_RADIUS * 0.3;
    let pos = Vec2::new(angle.cos(), angle.sin()) * radius;
    commands.spawn((
        Player,
        PlayerSlot(slot),
        Pos2D { x: pos.x, y: pos.y },
        Aim2D(Vec2::new(1.0, 0.0)),
        FireCooldown(0.0),
        DisableTimer(0.0),
        Alive(true),
        // Kinematic body — player position is driven directly by client input
        // (see movement::player_input) but the collider still participates in
        // physics so enemy bodies push against it during collision resolution.
        RigidBody::Kinematic,
        Collider::circle(PLAYER_RADIUS),
        Position(pos),
        LinearVelocity(Vec2::ZERO),
        LinearDamping(PLAYER_LINEAR_DAMPING),
        LockedAxes::ROTATION_LOCKED,
    ));
}

/// Set RoundState to a 3 s InterRound countdown so Round 1 starts shortly.
pub fn kickoff_match(round: &mut RoundState) {
    round.phase = RoundPhase::InterRound;
    round.round = 0;
    round.inter_round_timer = INTER_ROUND_DELAY_SEC;
    round.round_timer = 0.0;
    round.spawn_target = 0;
    round.spawns_remaining = 0;
    round.spawn_interval = 0.0;
    round.spawn_accumulator = 0.0;
}

/// On `ClientDisconnected`: despawn that slot's Player entity, clear its
/// recipient state, and — if the server is now empty — start a Resetting tick.
pub fn handle_client_disconnect(
    mut events: MessageReader<crate::network::ClientDisconnected>,
    mut commands: Commands,
    mut lifecycle: ResMut<Lifecycle>,
    mut recipients: ResMut<RecipientStates>,
    mut slots: ResMut<crate::network::PlayerSlots>,
    players: Query<(Entity, &PlayerSlot), With<Player>>,
    all_players: Query<&Player>,
    clock: Res<WorldClock>,
) {
    for ev in events.read() {
        let Some(pid) = slots.client_slot(ev.client) else {
            continue;
        };
        slots.release(ev.client);
        recipients.clear_slot(pid);

        let mut despawned_any = false;
        for (entity, slot) in players.iter() {
            if slot.0 == pid {
                commands.entity(entity).despawn();
                despawned_any = true;
            }
        }
        info!(
            "[server] disconnect slot {} (despawned={}) → players={}",
            pid,
            despawned_any,
            all_players.iter().count().saturating_sub(usize::from(despawned_any))
        );

        // After this frame the despawn will land; we just need to know whether
        // this was the LAST player. all_players still includes the to-despawn
        // entity, so check `count - 1` if we just queued one.
        let remaining = all_players
            .iter()
            .count()
            .saturating_sub(usize::from(despawned_any));
        if remaining == 0 && *lifecycle == Lifecycle::Running {
            *lifecycle = Lifecycle::Resetting;
            info!(
                "[server] state: Running → Resetting tick={} (last client gone)",
                clock.0
            );
        }
    }
}

/// On `Lifecycle == Resetting`: wipe enemies, clear scratch + history, reset
/// RNG to the original seed, drop to Idle.
#[allow(clippy::too_many_arguments)]
pub fn handle_resetting(
    mut lifecycle: ResMut<Lifecycle>,
    mut commands: Commands,
    mut clock: ResMut<WorldClock>,
    mut round: ResMut<RoundState>,
    mut rng: ResMut<SimRng>,
    mut next_enemy_id: ResMut<NextEnemyId>,
    mut inputs: ResMut<crate::game::sim::components::InputInbox>,
    mut pending_fires: ResMut<PendingFires>,
    mut fires: ResMut<FireEvents>,
    mut processed: ResMut<crate::game::sim::components::ProcessedInputTick>,
    mut rtt: ResMut<PlayerRtt>,
    mut rewind: ResMut<RewindRing>,
    mut history: ResMut<WorldStateHistory>,
    mut scratch: ResMut<SnapshotScratch>,
    seed: Res<Seed>,
    enemies: Query<Entity, With<Enemy>>,
) {
    if *lifecycle != Lifecycle::Resetting {
        return;
    }

    for e in enemies.iter() {
        commands.entity(e).despawn();
    }

    clock.0 = 0;
    *round = RoundState::default();
    rng.0 = DeterministicRng::from_seed(seed.0);
    next_enemy_id.0 = 0;
    inputs.0.clear();
    pending_fires.0.clear();
    fires.0.clear();
    processed.0 = [0; crate::game::MAX_PLAYERS];
    rtt.0 = [60.0; crate::game::MAX_PLAYERS];
    rewind.clear();
    history.clear();
    scratch.0.clear();

    *lifecycle = Lifecycle::Idle;
    info!(
        "[server] state: Resetting → Idle (RNG reseeded to 0x{:X})",
        seed.0
    );
}
