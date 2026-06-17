// Hand-port of DGSvsHS/Assets/_Game/Gameplay/DeterministicRng.cs.
//
// xoroshiro128+ with SplitMix64-expanded seed. This MUST stay bit-identical to
// the C# reference: same seeding, same next_u64, and crucially the same float
// derivation `(next >> 40) * 2^-24`. Do NOT swap in rand_xoshiro — its
// SeedableRng seeding and float conversion would not match the C# byte-for-byte.
//
// (Cross-validation against the real C# RNG is deferred per the user's call to
// skip the C# fixture harness for now; the port is line-for-line and the tests
// below pin determinism + the documented algorithm invariants.)

#![allow(dead_code)]

#[derive(Copy, Clone, Debug)]
pub struct DeterministicRng {
    s0: u64,
    s1: u64,
}

impl DeterministicRng {
    /// Mirror of `DeterministicRng.FromSeed`: seed both state words with
    /// SplitMix64, then guard against the all-zero state.
    pub fn from_seed(seed: u64) -> Self {
        let mut sm = seed;
        let s0 = splitmix64(&mut sm);
        let s1 = splitmix64(&mut sm);
        let mut rng = Self { s0, s1 };
        if (rng.s0 | rng.s1) == 0 {
            rng.s1 = 1;
        }
        rng
    }

    /// xoroshiro128+ — `result = s0 + s1` (wrapping), then advance state.
    pub fn next_u64(&mut self) -> u64 {
        let s0 = self.s0;
        let mut s1 = self.s1;
        let result = s0.wrapping_add(s1);
        s1 ^= s0;
        self.s0 = s0.rotate_left(24) ^ s1 ^ (s1 << 16);
        self.s1 = s1.rotate_left(37);
        result
    }

    /// Top 24 bits scaled into [0, 1). 2^-24 is exactly representable and the
    /// 24-bit operand converts to f32 exactly, so this is bit-identical to the
    /// C# `(NextU64() >> 40) * (1.0f / 16777216.0f)`.
    pub fn next_float01(&mut self) -> f32 {
        (self.next_u64() >> 40) as f32 * (1.0 / 16_777_216.0)
    }

    pub fn next_range(&mut self, min: f32, max: f32) -> f32 {
        min + (max - min) * self.next_float01()
    }
}

/// SplitMix64 (Vigna). Used only to expand the u64 seed into the 128-bit state.
fn splitmix64(state: &mut u64) -> u64 {
    *state = state.wrapping_add(0x9E37_79B9_7F4A_7C15);
    let mut z = *state;
    z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
    z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
    z ^ (z >> 31)
}

#[cfg(test)]
mod tests {
    use super::*;

    const SEED: u64 = 0xC0FF_EEF0_0D;

    #[test]
    fn same_seed_is_reproducible() {
        let mut a = DeterministicRng::from_seed(SEED);
        let mut b = DeterministicRng::from_seed(SEED);
        for _ in 0..1000 {
            assert_eq!(a.next_u64(), b.next_u64());
        }
    }

    #[test]
    fn different_seeds_diverge() {
        let mut a = DeterministicRng::from_seed(SEED);
        let mut b = DeterministicRng::from_seed(SEED ^ 1);
        // Extremely unlikely to match on the very first draw for distinct seeds.
        assert_ne!(a.next_u64(), b.next_u64());
    }

    #[test]
    fn zero_seed_is_not_degenerate() {
        // FromSeed expands via SplitMix64, so even seed 0 yields non-zero state
        // and a non-constant stream.
        let mut r = DeterministicRng::from_seed(0);
        let first = r.next_u64();
        let second = r.next_u64();
        assert_ne!(first, 0);
        assert_ne!(first, second);
    }

    #[test]
    fn float01_in_unit_interval() {
        let mut r = DeterministicRng::from_seed(SEED);
        for _ in 0..100_000 {
            let f = r.next_float01();
            assert!((0.0..1.0).contains(&f), "float01 out of range: {f}");
        }
    }

    #[test]
    fn range_respects_bounds() {
        let mut r = DeterministicRng::from_seed(SEED);
        let two_pi = std::f32::consts::PI * 2.0;
        for _ in 0..100_000 {
            let a = r.next_range(0.0, two_pi);
            assert!((0.0..two_pi).contains(&a), "angle out of range: {a}");
        }
    }

    #[test]
    fn splitmix64_advances_state() {
        // Sanity on the seeding primitive: distinct, non-zero, state mutates.
        let mut s = SEED;
        let a = splitmix64(&mut s);
        let b = splitmix64(&mut s);
        assert_ne!(a, b);
        assert_ne!(a, 0);
    }

    // TODO(cross-validate): once the real-game oracle is available, pin a golden
    // next_u64 / next_float01 / next_range(0, 2π) stream for SEED against the C#
    // DeterministicRng output. The algorithm here is a line-for-line port, so
    // this is a belt-and-suspenders check, not expected to surface a defect.
}