// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class ForgottenThird : ModuleRules
{
	public ForgottenThird(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "ForgottenCore" });

		PrivateDependencyModuleNames.AddRange(new string[] {  });

		// The game reads its JSON data (story, face presets, emotion map) from the project's Data/
		// folder at run time. Stage it next to the packaged game at the same relative path.
		RuntimeDependencies.Add("$(ProjectDir)/Data/...", StagedFileType.NonUFS);

		if (Target.bBuildEditor)
		{
			// The demo-world, character-import and rig commandlets.
			PrivateDependencyModuleNames.AddRange(new string[] { "UnrealEd", "AssetRegistry", "AssetTools", "IKRig", "IKRigEditor", "Slate", "SlateCore" });
		}

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });
		
		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
