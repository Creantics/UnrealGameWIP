using UnrealBuildTool;

public class FTGroomTools : ModuleRules
{
	public FTGroomTools(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
		PrivateDependencyModuleNames.AddRange(new string[] { "HairStrandsCore", "MeshDescription" });
	}
}
