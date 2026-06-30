// Per-recipient ack/staleness/pending-send bookkeeping for delta snapshots.
// Port of csharp_arch_server/Server/RecipientSnapshotState.cs.

use std::collections::HashMap;

use bevy::prelude::Resource;

use crate::game::constants::{MAX_DELTA_DEPTH, MAX_PLAYERS};

use super::bitset::IdBitSet;

/// `MaxDeltaDepth * 2` — bounds the per-recipient pending-send list so a
/// permanently-stalled client can't make the server grow without bound.
const MAX_PENDING: usize = (MAX_DELTA_DEPTH as usize) * 2;

#[derive(Clone, Debug, Default)]
struct PendingEntry {
    tick: u32,
    is_full: bool,
    included: Vec<u16>,
    removed: Vec<u16>,
}

#[derive(Default, Debug)]
pub struct RecipientSnapshotState {
    /// Highest snapshot tick the recipient has acked.
    pub last_acked_server_tick: u32,

    /// Set of enemy IDs the recipient is known to have. Updated when a pending
    /// send is acked.
    pub confirmed_ids: IdBitSet,

    /// Per-enemy staleness counter, capped at u16::MAX. Reset to 0 when an
    /// entity is included in a send, incremented on every other send. Keys
    /// mirror `confirmed_ids`.
    pub ticks_since_last_sent: HashMap<u16, u16>,

    /// Set by the transport when an input batch arrives with a higher ack;
    /// consumed by the broadcast system once per tick.
    pub pending_acked_tick: u32,

    pending: Vec<PendingEntry>,
}

impl RecipientSnapshotState {
    pub fn new() -> Self {
        Self::default()
    }

    /// Drain `pending_acked_tick` and advance `last_acked_server_tick` if it
    /// grew. Returns true iff an advance occurred (caller should call
    /// `on_ack_advanced` next).
    pub fn drain_pending_ack(&mut self) -> bool {
        let pending = self.pending_acked_tick;
        self.pending_acked_tick = 0;
        if pending > self.last_acked_server_tick {
            self.last_acked_server_tick = pending;
            true
        } else {
            false
        }
    }

    /// Record what was sent in a snapshot so it can be retired when acked.
    /// `included` is the set of enemy ids actually packed into the snapshot
    /// body (full or delta); `removed` is the delta's removed list (empty for
    /// full snapshots).
    pub fn on_snapshot_sent(
        &mut self,
        tick: u32,
        is_full: bool,
        included: &IdBitSet,
        removed: &[u16],
    ) {
        // Leak cap: drop the oldest entries first.
        while self.pending.len() >= MAX_PENDING {
            self.pending.remove(0);
        }

        self.pending.push(PendingEntry {
            tick,
            is_full,
            included: included.iter().collect(),
            removed: removed.to_vec(),
        });

        // Advance staleness counters. Keys mirror `confirmed_ids`, so iterate
        // the bitset directly — no key snapshot/allocation needed.
        for id in self.confirmed_ids.iter() {
            if included.contains(id) {
                self.ticks_since_last_sent.insert(id, 0);
            } else if let Some(cur) = self.ticks_since_last_sent.get_mut(&id) {
                *cur = cur.saturating_add(1);
            }
        }
    }

    /// Walk the pending list and retire every entry up to `last_acked_server_tick`.
    /// Full sends reset ConfirmedIds to just their included set; delta sends
    /// merge in includes and drop removed ids.
    pub fn on_ack_advanced(&mut self) {
        let acked = self.last_acked_server_tick;
        let mut write_idx = 0usize;
        for read_idx in 0..self.pending.len() {
            // Borrow individually so we can mutate the entry in place when
            // we keep it.
            let keep;
            {
                let p = &self.pending[read_idx];
                keep = p.tick > acked;
            }
            if keep {
                if write_idx != read_idx {
                    self.pending.swap(write_idx, read_idx);
                }
                write_idx += 1;
                continue;
            }

            let entry = std::mem::take(&mut self.pending[read_idx]);
            if entry.is_full {
                self.confirmed_ids.clear();
                self.ticks_since_last_sent.clear();
                for id in entry.included {
                    self.confirmed_ids.insert(id);
                    self.ticks_since_last_sent.insert(id, 0);
                }
            } else {
                for id in entry.included {
                    self.confirmed_ids.insert(id);
                    self.ticks_since_last_sent.entry(id).or_insert(0);
                }
                for id in entry.removed {
                    self.confirmed_ids.remove(id);
                    self.ticks_since_last_sent.remove(&id);
                }
            }
        }
        self.pending.truncate(write_idx);
    }

    pub fn clear(&mut self) {
        self.last_acked_server_tick = 0;
        self.confirmed_ids.clear();
        self.ticks_since_last_sent.clear();
        self.pending_acked_tick = 0;
        self.pending.clear();
    }
}

/// Per-slot recipient bookkeeping + the highest input tick observed from each
/// slot (used as the per-recipient `last_processed_input_tick` snapshot field).
#[derive(Resource)]
pub struct RecipientStates {
    pub slots: [Option<RecipientSnapshotState>; MAX_PLAYERS],
    pub highest_input_tick: [u32; MAX_PLAYERS],
}

impl Default for RecipientStates {
    fn default() -> Self {
        Self {
            slots: [const { None }; MAX_PLAYERS],
            highest_input_tick: [0; MAX_PLAYERS],
        }
    }
}

impl RecipientStates {
    pub fn ensure(&mut self, slot: u8) -> &mut RecipientSnapshotState {
        let i = slot as usize;
        if self.slots[i].is_none() {
            self.slots[i] = Some(RecipientSnapshotState::new());
        }
        self.slots[i].as_mut().unwrap()
    }

    pub fn get_mut(&mut self, slot: u8) -> Option<&mut RecipientSnapshotState> {
        self.slots.get_mut(slot as usize).and_then(|s| s.as_mut())
    }

    pub fn clear_slot(&mut self, slot: u8) {
        let i = slot as usize;
        if let Some(s) = self.slots.get_mut(i) {
            *s = None;
        }
        if let Some(t) = self.highest_input_tick.get_mut(i) {
            *t = 0;
        }
    }

    pub fn record_input_tick(&mut self, slot: u8, tick: u32) {
        if let Some(cur) = self.highest_input_tick.get_mut(slot as usize) {
            if tick > *cur {
                *cur = tick;
            }
        }
    }

    /// Record an incoming input's `last_acked_server_tick`. The broadcast
    /// system folds this into `last_acked_server_tick` once per tick.
    pub fn record_pending_ack(&mut self, slot: u8, acked: u32) {
        if acked == 0 {
            return;
        }
        let r = self.ensure(slot);
        if acked > r.pending_acked_tick {
            r.pending_acked_tick = acked;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ids(xs: &[u16]) -> IdBitSet {
        let mut b = IdBitSet::new();
        for &x in xs {
            b.insert(x);
        }
        b
    }

    #[test]
    fn full_send_then_ack_replaces_confirmed_set() {
        let mut r = RecipientSnapshotState::new();
        r.on_snapshot_sent(10, true, &ids(&[1, 2, 3]), &[]);
        r.pending_acked_tick = 10;
        assert!(r.drain_pending_ack());
        r.on_ack_advanced();
        assert_eq!(r.last_acked_server_tick, 10);
        assert_eq!(r.confirmed_ids, ids(&[1, 2, 3]));
    }

    #[test]
    fn delta_send_then_ack_merges_includes_and_drops_removed() {
        let mut r = RecipientSnapshotState::new();
        r.on_snapshot_sent(10, true, &ids(&[1, 2, 3]), &[]);
        r.pending_acked_tick = 10;
        r.drain_pending_ack();
        r.on_ack_advanced();
        r.on_snapshot_sent(11, false, &ids(&[4]), &[1]);
        r.pending_acked_tick = 11;
        r.drain_pending_ack();
        r.on_ack_advanced();
        assert_eq!(r.confirmed_ids, ids(&[2, 3, 4]));
    }

    #[test]
    fn staleness_counts_grow_for_non_included() {
        let mut r = RecipientSnapshotState::new();
        r.on_snapshot_sent(10, true, &ids(&[1, 2]), &[]);
        r.pending_acked_tick = 10;
        r.drain_pending_ack();
        r.on_ack_advanced();
        // Send a delta that only refreshes id 1; id 2 should age.
        r.on_snapshot_sent(11, false, &ids(&[1]), &[]);
        assert_eq!(*r.ticks_since_last_sent.get(&1).unwrap(), 0);
        assert_eq!(*r.ticks_since_last_sent.get(&2).unwrap(), 1);
    }

    #[test]
    fn pending_cap_truncates_old_entries() {
        let mut r = RecipientSnapshotState::new();
        for t in 0..(MAX_PENDING + 5) as u32 {
            r.on_snapshot_sent(t, false, &ids(&[1]), &[]);
        }
        assert_eq!(r.pending.len(), MAX_PENDING);
    }

    #[test]
    fn drain_pending_ack_no_change_returns_false() {
        let mut r = RecipientSnapshotState::new();
        assert!(!r.drain_pending_ack());
        r.last_acked_server_tick = 20;
        r.pending_acked_tick = 10; // older than current
        assert!(!r.drain_pending_ack());
        assert_eq!(r.last_acked_server_tick, 20);
    }
}
