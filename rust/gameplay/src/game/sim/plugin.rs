// SimPlugin — wires the authoritative game loop into Bevy's FixedUpdate at the
// 62.5 Hz sim rate, in the locked WireFormat.md §8.2 sub-step order. The sim
// only runs while the server lifecycle is `Running`.
//
// SimSet declares all nine sub-steps up front (chained) so the rewind (Phase 3)
// and snapshot (Phase 4) systems slot into their reserved sets without
// reordering anything.

use bevy::prelude::*;
use bevy::time::Fixed;

use super::components::*;
use super::movement::{enemy_integrate, enemy_seek, player_enemy_contact, player_input, tick_advance};
use super::rewind::{rewind_record, rewind_resolve, RewindRing};
use super::round::round_director;
use super::DeterministicRng;
use crate::game::constants::{SNAPSHOT_HISTORY_TICKS, TICKS_PER_SECOND};

#[derive(SystemSet, Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum SimSet {
    TickAdvance,
    RoundDirector,
    PlayerInput,
    RewindResolve,
    EnemySeek,
    EnemyIntegrate,
    PlayerContact,
    RewindRecord,
    Snapshot,
}

pub struct SimPlugin {
    pub seed: u64,
    pub god_mode: bool,
}

impl Default for SimPlugin {
    fn default() -> Self {
        Self {
            seed: 0xC0FF_EEF0_0D,
            god_mode: false,
        }
    }
}

impl Plugin for SimPlugin {
    fn build(&self, app: &mut App) {
        app.insert_resource(WorldClock(0))
            .insert_resource(RoundState::default())
            .insert_resource(SimRng(DeterministicRng::from_seed(self.seed)))
            .insert_resource(NextEnemyId(0))
            .insert_resource(GodMode(self.god_mode))
            .insert_resource(Seed(self.seed))
            .insert_resource(Lifecycle::default())
            .insert_resource(InputInbox::default())
            .insert_resource(PendingFires::default())
            .insert_resource(FireEvents::default())
            .insert_resource(ProcessedInputTick::default())
            .insert_resource(PlayerRtt::default())
            .insert_resource(RewindRing::new(SNAPSHOT_HISTORY_TICKS))
            .insert_resource(Time::<Fixed>::from_hz(TICKS_PER_SECOND as f64));

        app.configure_sets(
            FixedUpdate,
            (
                SimSet::TickAdvance,
                SimSet::RoundDirector,
                SimSet::PlayerInput,
                SimSet::RewindResolve,
                SimSet::EnemySeek,
                SimSet::EnemyIntegrate,
                SimSet::PlayerContact,
                SimSet::RewindRecord,
                SimSet::Snapshot,
            )
                .chain()
                .run_if(sim_running),
        );

        app.add_systems(
            FixedUpdate,
            (
                tick_advance.in_set(SimSet::TickAdvance),
                round_director.in_set(SimSet::RoundDirector),
                player_input.in_set(SimSet::PlayerInput),
                rewind_resolve.in_set(SimSet::RewindResolve),
                enemy_seek.in_set(SimSet::EnemySeek),
                enemy_integrate.in_set(SimSet::EnemyIntegrate),
                player_enemy_contact.in_set(SimSet::PlayerContact),
                rewind_record.in_set(SimSet::RewindRecord),
            ),
        );
    }
}

fn sim_running(state: Res<Lifecycle>) -> bool {
    *state == Lifecycle::Running
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::game::constants::*;
    use crate::game::spatial::{Pos2D, Vel2D};
    use crate::game::types::{InputCmd, RoundPhase};

    fn app(seed: u64, god: bool) -> App {
        let mut app = App::new();
        app.add_plugins(SimPlugin {
            seed,
            god_mode: god,
        });
        app.insert_resource(Lifecycle::Running);
        app
    }

    fn tick(app: &mut App) {
        app.world_mut().run_schedule(FixedUpdate);
    }

    fn spawn_player(app: &mut App, slot: u8, x: f32, y: f32) {
        app.world_mut().spawn((
            Player,
            PlayerSlot(slot),
            Pos2D { x, y },
            Aim2D(Vec2::new(1.0, 0.0)),
            FireCooldown(0.0),
            DisableTimer(0.0),
            Alive(true),
        ));
    }

    fn spawn_enemy(app: &mut App, id: u16, x: f32, y: f32) {
        app.world_mut()
            .spawn((Enemy, EnemyId(id), Pos2D { x, y }, Vel2D { x: 0.0, y: 0.0 }));
    }

    fn kickoff(app: &mut App) {
        let mut rs = app.world_mut().resource_mut::<RoundState>();
        rs.phase = RoundPhase::InterRound;
        rs.round = 0;
        rs.inter_round_timer = INTER_ROUND_DELAY_SEC;
        rs.round_timer = 0.0;
    }

    fn enemy_count(app: &mut App) -> usize {
        let world = app.world_mut();
        world.query_filtered::<(), With<Enemy>>().iter(world).count()
    }

    fn player_disable(app: &mut App, slot: u8) -> f32 {
        let world = app.world_mut();
        let mut q = world.query_filtered::<(&PlayerSlot, &DisableTimer), With<Player>>();
        for (s, d) in q.iter(world) {
            if s.0 == slot {
                return d.0;
            }
        }
        panic!("no player with slot {slot}");
    }

    /// Sorted (id, pos_x_mm, pos_y_mm) of all live enemies — quantized so the
    /// comparison is robust to sub-mm float noise.
    fn enemy_snapshot(app: &mut App) -> Vec<(u16, i32, i32)> {
        let world = app.world_mut();
        let mut q = world.query_filtered::<(&EnemyId, &Pos2D), With<Enemy>>();
        let mut v: Vec<(u16, i32, i32)> = q
            .iter(world)
            .map(|(id, p)| (id.0, (p.x * 1000.0).round() as i32, (p.y * 1000.0).round() as i32))
            .collect();
        v.sort_unstable();
        v
    }

    #[test]
    fn clock_advances_each_tick() {
        let mut a = app(1, false);
        tick(&mut a);
        assert_eq!(a.world().resource::<WorldClock>().0, 1);
        tick(&mut a);
        assert_eq!(a.world().resource::<WorldClock>().0, 2);
    }

    #[test]
    fn idle_does_not_tick() {
        let mut a = app(1, false);
        *a.world_mut().resource_mut::<Lifecycle>() = Lifecycle::Idle;
        tick(&mut a);
        assert_eq!(a.world().resource::<WorldClock>().0, 0);
    }

    #[test]
    fn interround_starts_round_one() {
        let mut a = app(1, false);
        spawn_player(&mut a, 0, 0.0, 0.0);
        kickoff(&mut a);
        let ticks = (INTER_ROUND_DELAY_SEC / SIM_DT).ceil() as i32 + 1;
        for _ in 0..ticks {
            tick(&mut a);
        }
        let rs = *a.world().resource::<RoundState>();
        assert_eq!(rs.phase, RoundPhase::InRound);
        assert_eq!(rs.round, 1);
        assert_eq!(rs.spawn_target, BASE_ENEMIES_PER_ROUND as i32); // round 1 → 700
    }

    #[test]
    fn target_enemies_scaling() {
        use super::super::round::target_enemies_for_round;
        assert_eq!(target_enemies_for_round(0), 0);
        assert_eq!(target_enemies_for_round(1), 700);
        assert_eq!(target_enemies_for_round(2), 980); // 700 * 1.4
    }

    #[test]
    fn spawns_are_deterministic_and_seed_sensitive() {
        let run = |seed: u64| {
            let mut a = app(seed, true); // god mode: avoid contact/reset noise
            spawn_player(&mut a, 0, 0.0, 0.0);
            kickoff(&mut a);
            for _ in 0..400 {
                tick(&mut a);
            }
            enemy_snapshot(&mut a)
        };
        let a1 = run(42);
        let a2 = run(42);
        assert!(!a1.is_empty(), "expected some enemies to have spawned");
        assert_eq!(a1, a2, "same seed must reproduce identical enemy state");
        let b = run(43);
        assert_ne!(a1, b, "different seed must change spawn placement");
    }

    #[test]
    fn contact_disables_player() {
        let mut a = app(1, false);
        spawn_player(&mut a, 0, 0.0, 0.0);
        spawn_enemy(&mut a, 1, 0.0, 0.0);
        tick(&mut a);
        assert!((player_disable(&mut a, 0) - DISABLE_DURATION_SEC).abs() < 1e-3);
    }

    #[test]
    fn god_mode_blocks_disable() {
        let mut a = app(1, true);
        spawn_player(&mut a, 0, 0.0, 0.0);
        spawn_enemy(&mut a, 1, 0.0, 0.0);
        tick(&mut a);
        assert_eq!(player_disable(&mut a, 0), 0.0);
    }

    #[test]
    fn player_moves_with_input() {
        let mut a = app(1, false);
        spawn_player(&mut a, 0, 0.0, 0.0);
        {
            let mut inbox = a.world_mut().resource_mut::<InputInbox>();
            let mut cmd = InputCmd::empty(1);
            cmd.move_x = 1.0;
            cmd.move_y = 0.0;
            inbox.0.push((0, cmd));
        }
        tick(&mut a);
        let world = a.world_mut();
        let mut q = world.query_filtered::<&Pos2D, With<Player>>();
        let p = q.iter(world).next().unwrap();
        assert!(
            (p.x - PLAYER_SPEED * SIM_DT).abs() < 1e-4,
            "x={} expected {}",
            p.x,
            PLAYER_SPEED * SIM_DT
        );
        assert!(p.y.abs() < 1e-5);
    }

    #[test]
    fn single_player_overrun_triggers_reset() {
        // One player surrounded: enemies reach it, it gets disabled, and
        // "all connected players disabled" wipes enemies + restarts at round 1
        // via InterRound. Verifies the team-wipe path end to end.
        let mut a = app(7, false);
        spawn_player(&mut a, 0, 0.0, 0.0);
        // Drop an enemy right on the player so contact fires immediately.
        spawn_enemy(&mut a, 1, 0.0, 0.0);
        // Force InRound so the reset branch is reachable.
        {
            let mut rs = a.world_mut().resource_mut::<RoundState>();
            rs.phase = RoundPhase::InRound;
            rs.round = 3;
        }
        tick(&mut a); // contact disables the player
        tick(&mut a); // round_director sees all-disabled → reset
        let rs = *a.world().resource::<RoundState>();
        assert_eq!(rs.phase, RoundPhase::InterRound);
        assert_eq!(rs.round, 0);
        assert_eq!(enemy_count(&mut a), 0, "team wipe must clear enemies");
        // Player was re-enabled.
        assert_eq!(player_disable(&mut a, 0), 0.0);
    }
}