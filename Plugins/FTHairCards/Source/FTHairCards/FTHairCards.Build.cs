using System.IO;
using UnrealBuildTool;

public class FTHairCards : ModuleRules
{
	public FTHairCards(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
		PrivateDependencyModuleNames.AddRange(new string[] { "UnrealEd", "HairStrandsCore", "HairCardGeneratorEditor", "ForgottenCore" });

		// The generator's settings class is exported, but its header (and the strand filter header it includes) is private to the generator.
		PrivateIncludePaths.Add(Path.Combine(GetModuleDirectory("HairCardGeneratorEditor"), "Private"));
	}
}
