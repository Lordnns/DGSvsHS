// ECS components + resources for the authoritative sim. Mirror of
// DGSvsHS/Assets/_Game/Server/Dots/Components.cs, adapted to Bevy idioms.
//
// Position/velocity reuse the existing spatial-grid component types (Pos2D /
// Vel2D) so the grid can index enemies directly in later phases.

#![allow(dead_code)]

use bevy::prelude::*;

use super::DeterministicRng;
use crate::game::constants::MAX_PLAYERS;
use crate::game::spatial::{Pos2D, Vel2D};
use crate::game::types::{FireEvent, InputCmd, RoundPhase};

// ---------- Enemy archetype ----------

#[derive(Component)]
pub struct Enemy;

#[derive(Component, Copy, Clone, Debug)]
pub struct EnemyId(pub u16);

// ---------- Player archetype ----------

#[derive(Component)]
pub struct Player;

#[derive(Component, Copy, Clone, Debug)]
pub struct PlayerSlot(pub u8);

#[derive(Component, Copy, Clone, Debug)]
pub struct Aim2D(pub Vec2);

#[derive(Component, Copy, Clone, Debug, Default)]
pub struct FireCooldown(pub f32);

#[derive(Component, Copy, Clone, Debug, Default)]
pub struct DisableTimer(pub f32);

impl DisableTimer {
    pub fn is_disabled(&self) -> bool {
        self.0 > 0.0
    }
}

#[derive(Component, Copy, Clone, Debug)]
pub struct Alive(pub bool);

// ---------- Sim-global resources (one of each) ----------

#[derive(Resource, Default)]
pub struct WorldClock(pub u32);

#[derive(Resource, Copy, Clone, Debug)]
pub struct RoundState {
    pub round: i32,
    pub phase: RoundPhase,
    pub round_timer: f32,
    pub inter_round_timer: f32,
    pub spawn_target: i32,
    pub spawns_remaining: i32,
    pub spawn_interval: f32,
    pub spawn_accumulator: f32,
}

impl Default for RoundState {
    fn default() -> Self {
        Self {
            round: 0,
            phase: RoundPhase::PreGame,
            round_timer: 0.0,
            inter_round_timer: 0.0,
            spawn_target: 0,
            spawns_remaining: 0,
            spawn_interval: 0.0,
            spawn_accumulator: 0.0,
        }
    }
}

#[derive(Resource, Copy, Clone)]
pub struct SimRng(pub DeterministicRng);

#[derive(Resource, Default, Copy, Clone)]
pub struct NextEnemyId(pub u16);

#[derive(Resource, Default, Copy, Clone)]
pub struct GodMode(pub bool);

#[derive(Resource, Copy, Clone)]
pub struct Seed(pub u64);

/// Server lifecycle state machine (mirror of DedicatedServerMain.ServerLifecycle).
/// A plain resource rather than bevy_state (that feature isn't enabled here).
#[derive(Resource, Copy, Clone, PartialEq, Eq, Debug, Default)]
pub enum Lifecycle {
    Booting,
    #[default]
    Idle,
    Running,
    Resetting,
    ShuttingDown,
}

// ---------- Input + fire bridge ----------

/// Inputs accepted from the transport since the last tick, tagged with the
/// owning player slot. Drained by `player_input` each tick.
#[derive(Resource, Default)]
pub struct InputInbox(pub Vec<(u8, InputCmd)>);

/// Highest input tick actually applied per player — feeds the per-recipient
/// `last_processed_input_tick` snapshot field in a later phase.
#[derive(Resource)]
pub struct ProcessedInputTick(pub [u32; MAX_PLAYERS]);

impl Default for ProcessedInputTick {
    fn default() -> Self {
        Self([0; MAX_PLAYERS])
    }
}

/// A fire command queued by `player_input`, resolved by the rewind system.
#[derive(Copy, Clone, Debug)]
pub struct PendingFire {
    pub player_id: u8,
    pub client_input_tick: u32,
    pub origin: Vec2,
    pub dir: Vec2,
}

#[derive(Resource, Default)]
pub struct PendingFires(pub Vec<PendingFire>);

/// Fire events produced this tick (cleared in `tick_advance`, filled by the
/// rewind resolver, drained by snapshot capture).
#[derive(Resource, Default)]
pub struct FireEvents(pub Vec<FireEvent>);

/// Smoothed per-player round-trip time in milliseconds. Default 60 ms (the DOTS
/// `RewindResolveSystem` initial value). Updated from the transport in a later
/// phase; the rewind resolver reads it for view-time computation.
#[derive(Resource)]
pub struct PlayerRtt(pub [f32; MAX_PLAYERS]);

impl Default for PlayerRtt {
    fn default() -> Self {
        Self([60.0; MAX_PLAYERS])
    }
}

// ---------- helpers ----------

#[inline]
pub fn pos_vec(p: &Pos2D) -> Vec2 {
    Vec2::new(p.x, p.y)
}