using System.Numerics;
using Arch.Core;
using DGSvsHS.Gameplay;

namespace DGSvsHS.ArchServer.Server.Systems;

public static class PlayerInput
{
    private static readonly QueryDescription AllPlayers = new QueryDescription()
        .WithAll<PlayerTag, PlayerSlot, Position2D, Aim2D, FireCooldown, DisableTimer, Alive>();

    public static void Run(World world, SimContext ctx)
    {
        int max = Constants.MaxPlayers;
        Span<int> latestIdx = stackalloc int[max];
        Span<uint> latestTick = stackalloc uint[max];
        for (int i = 0; i < max; i++) { latestIdx[i] = -1; latestTick[i] = 0; }

        var inputs = ctx.TickInputs;
        for (int i = 0; i < inputs.Count; i++)
        {
            var input = inputs[i];
            if (input.PlayerId >= max) continue;
            if (latestIdx[input.PlayerId] < 0 || input.Tick > latestTick[input.PlayerId])
            {
                latestIdx[input.PlayerId] = i;
                latestTick[input.PlayerId] = input.Tick;
            }
        }

        for (int i = 0; i < inputs.Count; i++)
        {
            var input = inputs[i];
            if (!input.Fire) continue;
            if (input.PlayerId >= max) continue;
            if (!TryGetPlayerSnapshot(world, input.PlayerId, out var pos, out var aim, out var alive)) continue;
            if (!alive) continue;

            Vector2 dir = input.Aim.LengthSquared() > 0.0001f ? Vector2.Normalize(input.Aim) : aim;
            ctx.PendingFires.Add(new PendingFire
            {
                PlayerId = input.PlayerId,
                ClientInputTick = input.Tick,
                Origin = pos,
                Direction = dir,
            });
        }

        int[] latestIdxArr = new int[max];
        for (int i = 0; i < max; i++) latestIdxArr[i] = latestIdx[i];

        world.Query(in AllPlayers, (
            ref PlayerSlot slot, ref Position2D pos, ref Aim2D aim,
            ref FireCooldown cd, ref DisableTimer dt, ref Alive alive) =>
        {
            byte pid = slot.Value;
            if (!alive.Bool)
            {
                cd.Seconds = MathF.Max(0f, cd.Seconds - Constants.SimDt);
                dt.Seconds = MathF.Max(0f, dt.Seconds - Constants.SimDt);
                return;
            }

            int idx = latestIdxArr[pid];
            if (idx < 0)
            {
                cd.Seconds = MathF.Max(0f, cd.Seconds - Constants.SimDt);
                dt.Seconds = MathF.Max(0f, dt.Seconds - Constants.SimDt);
                return;
            }

            var input = inputs[idx];

            Vector2 move = input.Move;
            float mag = move.Length();
            if (mag > 1f) move /= mag;
            Vector2 newPos = pos.Value + move * Constants.PlayerSpeed * Constants.SimDt;

            float r = newPos.Length();
            float maxR = Constants.ArenaRadius - Constants.PlayerRadius;
            if (r > maxR) newPos *= maxR / r;
            pos.Value = newPos;

            if (input.Aim.LengthSquared() > 0.0001f)
                aim.Value = Vector2.Normalize(input.Aim);

            cd.Seconds = MathF.Max(0f, cd.Seconds - Constants.SimDt);
            dt.Seconds = MathF.Max(0f, dt.Seconds - Constants.SimDt);
        });

        ctx.TickInputs.Clear();
    }

    private static readonly QueryDescription PlayerSnapshotQuery = new QueryDescription()
        .WithAll<PlayerTag, PlayerSlot, Position2D, Aim2D, Alive>();

    private static bool TryGetPlayerSnapshot(World world, byte slotId, out Vector2 pos, out Vector2 aim, out bool alive)
    {
        Vector2 foundPos = default;
        Vector2 foundAim = Vector2.UnitX;
        bool foundAlive = false;
        bool found = false;
        world.Query(in PlayerSnapshotQuery, (ref PlayerSlot s, ref Position2D p, ref Aim2D a, ref Alive al) =>
        {
            if (found || s.Value != slotId) return;
            foundPos = p.Value;
            foundAim = a.Value;
            foundAlive = al.Bool;
            found = true;
        });
        pos = foundPos; aim = foundAim; alive = foundAlive;
        return found;
    }
}
