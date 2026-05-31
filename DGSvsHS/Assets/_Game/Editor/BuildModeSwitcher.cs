using System.Collections.Generic;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace DGSvsHS.EditorTools
{
    /// <summary>
    /// One-click switcher between the build flavors used by the DGSvsHS paper:
    /// <list type="bullet">
    ///   <item><b>DGS</b> — Unity DOTS dedicated server. Define: <c>WITH_DGS</c>. Stripping: Low.</item>
    ///   <item><b>HS — Arch (QUIC 7778)</b> — Client connecting to the C#/Arch server.
    ///         Defines: <c>WITH_HS</c> + <c>HS_TARGET_ARCH</c>. Stripping: Low.
    ///         Picks port 7778 by default; same wire protocol as Bevy.</item>
    ///   <item><b>HS — Bevy (QUIC 4433)</b> — Client connecting to the Rust/Bevy/Avian server.
    ///         Defines: <c>WITH_HS</c> + <c>HS_TARGET_BEVY</c>. Stripping: Low.
    ///         Picks port 4433 by default; same wire protocol as Arch.</item>
    ///   <item><b>BareBone</b> — Minimal Unity listener for the green-computing baseline.
    ///         Define: <c>WITH_BAREBONE</c>. Stripping: Low (was High before; matched to the rest
    ///         to avoid stripping confounding the comparison).</item>
    /// </list>
    ///
    /// <para>Both HS flavors share <c>WITH_HS</c> as the umbrella define so all existing
    /// <c>#if !WITH_DGS</c> compile gates (QUIC transport, client side) keep working without
    /// changes. The sub-define (<c>HS_TARGET_ARCH</c> vs <c>HS_TARGET_BEVY</c>) only affects
    /// the default port picked in <see cref="DGSvsHS.Client.ClientMain"/> so the Inspector
    /// matches the right server out of the box; the wire format is identical.</para>
    ///
    /// <para>Modes are mutually exclusive. Switching applies to BOTH
    /// <see cref="NamedBuildTarget.Standalone"/> and <see cref="NamedBuildTarget.Server"/>.</para>
    /// </summary>
    public static class BuildModeSwitcher
    {
        private const string MenuRoot      = "DGSvsHS/Build Mode/";
        // Menu labels intentionally contain no `/` outside the deliberate hierarchy — Unity
        // splits menu paths on every `/`, so writing "C#/Arch" or "Rust/Bevy" in a label would
        // accidentally nest each into its own submenu. The two HS targets live as siblings
        // under a real HS submenu so the user sees: HS → Arch, HS → Bevy.
        private const string MenuDGS       = MenuRoot + "DGS — Unity DOTS Server";
        private const string MenuHSArch    = MenuRoot + "HS/Arch — C# server (QUIC 7778)";
        private const string MenuHSBevy    = MenuRoot + "HS/Bevy — Rust server (QUIC 4433)";
        private const string MenuBareBone  = MenuRoot + "BareBone — Minimal Listener Baseline";
        private const string MenuShow      = MenuRoot + "Show Current";

        private const string DefineDGS         = "WITH_DGS";
        private const string DefineHS          = "WITH_HS";          // umbrella — set whenever any HS sub-flavor is active
        private const string DefineHSArch      = "HS_TARGET_ARCH";   // sub-flavor: client points at Arch by default
        private const string DefineHSBevy      = "HS_TARGET_BEVY";   // sub-flavor: client points at Bevy by default
        private const string DefineBareBone    = "WITH_BAREBONE";

        // All mode-related defines we own — anything outside this list is preserved across switches.
        private static readonly string[] ModeDefines = { DefineDGS, DefineHS, DefineHSArch, DefineHSBevy, DefineBareBone };

        // ---------- Menu items ----------

        [MenuItem(MenuDGS, priority = 0)]
        public static void SetModeDGS()      => Apply("DGS",      new[] { DefineDGS },                  ManagedStrippingLevel.Low, clientPort: 7777);

        [MenuItem(MenuHSArch, priority = 1)]
        public static void SetModeHSArch()   => Apply("HS-Arch",  new[] { DefineHS, DefineHSArch },     ManagedStrippingLevel.Low, clientPort: 7778);

        [MenuItem(MenuHSBevy, priority = 2)]
        public static void SetModeHSBevy()   => Apply("HS-Bevy",  new[] { DefineHS, DefineHSBevy },     ManagedStrippingLevel.Low, clientPort: 4433);

        [MenuItem(MenuBareBone, priority = 3)]
        public static void SetModeBareBone() => Apply("BareBone", new[] { DefineBareBone },             ManagedStrippingLevel.Low, clientPort: 7779);

        [MenuItem(MenuShow, priority = 20)]
        public static void ShowCurrent()
        {
            var serverDefines = GetDefines(NamedBuildTarget.Server);
            var standaloneDefines = GetDefines(NamedBuildTarget.Standalone);
            var serverStrip = PlayerSettings.GetManagedStrippingLevel(NamedBuildTarget.Server);
            var standaloneStrip = PlayerSettings.GetManagedStrippingLevel(NamedBuildTarget.Standalone);
            string activeMode = DetectActiveMode(serverDefines);
            Debug.Log(
                $"[BuildMode] Active: {activeMode}\n" +
                $"  Server     defines: {string.Join(";", serverDefines)} | stripping: {serverStrip}\n" +
                $"  Standalone defines: {string.Join(";", standaloneDefines)} | stripping: {standaloneStrip}");
        }

        // ---------- Checkmark validation ----------

        [MenuItem(MenuDGS,      validate = true)] public static bool ValidateDGS()      { Menu.SetChecked(MenuDGS,      IsExactMode(DefineDGS));     return true; }
        [MenuItem(MenuHSArch,   validate = true)] public static bool ValidateHSArch()   { Menu.SetChecked(MenuHSArch,   IsExactMode(DefineHSArch));  return true; }
        [MenuItem(MenuHSBevy,   validate = true)] public static bool ValidateHSBevy()   { Menu.SetChecked(MenuHSBevy,   IsExactMode(DefineHSBevy));  return true; }
        [MenuItem(MenuBareBone, validate = true)] public static bool ValidateBareBone() { Menu.SetChecked(MenuBareBone, IsExactMode(DefineBareBone));return true; }

        // ---------- Implementation ----------

        private static void Apply(string label, string[] keepDefines, ManagedStrippingLevel strip, ushort? clientPort)
        {
            ApplyTo(NamedBuildTarget.Standalone, keepDefines, strip);
            ApplyTo(NamedBuildTarget.Server,     keepDefines, strip);
            AssetDatabase.SaveAssets();

            // The #if conditional in ClientMain only controls the field's COMPILE-TIME default;
            // any value already serialized into Client.unity wins at runtime. Rewrite the scene's
            // serialized Port so the Inspector matches the freshly selected mode without manual
            // editing. BareBone passes its own server port (7779) but there's typically no
            // ClientMain pointing at it — the rewrite is a no-op when no component is found.
            if (clientPort.HasValue) UpdateClientMainPortInScene(clientPort.Value);

            string portMsg = clientPort.HasValue ? $" | Client.unity ClientMain.Port={clientPort.Value}" : "";
            Debug.Log($"[BuildMode] → {label} | defines={string.Join(",", keepDefines)} | stripping={strip} (Standalone + Server){portMsg}");
        }

        // ---------- Client scene Port rewrite ----------

        private const string ClientScenePath = "Assets/Scenes/Client.unity";
        private const string ClientMainTypeName = "DGSvsHS.Client.ClientMain";

        /// <summary>
        /// Find every <see cref="ClientMainTypeName"/> component in <see cref="ClientScenePath"/>
        /// and set its serialized <c>Port</c> field to <paramref name="port"/>. Loads the scene
        /// additively if not already open, then saves + closes it cleanly so the user's currently
        /// open scenes are not disturbed.
        /// </summary>
        private static void UpdateClientMainPortInScene(ushort port)
        {
            if (!System.IO.File.Exists(ClientScenePath))
            {
                Debug.LogWarning($"[BuildMode] {ClientScenePath} not found — skipping ClientMain.Port update");
                return;
            }

            Scene scene = EditorSceneManager.GetSceneByPath(ClientScenePath);
            bool openedHere = false;
            if (!scene.IsValid() || !scene.isLoaded)
            {
                scene = EditorSceneManager.OpenScene(ClientScenePath, OpenSceneMode.Additive);
                openedHere = true;
            }

            int updatedCount = 0;
            foreach (var root in scene.GetRootGameObjects())
            {
                // Reflect by full type name to avoid forcing this Editor asmdef to reference the
                // DGSvsHS.Client runtime assembly — keeps the switcher functional even in modes
                // where ClientMain isn't compiled in (e.g. when WITH_DGS strips QUIC-side types).
                foreach (var mb in root.GetComponentsInChildren<MonoBehaviour>(includeInactive: true))
                {
                    if (mb == null) continue;
                    if (mb.GetType().FullName != ClientMainTypeName) continue;

                    var so = new SerializedObject(mb);
                    var portProp = so.FindProperty("Port");
                    if (portProp == null)
                    {
                        Debug.LogWarning($"[BuildMode] {ClientMainTypeName} has no serialized 'Port' field — was it renamed?");
                        continue;
                    }
                    if (portProp.intValue == port) continue;

                    portProp.intValue = port;
                    so.ApplyModifiedPropertiesWithoutUndo();
                    updatedCount++;
                }
            }

            if (updatedCount > 0)
            {
                EditorSceneManager.MarkSceneDirty(scene);
                EditorSceneManager.SaveScene(scene);
            }

            if (openedHere) EditorSceneManager.CloseScene(scene, removeScene: true);
        }

        private static void ApplyTo(NamedBuildTarget target, string[] keepDefines, ManagedStrippingLevel strip)
        {
            // Drop all of our mode defines, then re-add only the ones for the selected flavor.
            // Preserves any other defines the project might have (UNITY_ANALYTICS, third-party flags, …).
            var current = new HashSet<string>(GetDefines(target));
            foreach (var m in ModeDefines) current.Remove(m);
            foreach (var k in keepDefines) current.Add(k);

            var arr = new string[current.Count];
            current.CopyTo(arr);
            PlayerSettings.SetScriptingDefineSymbols(target, arr);
            PlayerSettings.SetManagedStrippingLevel(target, strip);
        }

        private static string[] GetDefines(NamedBuildTarget target)
        {
            PlayerSettings.GetScriptingDefineSymbols(target, out string[] defs);
            return defs ?? System.Array.Empty<string>();
        }

        /// <summary>True iff the Server target has exactly the mode signature for <paramref name="primary"/>.</summary>
        private static bool IsExactMode(string primary)
        {
            var defs = new HashSet<string>(GetDefines(NamedBuildTarget.Server));
            return primary switch
            {
                DefineDGS      => defs.Contains(DefineDGS)      && !defs.Contains(DefineHS) && !defs.Contains(DefineBareBone),
                DefineHSArch   => defs.Contains(DefineHS) && defs.Contains(DefineHSArch) && !defs.Contains(DefineHSBevy) && !defs.Contains(DefineDGS) && !defs.Contains(DefineBareBone),
                DefineHSBevy   => defs.Contains(DefineHS) && defs.Contains(DefineHSBevy) && !defs.Contains(DefineHSArch) && !defs.Contains(DefineDGS) && !defs.Contains(DefineBareBone),
                DefineBareBone => defs.Contains(DefineBareBone) && !defs.Contains(DefineHS) && !defs.Contains(DefineDGS),
                _ => false,
            };
        }

        private static string DetectActiveMode(string[] defines)
        {
            var d = new HashSet<string>(defines);
            bool dgs  = d.Contains(DefineDGS);
            bool hs   = d.Contains(DefineHS);
            bool bb   = d.Contains(DefineBareBone);
            bool arch = d.Contains(DefineHSArch);
            bool bevy = d.Contains(DefineHSBevy);

            if (dgs && !hs && !bb)              return "DGS";
            if (hs && arch && !bevy && !dgs && !bb) return "HS-Arch (port 7778)";
            if (hs && bevy && !arch && !dgs && !bb) return "HS-Bevy (port 4433)";
            if (hs && !arch && !bevy)           return "HS (NO sub-flavor — pick HS-Arch or HS-Bevy)";
            if (bb && !hs && !dgs)              return "BareBone";
            if (!dgs && !hs && !bb)             return "(none — pick a mode)";
            return "AMBIGUOUS (multiple mode defines set — pick one to clean up)";
        }
    }
}
