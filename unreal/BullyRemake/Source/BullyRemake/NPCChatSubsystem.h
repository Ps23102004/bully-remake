// OWNER: ai-npc
// Talk to any NPC, powered by an LLM, with per-NPC long-term memory and optional Fish Audio voice replies.
//
//   - Press T near a student: a chat panel opens (pure-C++ Slate, same approach as the Esc menu / HUD), the NPC
//     stops and faces Jimmy. Type a line, Enter sends. T / Esc closes.
//   - Replies come from any OpenAI-compatible chat endpoint (cloud or a local server): Config/NPCChat.json names
//     the base_url / model / api key file. The system prompt = the ped's persona + its remembered facts + scene.
//   - Memory: when a conversation closes, one summarization call merges the transcript into a small list of facts
//     stored in Saved/NPCChat/<ped>.json; the facts are fed back into every later conversation with that NPC.
//   - Voice: each ped's `voice` (a Fish Audio reference id) speaks the reply via POST /v1/tts (wav), played from
//     the NPC's chest. No key / no credit / empty voice = silent subtitles, never an error the player sees.
//
// Everything is config-driven (Config/NPCChat.json): endpoints, personas, voices, radii. No key material is ever
// stored in the repo; api_key_file paths point at files outside it (e.g. ~/.claude/secrets/fish-audio.key).
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Interfaces/IHttpRequest.h"
#include "NPCChatSubsystem.generated.h"

class ABullyCharacter;
class ABullyPlayerController;
class SNPCChatPanel;
class USoundWaveProcedural;
class UAudioComponent;

/** One speaker-tagged line of the live transcript. */
struct FNPCChatLine
{
	bool bPlayer = false;
	FString Text;
};

/** A ped's persona block from Config/NPCChat.json. */
struct FNPCPersona
{
	FString Name;        // display name, e.g. "Davis"
	FString Voice;       // Fish Audio reference id ("" = no voice)
	FString Persona;     // character sheet injected into the system prompt
};

UCLASS()
class UNPCChatSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** T / console `NPCChat`: open a chat with the nearest NPC in InteractRadius, or close the open one. */
	void Toggle(ABullyPlayerController* PC);
	bool IsChatOpen() const { return Panel.IsValid(); }

	/** Panel callbacks. */
	void SendPlayerLine(const FString& Line);
	void CloseChat();
	const TArray<FNPCChatLine>& GetTranscript() const { return Transcript; }
	FString GetNpcName() const { return Persona.Name; }
	bool IsWaitingForReply() const { return bWaiting; }

private:
	// --- target / lifecycle
	ABullyCharacter* FindNearestNpc(ABullyPlayerController* PC, FString& OutPedKey) const;
	void OpenChat(ABullyPlayerController* PC, ABullyCharacter* Npc, const FString& PedKey);
	void HoldNpc(bool bHold);

	// --- config / persona / memory
	bool LoadConfig();
	FNPCPersona ResolvePersona(const FString& PedKey) const;
	static FString PedKeyOf(const ABullyCharacter* C);
	FString MemoryPath(const FString& PedKey) const;
	TArray<FString> LoadFacts(const FString& PedKey) const;
	void SaveFacts(const FString& PedKey, const TArray<FString>& Facts) const;
	static FString ReadKeyFile(const FString& Path);

	// --- LLM
	FString BuildSystemPrompt() const;
	void RequestReply();
	void OnReply(FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bOk);
	void RequestMemorySummary(const FString& PedKey, TArray<FNPCChatLine> Lines, TArray<FString> OldFacts);
	TSharedRef<class IHttpRequest, ESPMode::ThreadSafe> MakeLlmRequest(const TArray<TSharedPtr<class FJsonValue>>& Messages, float Temperature, int32 MaxTokens) const;

	// --- TTS
	void Speak(const FString& Text);
	void OnSpeech(FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bOk);
	static bool ParseWav(const TArray<uint8>& Bytes, int32& OutRate, int32& OutChannels, TArray<uint8>& OutPcm);

	// config (Config/NPCChat.json)
	FString LlmBaseUrl, LlmModel, LlmKey;
	float LlmTemperature = 0.9f;
	int32 LlmMaxTokens = 180;
	FString TtsBaseUrl, TtsModel, TtsKey;
	bool bTtsEnabled = true;
	float InteractRadius = 350.f;
	TArray<FString> WorldFacts;
	TMap<FString, FNPCPersona> Personas;
	bool bConfigLoaded = false;

	// live conversation
	TWeakObjectPtr<ABullyPlayerController> Controller;
	TWeakObjectPtr<ABullyCharacter> NpcRef;
	FString NpcPedKey;
	FNPCPersona Persona;
	TArray<FString> Facts;
	TArray<FNPCChatLine> Transcript;
	bool bWaiting = false;
	TSharedPtr<SNPCChatPanel> Panel;
	TWeakPtr<IHttpRequest> PendingReply;

	UPROPERTY(Transient) TObjectPtr<USoundWaveProcedural> SpeechWave;
	UPROPERTY(Transient) TObjectPtr<UAudioComponent> SpeechAudio;

	// headless smoke (-BRNPCChatSmoke, nullrhi): open a chat with the nearest NPC, send one line, wait for the
	// LLM reply, close (fires the memory summary), then verify Saved/NPCChat/<ped>.json appears. Logs
	// "BR_NPCCHAT SMOKE OK/FAIL" for the harness to grep.
	void RunSmoke(float Dt);
	FTSTicker::FDelegateHandle SmokeTicker;
	float SmokeTime = 0.f;
	int32 SmokeStep = 0;
	FString SmokePed;
};
