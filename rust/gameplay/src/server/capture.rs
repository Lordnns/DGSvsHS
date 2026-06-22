// Snapshot capture + history recording. Port of
// csharp_arch_server/Server/Systems/SnapshotCapture.cs + the history.Record(...)
// call in csharp_arch_server/Program.cs after each sim tick.

use bevy::prelude::*;

use crate::game::sim::components::{
    Alive, DisableTimer, Enemy, EnemyId, FireCooldown, FireEvents, Player, PlayerRtt, PlayerSlot,
    WorldClock,
};
use crate::game::sim::components::{Aim2D, RoundState};
use crate::game::spatial::Pos2D;
use crate::game::{EnemySnap, PlayerSnap, Snapshot, SnapshotKind};

use super::history::WorldStateHistory;

/// The single per-tick snapshot scratch buffer. Built by `snapshot_capture`,
/// consumed by `record_history` and the broadcast system.
#[derive(Resource)]
pub struct SnapshotScratch(pub Snapshot);

impl Default for SnapshotScratch {
    fn default() -> Self {
        Self(Snapshot::with_capacity())
    }
}

/// Build a full snapshot of the current world into `SnapshotScratch.0`.
pub fn snapshot_capture(
    mut scratch: ResMut<SnapshotScratch>,
    clock: Res<WorldClock>,
    round: Res<RoundState>,
    fires: Res<FireEvents>,
    players: Query<
        (
            &PlayerSlot,
            &Pos2D,
            &Aim2D,
            &FireCooldown,
            &DisableTimer,
            &Alive,
        ),
        With<Player>,
    >,
    enemies: Query<(&EnemyId, &Pos2D), With<Enemy>>,
    _rtt: Res<PlayerRtt>,
) {
    let s = &mut scratch.0;
    s.clear();
    s.kind = SnapshotKind::Full;
    s.tick = clock.0;
    s.last_processed_input_tick = 0;
    s.round = round.round.max(0) as u16;
    s.round_timer = round.round_timer;
    s.inter_round_timer = round.inter_round_timer;
    s.phase = round.phase;

    for (slot, pos, aim, _cd, dt, alive) in players.iter() {
        s.players.push(PlayerSnap {
            id: slot.0,
            pos_x: pos.x,
            pos_y: pos.y,
            aim_x: aim.0.x,
            aim_y: aim.0.y,
            alive: alive.0,
            disable_timer: dt.0,
        });
    }

    let mut enemy_count = 0u32;
    for (id, pos) in enemies.iter() {
        s.enemies.push(EnemySnap {
            id: id.0,
            pos_x: pos.x,
            pos_y: pos.y,
        });
        enemy_count += 1;
    }
    s.enemy_total_in_world = enemy_count;

    s.recent_fire_events.extend_from_slice(&fires.0);
}

/// Mirror of `history.Record(snapshotScratch)` in Program.cs main loop.
pub fn record_history(scratch: Res<SnapshotScratch>, mut history: ResMut<WorldStateHistory>) {
    history.record(&scratch.0);
}
