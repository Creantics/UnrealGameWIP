using UnrealBuildTool;

public class FTLook : ModuleRules
{
	public FTLook(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
		// The grade is a global shader drawn from a scene view extension, so the renderer's modules are needed in every
		// build, not only the editor's.
		PrivateDependencyModuleNames.AddRange(new string[] { "RenderCore", "RHI", "Renderer", "Projects" });
	}
}
