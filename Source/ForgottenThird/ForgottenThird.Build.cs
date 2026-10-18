// Copyright Epic Games, Inc. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

public class ForgottenThird : ModuleRules
{
	public ForgottenThird(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "ForgottenCore", "Niagara" });

		PrivateDependencyModuleNames.AddRange(new string[] {  });

		// The game reads its JSON data (story, face presets, emotion map) from the project's Data/
		// folder at run time. Stage it next to the packaged game at the same relative path.
		RuntimeDependencies.Add("$(ProjectDir)/Data/...", StagedFileType.NonUFS);

		if (Target.bBuildEditor)
		{
			// The demo-world, character-import and rig commandlets.
			PrivateDependencyModuleNames.AddRange(new string[] { "UnrealEd", "AssetRegistry", "AssetTools", "IKRig", "IKRigEditor", "Slate", "SlateCore" });

			// FTBuildVfx builds Niagara Lightweight (stateless) emitters. Their module classes live in
			// Niagara's Internal headers, which UBT only exposes to engine-scope modules, so add the paths.
			PrivateDependencyModuleNames.AddRange(new string[] { "NiagaraCore", "NiagaraShader", "NiagaraEditor", "RenderCore", "RHI" });
			string NiagaraSource = Path.Combine(EngineDirectory, "Plugins", "FX", "Niagara", "Source");
			PrivateIncludePaths.Add(Path.Combine(NiagaraSource, "Niagara", "Internal"));
			PrivateIncludePaths.Add(Path.Combine(NiagaraSource, "NiagaraShader", "Internal"));
		}

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });
		
		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
