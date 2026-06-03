using System.Collections.Generic;
using System.Numerics;
using DGSvsHS.Gameplay;

namespace DGSvsHS.ArchServer.Net;

public static class SnapshotPriority
{
    public struct ScoredEnemy
    {
        public int Index;
        public float Score;
    }

    private static readonly System.Comparison<ScoredEnemy> ScoreAscending = (a, b) => a.Score.CompareTo(b.Score);

    public static void SelectForFull(
        Snapshot current,
        Vector2 recipientPos,
        int enemyByteBudget,
        List<EnemySnap> outSelected,
        List<ScoredEnemy> scratchScored)
    {
        outSelected.Clear();
        scratchScored.Clear();

        for (int i = 0; i < current.Enemies.Count; i++)
        {
            var e = current.Enemies[i];
            float dx = e.Position.X - recipientPos.X;
            float dy = e.Position.Y - recipientPos.Y;
            scratchScored.Add(new ScoredEnemy { Index = i, Score = dx * dx + dy * dy });
        }
        scratchScored.Sort(ScoreAscending);

        int remaining = enemyByteBudget;
        int sz = WireCodec.EnemySnapFullBytes;
        for (int s = 0; s < scratchScored.Count; s++)
        {
            if (sz > remaining) break;
            remaining -= sz;
            outSelected.Add(current.Enemies[scratchScored[s].Index]);
        }
    }

    public static void SelectForDelta(
        Snapshot current,
        Snapshot baseline,
        Vector2 recipientPos,
        HashSet<ushort> confirmedIds,
        IReadOnlyDictionary<ushort, ushort> ticksSinceLastSent,
        int enemyByteBudget,
        List<EnemyDeltaEntry> outChanged,
        List<ushort> outRemoved,
        List<EnemySnap> outAdded,
        HashSet<ushort> includedIds,
        List<ScoredEnemy> scratchScored,
        HashSet<ushort> scratchCurrentIds,
        Dictionary<ushort, int> scratchBaselineIndexById)
    {
        outChanged.Clear();
        outRemoved.Clear();
        outAdded.Clear();
        includedIds.Clear();
        scratchScored.Clear();
        scratchCurrentIds.Clear();
        scratchBaselineIndexById.Clear();

        var currentIds = scratchCurrentIds;
        for (int i = 0; i < current.Enemies.Count; i++) currentIds.Add(current.Enemies[i].Id);
        if (confirmedIds != null)
        {
            foreach (ushort cid in confirmedIds)
            {
                if (currentIds.Contains(cid)) continue;
                outRemoved.Add(cid);
            }
        }
        else
        {
            for (int i = 0; i < baseline.Enemies.Count; i++)
            {
                ushort bid = baseline.Enemies[i].Id;
                if (currentIds.Contains(bid)) continue;
                outRemoved.Add(bid);
            }
        }

        var baselineIndexById = scratchBaselineIndexById;
        for (int i = 0; i < baseline.Enemies.Count; i++) baselineIndexById[baseline.Enemies[i].Id] = i;

        // Pack removes first — they're tiny (2 B each) and dropping them risks ghost enemies.
        int removedBytes = outRemoved.Count * 2;
        if (removedBytes > enemyByteBudget)
        {
            int keepable = enemyByteBudget / 2;
            if (keepable < outRemoved.Count)
                outRemoved.RemoveRange(keepable, outRemoved.Count - keepable);
            removedBytes = outRemoved.Count * 2;
        }
        int remaining = enemyByteBudget - removedBytes;
        if (remaining < 0) remaining = 0;

        // ----- Phase A: spawn lane — current ids the recipient has not yet seen -----
        scratchScored.Clear();
        bool haveConfirmed = confirmedIds != null;
        for (int i = 0; i < current.Enemies.Count; i++)
        {
            var e = current.Enemies[i];
            bool isPendingSpawn = !haveConfirmed || !confirmedIds.Contains(e.Id);
            if (!isPendingSpawn) continue;
            float dx = e.Position.X - recipientPos.X;
            float dy = e.Position.Y - recipientPos.Y;
            scratchScored.Add(new ScoredEnemy { Index = i, Score = dx * dx + dy * dy });
        }
        scratchScored.Sort(ScoreAscending);
        int spawnsThisSnapshot = 0;
        int newEntrySize = WireCodec.EnemySnapFullBytes;
        for (int s = 0; s < scratchScored.Count; s++)
        {
            if (spawnsThisSnapshot >= Constants.MaxSpawnsPerSnapshot) break;
            if (remaining < newEntrySize) break;
            int idx = scratchScored[s].Index;
            var e = current.Enemies[idx];
            outAdded.Add(e);
            includedIds.Add(e.Id);
            remaining -= newEntrySize;
            spawnsThisSnapshot++;
        }

        // ----- Phase B: animation lane — confirmed-and-in-baseline enemies whose position changed -----
        scratchScored.Clear();
        for (int i = 0; i < current.Enemies.Count; i++)
        {
            var e = current.Enemies[i];
            if (!haveConfirmed || !confirmedIds.Contains(e.Id)) continue; // already covered by spawn lane
            if (!baselineIndexById.ContainsKey(e.Id)) continue;            // no baseline value to diff against
            float dx = e.Position.X - recipientPos.X;
            float dy = e.Position.Y - recipientPos.Y;
            float dist = MathF.Sqrt(dx * dx + dy * dy);
            int tsls = 0;
            if (ticksSinceLastSent != null && ticksSinceLastSent.TryGetValue(e.Id, out ushort t)) tsls = t;
            float score = dist - Constants.StalenessWeight * tsls;
            scratchScored.Add(new ScoredEnemy { Index = i, Score = score });
        }
        scratchScored.Sort(ScoreAscending);
        int changedEntrySize = WireCodec.EnemyDeltaEntryBytes;
        for (int s = 0; s < scratchScored.Count; s++)
        {
            int idx = scratchScored[s].Index;
            var e = current.Enemies[idx];
            int baseIdx = baselineIndexById[e.Id];
            var b = baseline.Enemies[baseIdx];
            if (!WireCodec.EnemyPositionChanged(b, e))
            {
                includedIds.Add(e.Id);
                continue;
            }
            if (changedEntrySize > remaining) break;
            remaining -= changedEntrySize;
            outChanged.Add(new EnemyDeltaEntry { Id = e.Id, Position = e.Position });
            includedIds.Add(e.Id);
        }
    }
}
