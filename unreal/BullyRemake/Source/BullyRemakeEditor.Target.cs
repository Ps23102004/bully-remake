using UnrealBuildTool;

public class BullyRemakeEditorTarget : TargetRules
{
	public BullyRemakeEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
		ExtraModuleNames.Add("BullyRemake");
	}
}
