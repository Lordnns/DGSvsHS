// Player input application + enemy AI/movement/contact. Ports of the DOTS
// systems PlayerInputSystem, EnemySeekSystem, EnemyIntegrateSystem,
// PlayerEnemyContactSystem (WireFormat.md §8.2 steps 3, 5, 6, 7).

use bevy::prelude::*;

use super::components::*;
use crate::game::constants::*;
use crate::game::spatial::{Pos2D, Vel2D};

const AIM_EPS_SQ: f32 = 0.0001;

/// Step 1: advance the world tick and clear the per-tick fire-event accumulator.
pub fn tick_advance(mut clock: ResMut<WorldClock>, mut fires: ResMut<FireEvents>) {
    clock.0 += 1;
    fires.0.clear();
}

/// Step 3: apply the latest input per player (move/aim/cooldown/disable decay)
/// and queue fire commands for the rewind resolver.
///
/// Parity note: the reference server queues a fire for every Fire input of an
/// *alive* player and does NOT gate on cooldown or disable — fire cadence is
/// enforced client-side. We mirror that exactly (even though the gameplay doc
/// says fire is blocked while disabled). Fire origin is the player's position
/// at the START of the tick (before this tick's movement is applied).
pub fn player_input(
    mut inbox: ResMut<InputInbox>,
    mut pending_fires: ResMut<PendingFires>,
    mut processed: ResMut<ProcessedInputTick>,
    mut players: Query<
        (
            &PlayerSlot,
            &mut Pos2D,
            &mut Aim2D,
            &mut FireCooldown,
            &mut DisableTimer,
            &Alive,
        ),
        With<Player>,
    >,
) {
    // Latest input per slot (highest tick wins).
    let mut latest: [Option<crate::game::types::InputCmd>; MAX_PLAYERS] = [None; MAX_PLAYERS];
    for (slot, cmd) in inbox.0.iter() {
        let s = *slot as usize;
        if s >= MAX_PLAYERS {
            continue;
        }
        if latest[s].map_or(true, |l| cmd.tick > l.tick) {
            latest[s] = Some(*cmd);
        }
    }

    // Pre-movement snapshot per slot for fire origins.
    let mut snap: [Option<(Vec2, Vec2, bool)>; MAX_PLAYERS] = [None; MAX_PLAYERS];
    for (slot, pos, aim, _cd, _dt, alive) in players.iter() {
        let s = slot.0 as usize;
        if s < MAX_PLAYERS {
            snap[s] = Some((pos_vec(pos), aim.0, alive.0));
        }
    }

    // Queue fires: iterate every buffered Fire input (not just the latest).
    for (slot, cmd) in inbox.0.iter() {
        if !cmd.fire() {
            continue;
        }
        let s = *slot as usize;
        if s >= MAX_PLAYERS {
            continue;
        }
        let Some((pos, aim, alive)) = snap[s] else {
            continue;
        };
        if !alive {
            continue;
        }
        let in_aim = Vec2::new(cmd.aim_x, cmd.aim_y);
        let dir = if in_aim.length_squared() > AIM_EPS_SQ {
            in_aim.normalize()
        } else {
            aim
        };
        pending_fires.0.push(PendingFire {
            player_id: *slot,
            client_input_tick: cmd.tick,
            origin: pos,
            dir,
        });
    }

    // Apply movement / aim / timers.
    let max_r = ARENA_RADIUS - PLAYER_RADIUS;
    for (slot, mut pos, mut aim, mut cd, mut dt, alive) in players.iter_mut() {
        let s = slot.0 as usize;
        let cmd = if alive.0 && s < MAX_PLAYERS {
            latest[s]
        } else {
            None
        };

        if let Some(cmd) = cmd {
            // Movement — clamp magnitude so diagonals don't exceed PlayerSpeed.
            let mut mv = Vec2::new(cmd.move_x, cmd.move_y);
            let mag = mv.length();
            if mag > 1.0 {
                mv /= mag;
            }
            let mut new_pos = pos_vec(&pos) + mv * PLAYER_SPEED * SIM_DT;
            let r = new_pos.length();
            if r > max_r {
                new_pos *= max_r / r;
            }
            pos.x = new_pos.x;
            pos.y = new_pos.y;

            let in_aim = Vec2::new(cmd.aim_x, cmd.aim_y);
            if in_aim.length_squared() > AIM_EPS_SQ {
                aim.0 = in_aim.normalize();
            }
            processed.0[s] = cmd.tick;
        }

        cd.0 = (cd.0 - SIM_DT).max(0.0);
        dt.0 = (dt.0 - SIM_DT).max(0.0);
    }

    inbox.0.clear();
}

/// Step 5: each enemy heads toward the nearest alive, non-disabled player.
pub fn enemy_seek(
    players: Query<(&PlayerSlot, &Pos2D, &Alive, &DisableTimer), With<Player>>,
    mut enemies: Query<(&Pos2D, &mut Vel2D), With<Enemy>>,
) {
    // Build target list in slot order so the (vanishingly rare) exact-distance
    // tie-break is deterministic across runs/builds.
    let mut targets: Vec<(u8, Vec2)> = players
        .iter()
        .filter(|(_, _, alive, dt)| alive.0 && dt.0 <= 0.0)
        .map(|(slot, p, _, _)| (slot.0, pos_vec(p)))
        .collect();
    targets.sort_by_key(|(slot, _)| *slot);
    let targets: Vec<Vec2> = targets.into_iter().map(|(_, p)| p).collect();

    for (pos, mut vel) in enemies.iter_mut() {
        if targets.is_empty() {
            vel.x = 0.0;
            vel.y = 0.0;
            continue;
        }
        let p = pos_vec(pos);
        let mut best = targets[0];
        let mut best_sq = f32::MAX;
        for t in &targets {
            let sq = (*t - p).length_squared();
            if sq < best_sq {
                best_sq = sq;
                best = *t;
            }
        }
        let mut dir = best - p;
        let len = best_sq.sqrt();
        if len > 0.0001 {
            dir /= len;
        } else {
            dir = Vec2::ZERO;
        }
        let v = dir * ENEMY_SPEED;
        vel.x = v.x;
        vel.y = v.y;
    }
}

/// Step 6: integrate enemy positions.
pub fn enemy_integrate(mut enemies: Query<(&mut Pos2D, &Vel2D), With<Enemy>>) {
    for (mut pos, vel) in enemies.iter_mut() {
        pos.x += vel.x * SIM_DT;
        pos.y += vel.y * SIM_DT;
    }
}

/// Step 7: alive, non-disabled player touching any enemy → disabled for
/// DisableDurationSec. Skipped entirely in God Mode.
pub fn player_enemy_contact(
    god: Res<GodMode>,
    enemies: Query<&Pos2D, With<Enemy>>,
    mut players: Query<(&Pos2D, &mut DisableTimer, &Alive), With<Player>>,
) {
    if god.0 {
        return;
    }
    let kill_r = PLAYER_KILL_RADIUS + ENEMY_RADIUS;
    let kill_r_sq = kill_r * kill_r;

    let enemy_pos: Vec<Vec2> = enemies.iter().map(pos_vec).collect();

    for (pos, mut dt, alive) in players.iter_mut() {
        if !alive.0 || dt.0 > 0.0 {
            continue;
        }
        let p = pos_vec(pos);
        for e in &enemy_pos {
            if (*e - p).length_squared() <= kill_r_sq {
                dt.0 = DISABLE_DURATION_SEC;
                break;
            }
        }
    }
}