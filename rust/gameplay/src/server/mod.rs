// Server-side orchestration: snapshot capture, per-recipient delta machinery,
// lifecycle transitions, input routing. Sits between the pure sim (game/sim)
// and the QUIC transport (network/). Ports the equivalent of
// csharp_arch_server/Server/* and csharp_arch_server/Net/SnapshotPriority.cs +
// the BroadcastSnapshot path from csharp_arch_server/Net/QuicServer.cs.

pub mod bitset;
pub mod broadcast;
pub mod capture;
pub mod history;
pub mod lifecycle;
pub mod plugin;
pub mod priority;
pub mod recipient;

pub use history::WorldStateHistory;
pub use plugin::ServerPlugin;
pub use recipient::{RecipientSnapshotState, RecipientStates};
