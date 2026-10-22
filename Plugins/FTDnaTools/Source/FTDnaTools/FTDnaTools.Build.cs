
using UnrealBuildTool;

public class FTDnaTools : ModuleRules
{
	public FTDnaTools(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
		PrivateDependencyModuleNames.AddRange(new string[] { "RigLogicModule", "DNACalibModule", "MeshDescription" });
	}
}
