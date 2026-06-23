// Round director + wave spawning. Port of
// DGSvsHS/Assets/_Game/Server/Dots/Systems/RoundDirectorSystem.cs.
//
// Implemented as an EXCLUSIVE system (&mut World) on purpose: the DOTS version
// creates/destroys enemy entities immediately (structural changes visible to
// later systems the same tick, and to its own in-tick enemy-count check).
// Deferred `Commands` would materialize a tick late and break both the
// round-end count and the "spawn-then-wipe same tick" reset semantics. Direct
// world mutation reproduces the reference exactly.

use bevy::prelude::*;
use std::f32::consts::TAU;

use super::components::*;
use super::DeterministicRng;
use crate::game::constants::*;
use crate::game::spatial::{Pos2D, Vel2D};
use crate::game::types::RoundPhase;

pub fn round_director(world: &mut World) {
    // --- gather player snapshots (entity, alive, disable_timer) ---
    let players: Vec<(Entity, bool, f32)> = world
        .query_filtered::<(Entity, &Alive, &DisableTimer), With<Player>>()
        .iter(world)
        .map(|(e, a, d)| (e, a.0, d.0))
        .collect();
    let total_players = players.len();

    let mut round = *world.resource::<RoundState>();

    // No clients connected — drop straight back to idle (matches DOTS).
    if total_players == 0 && round.phase != RoundPhase::PreGame {
        round.phase = RoundPhase::PreGame;
        round.round = 0;
        round.round_timer = 0.0;
        round.inter_round_timer = 0.0;
        round.spawns_remaining = 0;
        round.spawn_target = 0;
        *world.resource_mut::<RoundState>() = round;
        return;
    }

    // Live enemy entities (collected before this tick's spawns, like the DOTS
    // immediate-count path: at round end no spawn happens, so this is exact).
    let enemy_ents: Vec<Entity> = world
        .query_filtered::<Entity, With<Enemy>>()
        .iter(world)
        .collect();

    let mut rng = *world.resource::<SimRng>();
    let mut next_id = world.resource::<NextEnemyId>().0;

    // Decisions accumulated against local copies; applied to the world after.
    let mut spawns: Vec<(u16, f32, f32)> = Vec::new();
    let mut despawn_all = false;
    let mut reenable_players = false;

    match round.phase {
        RoundPhase::PreGame => {}

        RoundPhase::InterRound => {
            round.inter_round_timer -= SIM_DT;
            if round.inter_round_timer <= 0.0 {
                round.round += 1;
                if round.round > TOTAL_ROUNDS as i32 {
                    round.phase = RoundPhase::Victory;
                } else {
                    round.phase = RoundPhase::InRound;
                    round.round_timer = 0.0;
                    start_wave(&mut round);
                }
            }
        }

        RoundPhase::InRound => {
            round.round_timer += SIM_DT;
            tick_wave(&mut round, &mut rng.0, &mut next_id, &mut spawns);

            if all_connected_disabled(&players) {
                // Team wipe → fresh round 1 via InterRound. Enemies queued this
                // tick are dropped (DOTS spawns then destroys them; net effect
                // is the same, and the RNG was still consumed).
                reset_round_state(&mut round);
                despawn_all = true;
                reenable_players = true;
                spawns.clear();
            } else if round.spawns_remaining == 0 && enemy_ents.is_empty() {
                round.phase = RoundPhase::InterRound;
                round.inter_round_timer = INTER_ROUND_DELAY_SEC;
            }
        }

        RoundPhase::Victory | RoundPhase::Defeat => {}
    }

    // --- apply ---
    *world.resource_mut::<RoundState>() = round;
    world.resource_mut::<SimRng>().0 = rng.0; // RNG advanced even if spawns dropped
    world.resource_mut::<NextEnemyId>().0 = next_id;

    if despawn_all {
        for e in enemy_ents {
            world.despawn(e);
        }
    } else {
        for (id, x, y) in spawns {
            world.spawn((
                Enemy,
                EnemyId(id),
                Pos2D { x, y },
                Vel2D { x: 0.0, y: 0.0 },
            ));
        }
    }

    if reenable_players {
        let ents: Vec<Entity> = players.iter().map(|(e, _, _)| *e).collect();
        for e in ents {
            if let Some(mut a) = world.get_mut::<Alive>(e) {
                a.0 = true;
            }
            if let Some(mut d) = world.get_mut::<DisableTimer>(e) {
                d.0 = 0.0;
            }
            if let Some(mut c) = world.get_mut::<FireCooldown>(e) {
                c.0 = 0.0;
            }
        }
    }
}

fn start_wave(round: &mut RoundState) {
    let target = target_enemies_for_round(round.round);
    round.spawn_target = target;
    round.spawns_remaining = target;
    round.spawn_interval = ROUND_SPAWN_WINDOW_SEC / target.max(1) as f32;
    round.spawn_accumulator = 0.0;
}

/// `round(BaseEnemiesPerRound × EnemyScalingPerRound^(r-1))`.
/// Uses round-half-to-even to match C# `Mathf.RoundToInt` / `math.round`.
pub fn target_enemies_for_round(for_round: i32) -> i32 {
    if for_round < 1 {
        return 0;
    }
    let scaled = BASE_ENEMIES_PER_ROUND as f32 * ENEMY_SCALING_PER_ROUND.powf((for_round - 1) as f32);
    scaled.round_ties_even() as i32
}

fn tick_wave(
    round: &mut RoundState,
    rng: &mut DeterministicRng,
    next_id: &mut u16,
    spawns: &mut Vec<(u16, f32, f32)>,
) {
    if round.spawns_remaining <= 0 {
        return;
    }
    round.spawn_accumulator += SIM_DT;
    while round.spawn_accumulator >= round.spawn_interval && round.spawns_remaining > 0 {
        round.spawn_accumulator -= round.spawn_interval;
        spawn_one_enemy(rng, next_id, spawns);
        round.spawns_remaining -= 1;
    }
}

fn spawn_one_enemy(rng: &mut DeterministicRng, next_id: &mut u16, spawns: &mut Vec<(u16, f32, f32)>) {
    let angle = rng.next_range(0.0, TAU);
    let r = ARENA_RADIUS - ENEMY_RADIUS - 0.1;
    let (s, c) = (angle.sin(), angle.cos());
    spawns.push((*next_id, c * r, s * r));
    *next_id = next_id.wrapping_add(1);
}

fn all_connected_disabled(players: &[(Entity, bool, f32)]) -> bool {
    let mut total = 0;
    let mut disabled = 0;
    for (_, alive, dt) in players {
        if !*alive {
            continue;
        }
        total += 1;
        if *dt > 0.0 {
            disabled += 1;
        }
    }
    total > 0 && disabled == total
}

fn reset_round_state(round: &mut RoundState) {
    round.round = 0;
    round.phase = RoundPhase::InterRound;
    round.inter_round_timer = INTER_ROUND_DELAY_SEC;
    round.round_timer = 0.0;
    round.spawn_target = 0;
    round.spawns_remaining = 0;
    round.spawn_interval = 0.0;
    round.spawn_accumulator = 0.0;
}