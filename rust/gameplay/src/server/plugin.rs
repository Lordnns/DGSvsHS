// ServerPlugin — wires snapshot capture, history recording, broadcast, and
// lifecycle systems into the same FixedUpdate chain as the sim. The sim chain
// (defined in game/sim/plugin.rs) ends at SimSet::Snapshot; we extend it by
// adding capture + record + broadcast in that set, plus a SimSet::Lifecycle
// stage after for the Resetting transition.

use bevy::prelude::*;

use crate::game::constants::SNAPSHOT_HISTORY_TICKS;
use crate::game::sim::plugin::SimSet;
use crate::game::sim::components::Lifecycle;

use super::broadcast::broadcast_snapshot;
use super::capture::{record_history, snapshot_capture, SnapshotScratch};
use super::history::WorldStateHistory;
use super::lifecycle::{handle_client_disconnect, handle_resetting};
use super::recipient::RecipientStates;

pub struct ServerPlugin;

impl Plugin for ServerPlugin {
    fn build(&self, app: &mut App) {
        app.insert_resource(SnapshotScratch::default())
            .insert_resource(WorldStateHistory::new(SNAPSHOT_HISTORY_TICKS))
            .insert_resource(RecipientStates::default());

        // Snapshot-side systems extend the sim chain. They run only when the
        // server is in Running state (gated by SimSet's run_if(sim_running)).
        app.add_systems(
            FixedUpdate,
            (
                snapshot_capture,
                record_history,
                broadcast_snapshot,
            )
                .chain()
                .in_set(SimSet::Snapshot),
        );

        // Lifecycle systems run unconditionally in Update so they can react
        // to network events regardless of sim state.
        app.add_systems(
            Update,
            (
                handle_client_disconnect,
                handle_resetting.run_if(resource_equals(Lifecycle::Resetting)),
            ),
        );
    }
}

fn resource_equals<T: Resource + PartialEq>(value: T) -> impl Fn(Res<T>) -> bool {
    move |res: Res<T>| *res == value
}
