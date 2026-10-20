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

		// NPCChatSubsystem (ai-npc): OpenAI-compatible LLM chat + Fish Audio TTS over HTTP; push-to-talk mic capture
		// transcribed by a local whisper.cpp binary.
		PrivateDependencyModuleNames.AddRange(new string[] { "HTTP", "AudioCapture", "AudioCaptureCore" });

		// BullyPlayerController: the Esc settings menu is a pure-C++ Slate overlay (no editor-authored UMG asset).
		PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });
	}
}
