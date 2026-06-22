// Per-recipient enemy selection. Port of csharp_arch_server/Net/SnapshotPriority.cs.
//
// SelectForFull: rank all current enemies by distance² to the recipient anchor
// and pack the closest ones until the byte budget is exhausted.
//
// SelectForDelta (3 lanes):
//   1. Removed — baseline-confirmed IDs no longer in current. Tiny (2 B each),
//      packed first. If even those overflow, truncate to half the budget.
//   2. Spawn lane — current IDs the recipient hasn't yet seen. Sorted by
//      distance², capped at MaxSpawnsPerSnapshot.
//   3. Animation lane — confirmed-and-in-baseline IDs whose quantized position
//      changed. Score = distance − StalenessWeight × ticks_since_last_sent so
//      stale-but-far enemies still get refreshed.

use std::collections::{HashMap, HashSet};

use bevy::math::Vec2;

use crate::game::constants::{MAX_SPAWNS_PER_SNAPSHOT, STALENESS_WEIGHT};
use crate::game::{EnemyDeltaEntry, EnemySnap, Snapshot};
use crate::network::{
    enemy_position_changed, ENEMY_DELTA_ENTRY_BYTES, ENEMY_SNAP_FULL_BYTES,
};

#[derive(Copy, Clone, Debug)]
pub struct ScoredEnemy {
    pub index: usize,
    pub score: f32,
}

fn sort_ascending(scored: &mut [ScoredEnemy]) {
    scored.sort_by(|a, b| a.score.partial_cmp(&b.score).unwrap_or(std::cmp::Ordering::Equal));
}

pub fn select_for_full(
    current: &Snapshot,
    recipient: Vec2,
    enemy_byte_budget: usize,
    out_selected: &mut Vec<EnemySnap>,
    scratch: &mut Vec<ScoredEnemy>,
) {
    out_selected.clear();
    scratch.clear();

    for (i, e) in current.enemies.iter().enumerate() {
        let dx = e.pos_x - recipient.x;
        let dy = e.pos_y - recipient.y;
        scratch.push(ScoredEnemy {
            index: i,
            score: dx * dx + dy * dy,
        });
    }
    sort_ascending(scratch);

    let mut remaining = enemy_byte_budget;
    let sz = ENEMY_SNAP_FULL_BYTES;
    for s in scratch.iter() {
        if sz > remaining {
            break;
        }
        remaining -= sz;
        out_selected.push(current.enemies[s.index]);
    }
}

#[allow(clippy::too_many_arguments)]
pub fn select_for_delta(
    current: &Snapshot,
    baseline: &Snapshot,
    recipient: Vec2,
    confirmed_ids: &HashSet<u16>,
    ticks_since_last_sent: &HashMap<u16, u16>,
    enemy_byte_budget: usize,
    out_changed: &mut Vec<EnemyDeltaEntry>,
    out_removed: &mut Vec<u16>,
    out_added: &mut Vec<EnemySnap>,
    out_included_ids: &mut HashSet<u16>,
    scratch_scored: &mut Vec<ScoredEnemy>,
    scratch_current_ids: &mut HashSet<u16>,
    scratch_baseline_index_by_id: &mut HashMap<u16, usize>,
) {
    out_changed.clear();
    out_removed.clear();
    out_added.clear();
    out_included_ids.clear();
    scratch_scored.clear();
    scratch_current_ids.clear();
    scratch_baseline_index_by_id.clear();

    for e in current.enemies.iter() {
        scratch_current_ids.insert(e.id);
    }

    // Lane 1: removed = (confirmed | baseline) − current.
    if !confirmed_ids.is_empty() {
        for &cid in confirmed_ids.iter() {
            if !scratch_current_ids.contains(&cid) {
                out_removed.push(cid);
            }
        }
    } else {
        for e in baseline.enemies.iter() {
            if !scratch_current_ids.contains(&e.id) {
                out_removed.push(e.id);
            }
        }
    }

    for (i, e) in baseline.enemies.iter().enumerate() {
        scratch_baseline_index_by_id.insert(e.id, i);
    }

    let mut removed_bytes = out_removed.len() * 2;
    if removed_bytes > enemy_byte_budget {
        let keepable = enemy_byte_budget / 2;
        if keepable < out_removed.len() {
            out_removed.truncate(keepable);
        }
        removed_bytes = out_removed.len() * 2;
    }
    let mut remaining = enemy_byte_budget.saturating_sub(removed_bytes);

    // Lane 2: spawns = current − confirmed.
    scratch_scored.clear();
    let have_confirmed = !confirmed_ids.is_empty();
    for (i, e) in current.enemies.iter().enumerate() {
        let is_pending_spawn = !have_confirmed || !confirmed_ids.contains(&e.id);
        if !is_pending_spawn {
            continue;
        }
        let dx = e.pos_x - recipient.x;
        let dy = e.pos_y - recipient.y;
        scratch_scored.push(ScoredEnemy {
            index: i,
            score: dx * dx + dy * dy,
        });
    }
    sort_ascending(scratch_scored);

    let new_entry_size = ENEMY_SNAP_FULL_BYTES;
    let mut spawns_this_snapshot = 0usize;
    for s in scratch_scored.iter() {
        if spawns_this_snapshot >= MAX_SPAWNS_PER_SNAPSHOT {
            break;
        }
        if remaining < new_entry_size {
            break;
        }
        let e = current.enemies[s.index];
        out_added.push(e);
        out_included_ids.insert(e.id);
        remaining -= new_entry_size;
        spawns_this_snapshot += 1;
    }

    // Lane 3: animation = confirmed ∩ baseline, position changed.
    scratch_scored.clear();
    for (i, e) in current.enemies.iter().enumerate() {
        if !have_confirmed || !confirmed_ids.contains(&e.id) {
            continue;
        }
        if !scratch_baseline_index_by_id.contains_key(&e.id) {
            continue;
        }
        let dx = e.pos_x - recipient.x;
        let dy = e.pos_y - recipient.y;
        let dist = (dx * dx + dy * dy).sqrt();
        let tsls = ticks_since_last_sent.get(&e.id).copied().unwrap_or(0);
        let score = dist - STALENESS_WEIGHT * (tsls as f32);
        scratch_scored.push(ScoredEnemy { index: i, score });
    }
    sort_ascending(scratch_scored);

    let changed_entry_size = ENEMY_DELTA_ENTRY_BYTES;
    for s in scratch_scored.iter() {
        let e = current.enemies[s.index];
        let base_idx = *scratch_baseline_index_by_id.get(&e.id).unwrap();
        let b = &baseline.enemies[base_idx];
        if !enemy_position_changed(b, &e) {
            // Position unchanged after quantization — still counts as confirmed
            // for this recipient (the client already has the right value).
            out_included_ids.insert(e.id);
            continue;
        }
        if changed_entry_size > remaining {
            break;
        }
        remaining -= changed_entry_size;
        out_changed.push(EnemyDeltaEntry {
            id: e.id,
            pos_x: e.pos_x,
            pos_y: e.pos_y,
        });
        out_included_ids.insert(e.id);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::game::SnapshotKind;

    fn enemy(id: u16, x: f32, y: f32) -> EnemySnap {
        EnemySnap {
            id,
            pos_x: x,
            pos_y: y,
        }
    }

    fn snap_full(tick: u32, enemies: &[EnemySnap]) -> Snapshot {
        let mut s = Snapshot::with_capacity();
        s.kind = SnapshotKind::Full;
        s.tick = tick;
        s.enemies.extend_from_slice(enemies);
        s
    }

    #[test]
    fn full_selects_closest_first() {
        let cur = snap_full(
            1,
            &[
                enemy(1, 0.0, 0.0),
                enemy(2, 100.0, 0.0),
                enemy(3, 5.0, 0.0),
            ],
        );
        let mut out = Vec::new();
        let mut scratch = Vec::new();
        select_for_full(&cur, Vec2::ZERO, 12, &mut out, &mut scratch);
        let ids: Vec<u16> = out.iter().map(|e| e.id).collect();
        assert_eq!(ids, vec![1, 3]);
    }

    #[test]
    fn delta_classifies_removed_spawn_animation() {
        let baseline = snap_full(10, &[enemy(1, 0.0, 0.0), enemy(2, 5.0, 0.0)]);
        let current = snap_full(11, &[enemy(1, 0.5, 0.0), enemy(3, 10.0, 0.0)]);
        let confirmed: HashSet<u16> = [1u16, 2u16].into_iter().collect();
        let tsls: HashMap<u16, u16> = HashMap::new();
        let mut changed = Vec::new();
        let mut removed = Vec::new();
        let mut added = Vec::new();
        let mut included = HashSet::new();
        let mut scratch_scored = Vec::new();
        let mut scratch_current_ids = HashSet::new();
        let mut scratch_baseline = HashMap::new();

        select_for_delta(
            &current,
            &baseline,
            Vec2::ZERO,
            &confirmed,
            &tsls,
            10_000,
            &mut changed,
            &mut removed,
            &mut added,
            &mut included,
            &mut scratch_scored,
            &mut scratch_current_ids,
            &mut scratch_baseline,
        );

        assert_eq!(removed, vec![2u16]);
        assert_eq!(added.len(), 1);
        assert_eq!(added[0].id, 3);
        assert_eq!(changed.len(), 1);
        assert_eq!(changed[0].id, 1);
        assert!(included.contains(&1));
        assert!(included.contains(&3));
    }

    #[test]
    fn delta_skips_unchanged_position_but_marks_included() {
        let baseline = snap_full(10, &[enemy(1, 5.0, 0.0)]);
        let current = snap_full(11, &[enemy(1, 5.0, 0.0)]);
        let confirmed: HashSet<u16> = [1u16].into_iter().collect();
        let tsls = HashMap::new();
        let mut changed = Vec::new();
        let mut removed = Vec::new();
        let mut added = Vec::new();
        let mut included = HashSet::new();
        let mut scratch_scored = Vec::new();
        let mut scratch_current_ids = HashSet::new();
        let mut scratch_baseline = HashMap::new();

        select_for_delta(
            &current,
            &baseline,
            Vec2::ZERO,
            &confirmed,
            &tsls,
            10_000,
            &mut changed,
            &mut removed,
            &mut added,
            &mut included,
            &mut scratch_scored,
            &mut scratch_current_ids,
            &mut scratch_baseline,
        );

        assert!(changed.is_empty());
        assert!(removed.is_empty());
        assert!(added.is_empty());
        assert!(included.contains(&1));
    }
}
