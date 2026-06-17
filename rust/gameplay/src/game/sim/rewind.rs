// Lag-compensated hit resolution. Ports of the DOTS RewindRecordSystem and
// RewindResolveSystem and the WireFormat.md §7 rewind contract.
//
// The ring stores one frame of (enemy_id, position) per server tick, captured
// at the END of the tick (after integrate). On a Fire command, the resolver
// rewinds to a fractional view-time, builds a bracketing-frame-interpolated
// enemy set, runs the piercing beam against it, and applies kills to the
// CURRENT world by id.
//
// `rewind_resolve` is an exclusive system so kills despawn immediately (before
// seek/integrate/contact run this tick), matching the DOTS ECB playback inside
// RewindResolveSystem.OnUpdate.

use std::collections::{HashMap, HashSet};

use bevy::prelude::*;

use super::components::*;
use crate::game::constants::*;
use crate::game::spatial::Pos2D;
use crate::game::types::FireEvent;

// ---------- Ring ----------

pub struct RewindFrame {
    pub tick: u32,
    pub enemies: Vec<(u16, Vec2)>,
}

#[derive(Resource)]
pub struct RewindRing {
    frames: Vec<RewindFrame>,
    head: usize,
    count: usize,
    cap: usize,
}

impl RewindRing {
    pub fn new(cap: usize) -> Self {
        let frames = (0..cap)
            .map(|_| RewindFrame {
                tick: 0,
                enemies: Vec::new(),
            })
            .collect();
        Self {
            frames,
            head: 0,
            count: 0,
            cap,
        }
    }

    /// Overwrite the head slot with this tick's enemy positions and advance.
    /// Reuses the slot's Vec to avoid per-tick reallocation.
    pub fn record(&mut self, tick: u32, it: impl Iterator<Item = (u16, Vec2)>) {
        let slot = &mut self.frames[self.head];
        slot.tick = tick;
        slot.enemies.clear();
        slot.enemies.extend(it);
        self.head = (self.head + 1) % self.cap;
        if self.count < self.cap {
            self.count += 1;
        }
    }

    pub fn clear(&mut self) {
        self.head = 0;
        self.count = 0;
    }

    pub fn count(&self) -> usize {
        self.count
    }

    fn oldest_index(&self) -> usize {
        (self.head + self.cap - self.count) % self.cap
    }

    /// Bracketing slots for a fractional view-tick. Returns (floor_idx,
    /// ceil_idx, alpha). Mirrors DOTS FindBracketingSlots, including the
    /// clamp-to-oldest fallback when the view precedes the buffer.
    fn bracket(&self, view_tick_f: f32) -> Option<(usize, usize, f32)> {
        if self.count == 0 {
            return None;
        }
        // i64 avoids the negative-float → unsigned wrap hazard for very early
        // ticks / high latency; the clamp-to-oldest path is what matters there.
        let view_floor = view_tick_f.floor() as i64;
        let view_ceil = view_floor + 1;

        let mut floor_slot: Option<usize> = None;
        let mut ceil_slot: Option<usize> = None;
        for i in 0..self.count {
            let idx = (self.head + self.cap - 1 - i) % self.cap;
            let t = self.frames[idx].tick as i64;
            if t == view_floor {
                floor_slot = Some(idx);
            }
            if t == view_ceil {
                ceil_slot = Some(idx);
            }
        }

        match floor_slot {
            None => {
                let oldest = self.oldest_index();
                Some((oldest, oldest, 0.0))
            }
            Some(f) => match ceil_slot {
                None => Some((f, f, 0.0)),
                Some(c) => {
                    let ft = self.frames[f].tick as f32;
                    let alpha = (view_tick_f - ft).clamp(0.0, 1.0);
                    Some((f, c, alpha))
                }
            },
        }
    }

    /// Resolve one fire against the interpolated set. Adds newly killed enemy
    /// ids to `kills` (only ids present in `alive`, i.e. still in the current
    /// world) and returns how many THIS beam newly killed (shared kill set, so
    /// an enemy already killed by an earlier beam this tick isn't recounted —
    /// matching the DOTS shared KillFlags).
    pub fn resolve_fire(
        &self,
        fire: &PendingFire,
        view_tick_f: f32,
        alive: &HashMap<u16, Entity>,
        kills: &mut HashSet<u16>,
    ) -> u32 {
        let Some((floor_i, ceil_i, alpha)) = self.bracket(view_tick_f) else {
            return 0;
        };
        let floor = &self.frames[floor_i];
        let ceil = &self.frames[ceil_i];
        let hit_r = ENEMY_RADIUS + BEAM_RADIUS;
        let hit_r_sq = hit_r * hit_r;
        let mut local = 0u32;

        // Floor-frame enemies; lerp with ceil if the same id is present there.
        // Floor-only enemies (died mid-bracket) stay at floor.pos.
        for (id, fpos) in &floor.enemies {
            let mut pos = *fpos;
            if floor_i != ceil_i {
                if let Some((_, cpos)) = ceil.enemies.iter().find(|(cid, _)| cid == id) {
                    pos = fpos.lerp(*cpos, alpha);
                }
            }
            if segment_hits(fire.origin, fire.dir, BULLET_MAX_RANGE, pos, hit_r_sq)
                && alive.contains_key(id)
                && kills.insert(*id)
            {
                local += 1;
            }
        }

        // Ceil-only enemies (spawned mid-bracket) included only if alpha ≥ 0.5.
        if alpha >= 0.5 && floor_i != ceil_i {
            for (id, cpos) in &ceil.enemies {
                if floor.enemies.iter().any(|(fid, _)| fid == id) {
                    continue;
                }
                if segment_hits(fire.origin, fire.dir, BULLET_MAX_RANGE, *cpos, hit_r_sq)
                    && alive.contains_key(id)
                    && kills.insert(*id)
                {
                    local += 1;
                }
            }
        }

        local
    }
}

/// `server_tick - (one_way_ms/1000)·TPS - (InterpBufferMs/1000)·TPS`.
pub fn compute_view_tick_f(current_tick: u32, one_way_ms: f32) -> f32 {
    current_tick as f32
        - (one_way_ms / 1000.0) * TICKS_PER_SECOND
        - (INTERPOLATION_BUFFER_MS / 1000.0) * TICKS_PER_SECOND
}

/// DOTS `SegmentHits`: project the enemy centre onto the beam ray, accept if the
/// projection lands within [0, max_range] and the perpendicular distance is
/// within the hit radius. (Deliberately NOT spatial.rs's entry-point test — the
/// kill decision must match the reference rewind exactly.)
fn segment_hits(origin: Vec2, dir: Vec2, max_range: f32, enemy: Vec2, hit_radius_sq: f32) -> bool {
    let to_enemy = enemy - origin;
    let t = to_enemy.dot(dir);
    if t < 0.0 || t > max_range {
        return false;
    }
    let closest = origin + dir * t;
    (enemy - closest).length_squared() <= hit_radius_sq
}

// ---------- Systems ----------

/// Step 4: resolve queued fires against the rewind ring; despawn kills now.
pub fn rewind_resolve(world: &mut World) {
    let pending = std::mem::take(&mut world.resource_mut::<PendingFires>().0);
    if pending.is_empty() {
        return;
    }

    let current_tick = world.resource::<WorldClock>().0;
    let rtt = world.resource::<PlayerRtt>().0;

    // Current alive enemies: id → entity. Kills are applied to this set by id.
    let alive: HashMap<u16, Entity> = world
        .query_filtered::<(Entity, &EnemyId), With<Enemy>>()
        .iter(world)
        .map(|(e, id)| (id.0, e))
        .collect();

    let mut kills: HashSet<u16> = HashSet::new();
    let mut fire_events: Vec<FireEvent> = Vec::with_capacity(pending.len());

    {
        let ring = world.resource::<RewindRing>();
        for f in &pending {
            let one_way = 0.5 * rtt.get(f.player_id as usize).copied().unwrap_or(60.0);
            let view_tick_f = compute_view_tick_f(current_tick, one_way);
            let local = ring.resolve_fire(f, view_tick_f, &alive, &mut kills);
            fire_events.push(FireEvent {
                tick: current_tick,
                shooter_id: f.player_id,
                origin_x: f.origin.x,
                origin_y: f.origin.y,
                dir_x: f.dir.x,
                dir_y: f.dir.y,
                distance: BULLET_MAX_RANGE,
                kill_count: local.min(255) as u8,
            });
        }
    }

    for id in &kills {
        if let Some(e) = alive.get(id) {
            world.despawn(*e);
        }
    }
    world.resource_mut::<FireEvents>().0.extend(fire_events);
}

/// Step 8: record post-integrate enemy positions into the ring for this tick.
pub fn rewind_record(
    clock: Res<WorldClock>,
    mut ring: ResMut<RewindRing>,
    enemies: Query<(&EnemyId, &Pos2D), With<Enemy>>,
) {
    ring.record(clock.0, enemies.iter().map(|(id, p)| (id.0, pos_vec(p))));
}

// ---------- Tests ----------

#[cfg(test)]
mod tests {
    use super::*;

    fn ent(id: u32) -> Entity {
        Entity::from_raw_u32(id).unwrap()
    }

    fn beam_along_x() -> PendingFire {
        PendingFire {
            player_id: 0,
            client_input_tick: 0,
            origin: Vec2::ZERO,
            dir: Vec2::new(1.0, 0.0),
        }
    }

    #[test]
    fn lerps_between_bracketing_frames() {
        let mut ring = RewindRing::new(8);
        ring.record(10, [(1u16, Vec2::new(10.0, 0.0))].into_iter());
        ring.record(11, [(1u16, Vec2::new(8.0, 0.0))].into_iter());

        // view 10.5 → enemy at lerp((10,0),(8,0),0.5) = (9,0); beam on x-axis hits.
        let alive = HashMap::from([(1u16, ent(1))]);
        let mut kills = HashSet::new();
        let local = ring.resolve_fire(&beam_along_x(), 10.5, &alive, &mut kills);
        assert_eq!(local, 1);
        assert!(kills.contains(&1));
    }

    #[test]
    fn clamps_to_oldest_when_view_precedes_buffer() {
        let mut ring = RewindRing::new(8);
        ring.record(10, [(1u16, Vec2::new(10.0, 0.0))].into_iter());
        ring.record(11, [(1u16, Vec2::new(8.0, 0.0))].into_iter());

        // view 3.0 is older than the oldest frame (tick 10) → clamp to tick 10.
        let alive = HashMap::from([(1u16, ent(1))]);
        let mut kills = HashSet::new();
        let local = ring.resolve_fire(&beam_along_x(), 3.0, &alive, &mut kills);
        assert_eq!(local, 1);
    }

    #[test]
    fn dead_enemy_not_in_alive_is_not_killed() {
        let mut ring = RewindRing::new(8);
        ring.record(10, [(1u16, Vec2::new(10.0, 0.0))].into_iter());
        // Enemy id 1 is in the ring but NOT in the current alive set.
        let alive: HashMap<u16, Entity> = HashMap::new();
        let mut kills = HashSet::new();
        let local = ring.resolve_fire(&beam_along_x(), 10.0, &alive, &mut kills);
        assert_eq!(local, 0);
        assert!(kills.is_empty());
    }

    #[test]
    fn off_axis_enemy_is_missed() {
        let mut ring = RewindRing::new(8);
        // 5 m off the beam axis — well outside enemy+beam radius.
        ring.record(10, [(1u16, Vec2::new(10.0, 5.0))].into_iter());
        let alive = HashMap::from([(1u16, ent(1))]);
        let mut kills = HashSet::new();
        let local = ring.resolve_fire(&beam_along_x(), 10.0, &alive, &mut kills);
        assert_eq!(local, 0);
    }

    #[test]
    fn piercing_kills_all_in_path_once() {
        let mut ring = RewindRing::new(8);
        ring.record(
            10,
            [
                (1u16, Vec2::new(3.0, 0.0)),
                (2u16, Vec2::new(6.0, 0.0)),
                (3u16, Vec2::new(9.0, 0.0)),
                (4u16, Vec2::new(6.0, 4.0)), // off-axis decoy
            ]
            .into_iter(),
        );
        let alive = HashMap::from([(1u16, ent(1)), (2u16, ent(2)), (3u16, ent(3)), (4u16, ent(4))]);
        let mut kills = HashSet::new();
        let local = ring.resolve_fire(&beam_along_x(), 10.0, &alive, &mut kills);
        assert_eq!(local, 3);
        assert!(kills.contains(&1) && kills.contains(&2) && kills.contains(&3));
        assert!(!kills.contains(&4));

        // A second beam over the same already-killed enemies adds no new kills.
        let local2 = ring.resolve_fire(&beam_along_x(), 10.0, &alive, &mut kills);
        assert_eq!(local2, 0);
    }
}