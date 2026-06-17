// Authoritative game simulation for the Bevy server (Build 2), ported to match
// the Unity DOTS reference systems under
// DGSvsHS/Assets/_Game/Server/Dots/Systems/ and the locked sub-step order in
// WireFormat.md §8.2. Transport-independent — the QUIC layer lives in
// crate::network.

pub mod components;
pub mod movement;
pub mod plugin;
pub mod rewind;
pub mod rng;
pub mod round;

pub use components::*;
pub use plugin::{SimPlugin, SimSet};
pub use rewind::{RewindRing};
pub use rng::DeterministicRng;