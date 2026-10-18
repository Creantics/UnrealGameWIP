using System.IO;
using UnrealBuildTool;

// Wraps the plain C++20 library in Core/ so Unreal compiles it with the rest of the game.
// UBT only compiles files under this directory, so Private/Core/ holds one wrapper .cpp per
// Core/src/*.cpp that #includes it. Add a wrapper whenever you add a source file to Core.
public class ForgottenCore : ModuleRules
{
	public ForgottenCore(ReadOnlyTargetRules Target) : base(Target)
	{
		// No shared PCH, so Core sources never see Unreal headers.
		PCHUsage = PCHUsageMode.NoPCHs;
		bUseUnity = false;
		CppStandard = CppStandardVersion.Cpp20;

		string CoreRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "..", "Core"));
		PublicIncludePaths.Add(Path.Combine(CoreRoot, "include"));
		PrivateIncludePaths.Add(CoreRoot);

		// Only for IMPLEMENT_MODULE in ForgottenCoreModule.cpp.
		PrivateDependencyModuleNames.Add("Core");
	}
}
