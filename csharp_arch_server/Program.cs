
using System.Diagnostics;
using System.Globalization;
using System.Numerics;
using Arch.Core;
using DGSvsHS.ArchServer.Net;
using DGSvsHS.ArchServer.Server;
using DGSvsHS.ArchServer.Server.Systems;
using DGSvsHS.Gameplay;

namespace DGSvsHS.ArchServer;

public static class Program
{
    public enum ServerLifecycle : byte
    {
        Booting = 0,
        Idle = 1,
        Running = 2,
        Resetting = 3,
        ShuttingDown = 4,
    }

    private sealed class Config
    {
        public ushort Port = 7778;
        public ulong Seed = 0xC0FFEE_F00DUL;
        public bool AutoStartMatch = true;
#if GODMODE_DEFAULT
        public bool GodMode = true;
#else
        public bool GodMode;
#endif
        public float HeartbeatIntervalSec = 1.0f;
        public int SimulatedClients;
        public float? RunForSeconds;
    }

    public static int Main(string[] args)
    {
        CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
        Thread.CurrentThread.CurrentCulture = CultureInfo.InvariantCulture;

        var cfg = ParseArgs(args);

        Console.WriteLine("[ArchServer] DGSvsHS C# / Arch / BepuPhysics server — sim port of DedicatedServerMain");
#if GODMODE_DEFAULT
        Console.WriteLine("[ArchServer] BUILD FLAVOR: godmode (compile-time default, --god-mode=false still overrides)");
#endif
        Console.WriteLine($"[ArchServer] tick={Constants.SimTickMs}ms ({Constants.TicksPerSecond:F1} Hz) | arena=±{Constants.ArenaRadius}m | maxEnemies={Constants.MaxEnemies}");
        Console.WriteLine($"[ArchServer] port={cfg.Port} seed=0x{cfg.Seed:X} godMode={cfg.GodMode} simClients={cfg.SimulatedClients}");

        var world = World.Create();
        var ctx = new SimContext();
        SimBootstrap.ResetGlobals(ctx, cfg.Seed, cfg.GodMode);

        var snapshotScratch = new Snapshot();
        var history = new WorldStateHistory(Constants.SnapshotHistoryTicks);
        var state = ServerLifecycle.Booting;
        var selfProcess = Process.GetCurrentProcess();
        var startupTicks = Stopwatch.GetTimestamp();
        var heartbeatLastTicks = startupTicks;
        var tickStopwatch = new Stopwatch();
        double heartbeatTickMsSum = 0;
        int heartbeatTickCount = 0;
        var prevPhase = RoundPhase.PreGame;
        int prevRound = -1;

        // ---------- QUIC server (real network path) ----------
        QuicServer? quic = null;
        if (cfg.SimulatedClients > 0)
        {
            int n = Math.Min(cfg.SimulatedClients, Constants.MaxPlayers);
            for (byte i = 0; i < n; i++) SpawnPlayer(world, i);
            KickoffMatch(ctx);
            state = ServerLifecycle.Running;
            Console.WriteLine($"[ArchServer] state: Booting → Running tick=0 (spawned {n} sim clients)");
        }
        else
        {
            quic = new QuicServer(cfg.Port);
            quic.ClientConnected += pid =>
            {
                SpawnPlayer(world, pid);
                Console.WriteLine($"[ArchServer] client connected → slot {pid} (Spawn player at arena rim)");
                if (state == ServerLifecycle.Idle)
                {
                    if (cfg.AutoStartMatch) KickoffMatch(ctx);
                    state = ServerLifecycle.Running;
                    Console.WriteLine($"[ArchServer] state: Idle → Running tick={ctx.Tick} (first client)");
                }
            };
            quic.ClientDisconnected += pid =>
            {
                DespawnPlayer(world, pid);
                Console.WriteLine($"[ArchServer] client disconnected ← slot {pid}");
                if (SimBootstrap.CountPlayers(world) == 0 && state == ServerLifecycle.Running)
                {
                    state = ServerLifecycle.Resetting;
                    Console.WriteLine($"[ArchServer] state: Running → Resetting tick={ctx.Tick} (last client gone)");
                }
            };
            quic.InputReceived += (pid, cmd) =>
            {
                ctx.TickInputs.Add(new TickInput
                {
                    PlayerId = pid,
                    Tick = cmd.Tick,
                    LastAckedServerTick = cmd.LastAckedServerTick,
                    Move = cmd.Move,
                    Aim = cmd.Aim,
                    Flags = cmd.Flags,
                });
            };

            quic.Start();
            state = ServerLifecycle.Idle;
            Console.WriteLine($"[ArchServer] state: Booting → Idle tick=0 (waiting for clients)");
        }

        // ---------- Main loop ----------
        // Outer loop = 125 Hz (8 ms). Sim runs every other outer tick = 62.5 Hz.
        // The 2:1 ratio matches DGS (targetFrameRate=125) and Bevy
        // (ScheduleRunnerPlugin::run_loop(1/125)), so per-Update overhead is a
        // constant across the three-server comparison instead of asymmetric.
        // PollEvents / SetServerTick run every 8 ms; the sim block (TickAdvance
        // → BroadcastSnapshot) only runs on every second outer iteration.
        var outerPeriodTicks = Stopwatch.Frequency * Constants.SimTickMs / 1000 / 2; // 8 ms
        var nextTickTime = Stopwatch.GetTimestamp();
        var running = true;
        ulong outerCounter = 0;
        Console.CancelKeyPress += (_, e) => { e.Cancel = true; running = false; };

        while (running)
        {
            var now = Stopwatch.GetTimestamp();
            while (now < nextTickTime)
            {
                var remainingMs = (nextTickTime - now) * 1000.0 / Stopwatch.Frequency;
                if (remainingMs > 2) Thread.Sleep(1);
                now = Stopwatch.GetTimestamp();
            }
            nextTickTime += outerPeriodTicks;

            quic?.PollEvents();
            quic?.SetServerTick(ctx.Tick);

            // Sim fires on every other outer tick (16 ms = 62.5 Hz sim rate).
            bool simThisOuterTick = (++outerCounter & 1UL) == 0;
            if (!simThisOuterTick) continue;

            // ---- Drive sim (matches DedicatedServerMain.DriveSim) ----
            if (state == ServerLifecycle.Running)
            {
                tickStopwatch.Restart();

                TickAdvance.Run(ctx);
                RoundDirector.Run(world, ctx);
                PlayerInput.Run(world, ctx);
                RewindResolve.Run(world, ctx);
                EnemySeek.Run(world, ctx);
                EnemyIntegrate.Run(world);
                PlayerEnemyContact.Run(world, ctx);
                RewindRecord.Run(world, ctx);
                SnapshotCapture.Run(world, ctx, snapshotScratch);
                history.Record(snapshotScratch);

                tickStopwatch.Stop();
                heartbeatTickMsSum += tickStopwatch.Elapsed.TotalMilliseconds;
                heartbeatTickCount++;

                quic?.BroadcastSnapshot(snapshotScratch, history);
            }
            else if (state == ServerLifecycle.Resetting)
            {
                SimBootstrap.ResetGlobals(ctx, cfg.Seed, cfg.GodMode);
                SimBootstrap.DestroyAllEnemies(world);
                history.Clear();
                prevPhase = RoundPhase.PreGame;
                prevRound = -1;
                state = ServerLifecycle.Idle;
                Console.WriteLine($"[ArchServer] state: Resetting → Idle tick={ctx.Tick} (RNG reseeded to 0x{cfg.Seed:X})");
            }

            // ---- Phase / round transition log ----
            if (state == ServerLifecycle.Running && (ctx.Round.Phase != prevPhase || ctx.Round.Round != prevRound))
            {
                int enemies = SimBootstrap.CountEnemies(world);
                int players = SimBootstrap.CountPlayers(world);
                Console.WriteLine($"[Server] state: phase {prevPhase}→{ctx.Round.Phase} round {prevRound}→{ctx.Round.Round} tick={ctx.Tick} enemies={enemies} players={players}");
                prevPhase = ctx.Round.Phase;
                prevRound = ctx.Round.Round;
            }

            // ---- Heartbeat ----
            now = Stopwatch.GetTimestamp();
            var heartbeatElapsed = (now - heartbeatLastTicks) / (double)Stopwatch.Frequency;
            if (cfg.HeartbeatIntervalSec > 0 && heartbeatElapsed >= cfg.HeartbeatIntervalSec)
            {
                heartbeatLastTicks = now;
                LogHeartbeat(world, ctx, state, selfProcess, startupTicks, now, heartbeatElapsed,
                             ref heartbeatTickMsSum, ref heartbeatTickCount, quic);
            }

            // ---- Duration cap ----
            if (cfg.RunForSeconds.HasValue)
            {
                double uptime = (now - startupTicks) / (double)Stopwatch.Frequency;
                if (uptime >= cfg.RunForSeconds.Value) running = false;
            }
        }

        Console.WriteLine($"[ArchServer] state: {state} → ShuttingDown tick={ctx.Tick}");
        quic?.Dispose();
        World.Destroy(world);
        return 0;
    }

    private static readonly QueryDescription PlayerSlotQuery = new QueryDescription().WithAll<PlayerTag, PlayerSlot>();

    public static void DespawnPlayer(World world, byte playerId)
    {
        Entity? found = null;
        world.Query(in PlayerSlotQuery, (Entity e, ref PlayerSlot slot) =>
        {
            if (found is null && slot.Value == playerId) found = e;
        });
        if (found is { } e) world.Destroy(e);
    }

    // ---------- Client lifecycle ----------

    public static void SpawnPlayer(World world, byte playerId)
    {
        float angle = playerId / (float)Constants.MaxPlayers * MathF.PI * 2f;
        var pos = new Vector2(MathF.Cos(angle), MathF.Sin(angle)) * (Constants.ArenaRadius * 0.3f);
        world.Create(
            new PlayerTag(),
            new PlayerSlot { Value = playerId },
            new Position2D { Value = pos },
            new Aim2D { Value = Vector2.UnitX },
            new FireCooldown { Seconds = 0f },
            new DisableTimer { Seconds = 0f },
            Alive.From(true));
    }

    public static void KickoffMatch(SimContext ctx)
    {
        ctx.Round.Phase = RoundPhase.InterRound;
        ctx.Round.Round = 0;
        ctx.Round.InterRoundTimer = Constants.InterRoundDelaySec;
        ctx.Round.RoundTimer = 0f;
    }

    // ---------- Heartbeat ----------

    private static void LogHeartbeat(
        World world, SimContext ctx, ServerLifecycle state, Process selfProcess,
        long startupTicks, long nowTicks, double wallElapsed,
        ref double heartbeatTickMsSum, ref int heartbeatTickCount,
        QuicServer? quic)
    {
        int aliveEnemies = SimBootstrap.CountEnemies(world);
        int activePlayers = SimBootstrap.CountPlayers(world);
        double avgMsPerTick = heartbeatTickCount > 0 ? heartbeatTickMsSum / heartbeatTickCount : 0;
        float expectedTicks = (float)(wallElapsed * Constants.TicksPerSecond);

        double rssMb = 0, vmMb = 0;
        try
        {
            selfProcess.Refresh();
            rssMb = selfProcess.WorkingSet64 / (1024.0 * 1024.0);
            vmMb = selfProcess.VirtualMemorySize64 / (1024.0 * 1024.0);
        }
        catch { /* some Linux configs throw here, skip */ }

        long managedMb = GC.GetTotalMemory(false) / (1024 * 1024);
        double uptime = (nowTicks - startupTicks) / (double)Stopwatch.Frequency;

        string netSuffix = quic is null ? ""
            : $" | net: dgram={quic.DatagramsReceived} batch={quic.InputBatchesParsed} cmds={quic.InputCmdsQueued}";
        Console.WriteLine(
            $"SERVER STATS | RAM (RSS): {rssMb:F2} MB | RAM (VM) : {vmMb:F2} MB | " +
            $"Uptime: {uptime:F1}s | tick={ctx.Tick} bodies={aliveEnemies} state={state}{netSuffix}");

        if (state == ServerLifecycle.Running && ctx.Round.Phase == RoundPhase.InRound)
        {
            Console.WriteLine($"[Server] tick={ctx.Tick} round={ctx.Round.Round}/{Constants.TotalRounds} " +
                              $"elapsed={ctx.Round.RoundTimer:F1}s alive={aliveEnemies} " +
                              $"toSpawn={ctx.Round.SpawnsRemaining}/{ctx.Round.SpawnTarget} " +
                              $"players={activePlayers} ms/tick={avgMsPerTick:F2} ticks={heartbeatTickCount}/{expectedTicks:F0} mem={managedMb}MB");
        }
        else if (state == ServerLifecycle.Running && ctx.Round.Phase == RoundPhase.InterRound)
        {
            Console.WriteLine($"[Server] tick={ctx.Tick} phase=InterRound nextRound={ctx.Round.Round + 1}/{Constants.TotalRounds} " +
                              $"countdown={ctx.Round.InterRoundTimer:F1}s players={activePlayers} " +
                              $"ms/tick={avgMsPerTick:F2} mem={managedMb}MB");
        }

        heartbeatTickMsSum = 0;
        heartbeatTickCount = 0;
    }

    // ---------- CLI ----------

    private static Config ParseArgs(string[] args)
    {
        var cfg = new Config();
        foreach (var raw in args)
        {
            var arg = raw;
            if (arg.StartsWith("--")) arg = arg[2..];
            var eq = arg.IndexOf('=');
            string key = eq >= 0 ? arg[..eq] : arg;
            string val = eq >= 0 ? arg[(eq + 1)..] : "";
            switch (key)
            {
                case "port":            cfg.Port = ushort.Parse(val); break;
                case "seed":            cfg.Seed = ParseSeed(val); break;
                case "auto-start":      cfg.AutoStartMatch = ParseBool(val); break;
                case "god-mode":        cfg.GodMode = ParseBool(val); break;
                case "heartbeat":       cfg.HeartbeatIntervalSec = float.Parse(val, CultureInfo.InvariantCulture); break;
                case "sim-clients":     cfg.SimulatedClients = int.Parse(val); break;
                case "duration":        cfg.RunForSeconds = float.Parse(val, CultureInfo.InvariantCulture); break;
                case "help" or "h" or "?":
                    PrintHelp();
                    Environment.Exit(0);
                    break;
            }
        }
        return cfg;
    }

    private static ulong ParseSeed(string s)
    {
        if (s.StartsWith("0x") || s.StartsWith("0X"))
            return ulong.Parse(s[2..], System.Globalization.NumberStyles.HexNumber, CultureInfo.InvariantCulture);
        return ulong.Parse(s, CultureInfo.InvariantCulture);
    }

    private static bool ParseBool(string s) =>
        s is "" or "1" or "true" or "True" or "yes" or "on";

    private static void PrintHelp()
    {
        Console.WriteLine("DGSvsHS.ArchServer — C# / Arch / BepuPhysics dedicated server");
        Console.WriteLine("Usage: DGSvsHS.ArchServer [options]");
        Console.WriteLine("  --port=N            UDP port (default 7778)");
        Console.WriteLine("  --seed=0xHEX|N      RNG seed (default 0xC0FFEEF00D)");
        Console.WriteLine("  --god-mode[=BOOL]   Disable enemy contact damage (default false)");
        Console.WriteLine("  --heartbeat=SEC     Heartbeat interval (default 1.0; 0 disables)");
        Console.WriteLine("  --sim-clients=N     Spawn N synthetic players (until QUIC server lands)");
        Console.WriteLine("  --duration=SEC      Exit after SEC wall-clock seconds (for trials)");
    }
}
