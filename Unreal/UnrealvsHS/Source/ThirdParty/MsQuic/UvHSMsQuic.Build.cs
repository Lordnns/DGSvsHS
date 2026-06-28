// ThirdParty wrapper for Microsoft's msquic library — same QUIC stack the
// Arch (C#) server uses via StirlingLabs.MsQuic. Mirroring at the transport
// layer makes the Arch ↔ Bevy ↔ Unreal server comparison fair (all three
// negotiate the same QUIC handshake, datagram framing, congestion control).
//
// Binaries are NOT vendored in this folder — see README.md for download
// + placement instructions. Build.cs fails fast at link time with a clear
// error if the expected files are missing.

using UnrealBuildTool;
using System.IO;

// Module renamed from "MsQuic" → "UvHSMsQuic" because Unreal has an internal
// MsQuicRuntime engine plugin / MsQuic module pair, and UBT rejects project
// modules whose names collide with engine-plugin modules (the cross-hierarchy
// reference rule). Same code, just a unique name.
public class UvHSMsQuic : ModuleRules
{
	public UvHSMsQuic(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;

		string IncludeDir = Path.Combine(ModuleDirectory, "include");
		string LibDir     = Path.Combine(ModuleDirectory, "lib");

		PublicSystemIncludePaths.Add(IncludeDir);

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			string WinLibDir = Path.Combine(LibDir, "Win64");
			string WinLib = Path.Combine(WinLibDir, "msquic.lib");
			PublicAdditionalLibraries.Add(WinLib);

			// Ship msquic.dll plus any auxiliary DLLs the variant brings.
			// The MsQuic.OpenSSL3 NuGet may include libcrypto-3-x64.dll /
			// libssl-3-x64.dll alongside msquic.dll — drop_msquic.sh copies
			// every .dll from the NuGet's x64 dir, and we forward them all
			// to the binary output dir as RuntimeDependencies.
			if (Directory.Exists(WinLibDir))
			{
				foreach (string Dll in Directory.GetFiles(WinLibDir, "*.dll"))
				{
					string DllName = Path.GetFileName(Dll);
					RuntimeDependencies.Add("$(BinaryOutputDir)/" + DllName, Dll);
				}
			}
			PublicDelayLoadDLLs.Add("msquic.dll");
		}
		else if (Target.Platform == UnrealTargetPlatform.Linux)
		{
			// UBT's Linux toolchain converts a `.so.<N>` path into
			// `-l<basename-no-libprefix-no-version>` which lld can't resolve
			// (it would need `-l:libmsquic.so.2` to honor the literal name).
			// Workaround: link against an unversioned `libmsquic.so` copy of
			// the same file. The SONAME embedded inside the .so still points
			// to `libmsquic.so.2`, so the runtime loader resolves against
			// the versioned copy we ship via RuntimeDependencies.
			string LinSoVer = Path.Combine(LibDir, "Linux", "libmsquic.so.2");
			RuntimeDependencies.Add("$(BinaryOutputDir)/libmsquic.so.2", LinSoVer);
			// Removed PublicAdditionalLibraries.Add(LinSo) to prevent dynamic loader
			// from binding MsQuic globally before main(), which causes OpenSSL 
			// symbol collisions with Unreal's statically linked OpenSSL 1.1.
			// We now load it manually via dlopen with RTLD_DEEPBIND.
		}
		else
		{
			throw new BuildException("MsQuic: unsupported platform " + Target.Platform);
		}
	}
}
