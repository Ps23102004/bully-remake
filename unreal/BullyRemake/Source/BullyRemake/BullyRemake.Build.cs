using UnrealBuildTool;

public class BullyRemake : ModuleRules
{
	public BullyRemake(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput"
		});

		// BullyShotRunner: JSON shot lists for offscreen evidence captures.
		PrivateDependencyModuleNames.AddRange(new string[] { "Json" });
	}
}
