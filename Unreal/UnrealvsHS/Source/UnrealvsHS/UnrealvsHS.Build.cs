// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class UnrealvsHS : ModuleRules
{
	public UnrealvsHS(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"AIModule",
			"NavigationSystem",
			"StateTreeModule",
			"GameplayStateTreeModule",
			"Niagara",
			"UMG",
			"Slate",
			"Chaos",
			"PhysicsCore",
			"Sockets",
			"Networking"
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			"UvHSMsQuic",
			"OpenSSL",
			"MassEntity",
			"MassCommon",
			"StructUtils",
		});

		PublicIncludePaths.AddRange(new string[] {
			"UnrealvsHS",
			"UnrealvsHS/Client",
			"UnrealvsHS/Gameplay",
			"UnrealvsHS/Mass",
			"UnrealvsHS/Net",
			"UnrealvsHS/Server",
		});

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
