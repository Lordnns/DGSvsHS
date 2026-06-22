using DGSvsHS.Gameplay;
using UnityEngine;

namespace DGSvsHS.Client
{
    /// <summary>
    /// Deterministic bot driver for benchmark trials and bot-harness sessions.
    ///
    /// <para>Fully a pure function of (clientTick, botId, seed) — no snapshot
    /// inspection, no Unity randomness, no wall-clock dependency. The bot:</para>
    /// <list type="bullet">
    ///   <item><b>Moves</b> on a fixed-radius orbit (per-bot radius / angular
    ///   speed / phase derived from the seeded RNG).</item>
    ///   <item><b>Aims</b> by sweeping a second independent rotation, also
    ///   tick-driven (per-bot aim sweep rate + phase from the same RNG stream).
    ///   We deliberately do NOT pick "nearest enemy in latest snapshot" — that
    ///   would couple aim direction to network arrival jitter and break
    ///   run-to-run reproducibility of fire events.</item>
    ///   <item><b>Fires</b> on a fixed duty cycle (N ticks on, M ticks off,
    ///   both derived from the RNG). Server-side cooldown (PlayerFireCooldownSec)
    ///   further rate-limits the actual beams.</item>
    /// </list>
    ///
    /// <para>Same (botId, seed) ALWAYS produces the same input stream across
    /// runs, across servers, across machines. That's what makes the power-trial
    /// workload comparable bevy vs arch vs dgs.</para>
    ///
    /// <para>Lives in the Client assembly (not Harness) so <see cref="ClientMain"/>
    /// can instantiate it directly from CLI args (`--bot-id N`) without crossing
    /// assemblies. TrialRunner (in Harness) uses the same class.</para>
    /// </summary>
    public sealed class AutoPilot : IAutoPilot
    {
        private readonly byte _botId;
        private readonly ulong _seed;

        // Movement orbit parameters.
        private readonly float _orbitRadius;
        private readonly float _orbitAngularSpeed;
        private readonly float _orbitPhase;

        // Aim sweep parameters (independent of movement orbit so the beam isn't
        // pinned to the tangent of the orbit — feels more like a real player
        // panning around).
        private readonly float _aimAngularSpeed;
        private readonly float _aimPhase;

        // Fire duty cycle in ticks: `_fireOnTicks` ticks pressing fire, then
        // `_fireOffTicks` ticks releasing. Period = on+off, repeats forever.
        // Deterministic per (botId, seed). Realistic-looking trigger discipline
        // without any RNG at sample time.
        private readonly int _fireOnTicks;
        private readonly int _fireOffTicks;

        public AutoPilot(byte botId, ulong seed)
        {
            _botId = botId;
            _seed = seed;

            // Per-bot RNG. The botId-XOR-magic decorrelates per-bot streams while
            // letting one global seed reproduce them all.
            var rng = DeterministicRng.FromSeed(seed ^ (0xA5A5A5A5UL + botId));

            _orbitRadius       = rng.NextRange(Constants.ArenaRadius * 0.4f, Constants.ArenaRadius * 0.75f);
            _orbitAngularSpeed = rng.NextRange(0.5f, 1.2f);                 // rad/sec
            _orbitPhase        = rng.NextRange(0f, Mathf.PI * 2f);

            _aimAngularSpeed   = rng.NextRange(2.0f, 4.5f);                 // rad/sec, faster than orbit
            _aimPhase          = rng.NextRange(0f, Mathf.PI * 2f);

            // 30..60 ticks on, 6..18 ticks off (at 62.5 Hz: ~480-960 ms bursts,
            // ~100-290 ms gaps). Bot-specific but seed-stable.
            _fireOnTicks  = Mathf.RoundToInt(rng.NextRange(30f, 60f));
            _fireOffTicks = Mathf.RoundToInt(rng.NextRange(6f, 18f));
        }

        public InputCmd Sample(uint clientTick, Vector2 localPlayerPredictedPos, ClientSimulation sim)
        {
            float t = clientTick * Constants.SimDt;

            // ---- Move: walk toward the next point on the orbit circle.
            float orbitAngle = _orbitPhase + _orbitAngularSpeed * t;
            Vector2 target = new Vector2(Mathf.Cos(orbitAngle), Mathf.Sin(orbitAngle)) * _orbitRadius;
            Vector2 toTarget = target - localPlayerPredictedPos;
            Vector2 move = toTarget.sqrMagnitude > 0.04f ? toTarget.normalized : Vector2.zero;

            // ---- Aim: pure tick-driven rotation. Independent of snapshot content
            // so two runs of the same (botId, seed) emit bit-identical aim vectors.
            float aimAngle = _aimPhase + _aimAngularSpeed * t;
            Vector2 aim = new Vector2(Mathf.Cos(aimAngle), Mathf.Sin(aimAngle));

            // ---- Fire: fixed duty cycle in ticks. Press fire while inside the
            // "on" window, release during the "off" window. Server cooldown
            // (PlayerFireCooldownSec = 0.12s = ~7.5 ticks) further rate-limits.
            int period = _fireOnTicks + _fireOffTicks;
            int phase = period > 0 ? (int)(clientTick % (uint)period) : 0;
            bool fireHeld = phase < _fireOnTicks;
            var flags = fireHeld ? InputFlags.Fire : InputFlags.None;

            return new InputCmd { Tick = clientTick, Move = move, Aim = aim, Flags = flags };
        }
    }
}
