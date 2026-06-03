namespace DGSvsHS.ArchServer.Server.Systems;

public static class TickAdvance
{
    public static void Run(SimContext ctx)
    {
        ctx.Tick++;
        ctx.FireEvents.Clear();
    }
}
