// OWNER: ai-npc
#include "NPCChatSubsystem.h"

#include "AmbientWalker.h"
#include "Async/Async.h"
#include "BullyCharacter.h"
#include "BullyPlayerController.h"
#include "BullyScriptedNPC.h"
#include "Components/AudioComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Sound/SoundWaveProcedural.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Text/STextBlock.h"

DEFINE_LOG_CATEGORY_STATIC(LogNPCChat, Log, All);

// ---------------------------------------------------------------------------------------------------------------
// Slate panel: NPC name + the last lines + an input box, bottom-centre. The world keeps running behind it.

class SNPCChatPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SNPCChatPanel) {}
		SLATE_ARGUMENT(UNPCChatSubsystem*, Owner)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Owner = InArgs._Owner;
		const FSlateFontInfo Name = FCoreStyle::GetDefaultFontStyle("Bold", 18);
		const FSlateFontInfo Line = FCoreStyle::GetDefaultFontStyle("Regular", 15);
		const FSlateFontInfo Small = FCoreStyle::GetDefaultFontStyle("Regular", 12);
		const FLinearColor Text(0.95f, 0.93f, 0.88f, 1.f);
		const FLinearColor Dim(0.62f, 0.62f, 0.66f, 1.f);
		const FLinearColor Accent(0.95f, 0.72f, 0.20f, 1.f);

		ChildSlot
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().FillHeight(1.f) // transparent spacer: the panel hugs the bottom, the world stays visible
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0, 0, 0, 36)
			[
				SNew(SBox).WidthOverride(760.f)
				[
					SNew(SBorder)
					.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
					.BorderBackgroundColor(FLinearColor(0.04f, 0.04f, 0.06f, 0.82f))
					.Padding(FMargin(20.f, 14.f))
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(STextBlock).Font(Name).ColorAndOpacity(Accent)
							.Text_Lambda([this]() { return FText::FromString(Owner ? Owner->GetNpcName() : FString()); })
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 8)
						[
							SNew(SBox).MaxDesiredHeight(170.f)
							[
								SAssignNew(Scroll, SScrollBox)
							]
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							SAssignNew(Input, SEditableTextBox)
							.Font(Line)
							.HintText(FText::FromString(TEXT("Say something... (Enter to send, Esc to walk away)")))
							.OnTextCommitted_Lambda([this](const FText& T, ETextCommit::Type How)
							{
								if (How == ETextCommit::OnEnter && Owner && !T.IsEmpty())
								{
									const FString Line = T.ToString();
									Input->SetText(FText::GetEmpty());
									Owner->SendPlayerLine(Line);
									RefocusInput();
								}
							})
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
						[
							SNew(STextBlock).Font(Small).ColorAndOpacity(Dim)
							.Text_Lambda([this]() -> FText
							{
								if (!Owner) { return FText::GetEmpty(); }
								if (Owner->IsCapturingVoice()) { return FText::FromString(TEXT("listening... (release Tab to send)")); }
								if (Owner->IsTranscribing()) { return FText::FromString(TEXT("transcribing...")); }
								if (Owner->IsWaitingForReply()) { return FText::FromString(TEXT("...")); }
								if (Owner->HasVoiceInput()) { return FText::FromString(TEXT("hold Tab to talk")); }
								return FText::GetEmpty();
							})
						]
					]
				]
			]
		];
	}

	/** Rebuild the transcript rows (called by the subsystem after every change). */
	void Refresh()
	{
		if (!Scroll.IsValid() || !Owner) { return; }
		const FSlateFontInfo Line = FCoreStyle::GetDefaultFontStyle("Regular", 15);
		const FLinearColor Text(0.95f, 0.93f, 0.88f, 1.f);
		const FLinearColor Jimmy(0.55f, 0.75f, 1.0f, 1.f);
		Scroll->ClearChildren();
		for (const FNPCChatLine& L : Owner->GetTranscript())
		{
			const FString Speaker = L.bPlayer ? TEXT("Jimmy") : Owner->GetNpcName();
			Scroll->AddSlot().Padding(0, 2)
			[
				SNew(STextBlock).Font(Line).AutoWrapText(true)
				.ColorAndOpacity(L.bPlayer ? Jimmy : Text)
				.Text(FText::FromString(FString::Printf(TEXT("%s: %s"), *Speaker, *L.Text)))
			];
		}
		Scroll->ScrollToEnd();
	}

	void RefocusInput()
	{
		if (Input.IsValid() && FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().SetKeyboardFocus(Input, EFocusCause::SetDirectly);
		}
	}

	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& Geo, const FKeyEvent& Event) override
	{
		if (Event.GetKey() == EKeys::Escape)
		{
			if (Owner) { Owner->CloseChat(); }
			return FReply::Handled();
		}
		return SCompoundWidget::OnKeyDown(Geo, Event);
	}
	// Push-to-talk on Tab via the PREVIEW pass: it fires before the focused text box, so Tab never reaches
	// Slate focus navigation or the input field.
	virtual FReply OnPreviewKeyDown(const FGeometry& Geo, const FKeyEvent& Event) override
	{
		if (Event.GetKey() == EKeys::Tab)
		{
			if (Owner && !Event.IsRepeat()) { Owner->StartVoice(); }
			return FReply::Handled();
		}
		return SCompoundWidget::OnPreviewKeyDown(Geo, Event);
	}
	// No preview pass exists for key-up; the release bubbles up from the (non-consuming) text box to here.
	virtual FReply OnKeyUp(const FGeometry& Geo, const FKeyEvent& Event) override
	{
		if (Event.GetKey() == EKeys::Tab)
		{
			if (Owner) { Owner->StopVoiceAndSend(); }
			return FReply::Handled();
		}
		return SCompoundWidget::OnKeyUp(Geo, Event);
	}

	UNPCChatSubsystem* Owner = nullptr;

private:
	TSharedPtr<SScrollBox> Scroll;
	TSharedPtr<SEditableTextBox> Input;
};

// ---------------------------------------------------------------------------------------------------------------
// lifecycle

bool UNPCChatSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* W = Cast<UWorld>(Outer);
	return W && (W->WorldType == EWorldType::Game || W->WorldType == EWorldType::PIE);
}

void UNPCChatSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	bConfigLoaded = LoadConfig();
	if (FParse::Param(FCommandLine::Get(), TEXT("BRNPCChatSmoke")))
	{
		InteractRadius = FMath::Max(InteractRadius, 20000.f); // smoke player spawns at the gates, walkers are in the courtyard
		UE_LOG(LogNPCChat, Display, TEXT("BR_NPCCHAT smoke armed"));
		SmokeTicker = FTSTicker::GetCoreTicker().AddTicker(TEXT("NPCChatSmoke"), 0.f, [this](float Dt)
		{
			RunSmoke(Dt);
			return SmokeStep < 100;
		});
	}
}

void UNPCChatSubsystem::Deinitialize()
{
	FTSTicker::GetCoreTicker().RemoveTicker(SmokeTicker);
	if (IsChatOpen())
	{
		CloseChat();
	}
	Super::Deinitialize();
}

void UNPCChatSubsystem::RunSmoke(float Dt)
{
	// Wall-clock, not game time: fixed-timestep nullrhi runs game seconds far faster than real ones, and the
	// LLM/memory HTTP round-trips happen in real time (first smoke: 90 "seconds" lapsed in 2.5 real ones).
	if (SmokeStart <= 0.0) { SmokeStart = FPlatformTime::Seconds(); }
	SmokeTime = static_cast<float>(FPlatformTime::Seconds() - SmokeStart);
	const int32 StepBefore = SmokeStep;
	if (SmokeStep == 0 && SmokeTime > 8.f)
	{
		// walkers need a few seconds to spawn; retry the toggle until someone is in range
		ABullyPlayerController* PC = Cast<ABullyPlayerController>(GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr);
		if (PC)
		{
			Toggle(PC);
			if (IsChatOpen())
			{
				SmokePed = NpcPedKey;
				UE_LOG(LogNPCChat, Display, TEXT("BR_NPCCHAT SMOKE open: ped=%s name=%s"), *NpcPedKey, *Persona.Name);
				SendPlayerLine(TEXT("Hey, I'm Jimmy, the new kid. What's your deal?"));
				SmokeStep = 1;
			}
		}
		if (SmokeStep == 0 && SmokeTime > 40.f)
		{
			UE_LOG(LogNPCChat, Error, TEXT("BR_NPCCHAT SMOKE FAIL: no NPC in range / no controller"));
			SmokeStep = 100;
		}
	}
	else if (SmokeStep == 1)
	{
		if (Transcript.Num() >= 2 && !Transcript.Last().bPlayer)
		{
			UE_LOG(LogNPCChat, Display, TEXT("BR_NPCCHAT SMOKE reply: %s"), *Transcript.Last().Text);
			CloseChat();
			SmokeStep = 2;
		}
		else if (SmokeTime > 90.f)
		{
			UE_LOG(LogNPCChat, Error, TEXT("BR_NPCCHAT SMOKE FAIL: no LLM reply in 90 s"));
			CloseChat();
			SmokeStep = 100;
		}
	}
	else if (SmokeStep == 2)
	{
		if (FPaths::FileExists(MemoryPath(SmokePed)))
		{
			UE_LOG(LogNPCChat, Display, TEXT("BR_NPCCHAT SMOKE OK: memory written to %s"), *MemoryPath(SmokePed));
			SmokeStep = 100;
		}
		else if (SmokeTime > 150.f)
		{
			UE_LOG(LogNPCChat, Error, TEXT("BR_NPCCHAT SMOKE FAIL: memory summary never landed"));
			SmokeStep = 100;
		}
	}
	// the smoke owns the run's length: the walk script just idles, so quit as soon as there is a verdict
	if (SmokeStep == 100 && StepBefore != 100 && GEngine)
	{
		UE_LOG(LogNPCChat, Display, TEXT("BR_NPCCHAT smoke done, quitting"));
		GEngine->Exec(GetWorld(), TEXT("QUIT"));
	}
}

// ---------------------------------------------------------------------------------------------------------------
// config

FString UNPCChatSubsystem::ReadKeyFile(const FString& InPath)
{
	if (InPath.IsEmpty()) { return FString(); }
	FString Path = InPath;
	if (Path.StartsWith(TEXT("~/")))
	{
		Path = FPaths::Combine(FPlatformProcess::UserHomeDir(), Path.Mid(2));
	}
	FString Key;
	if (!FFileHelper::LoadFileToString(Key, *Path))
	{
		UE_LOG(LogNPCChat, Warning, TEXT("BR_NPCCHAT key file not readable: %s"), *Path);
		return FString();
	}
	return Key.TrimStartAndEnd();
}

bool UNPCChatSubsystem::LoadConfig()
{
	const FString Path = FPaths::ProjectConfigDir() / TEXT("NPCChat.json");
	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *Path))
	{
		UE_LOG(LogNPCChat, Warning, TEXT("BR_NPCCHAT no %s: chat disabled"), *Path);
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), Root) || !Root.IsValid())
	{
		UE_LOG(LogNPCChat, Error, TEXT("BR_NPCCHAT %s is not valid JSON: chat disabled"), *Path);
		return false;
	}

	if (const TSharedPtr<FJsonObject>* Llm; Root->TryGetObjectField(TEXT("llm"), Llm))
	{
		(*Llm)->TryGetStringField(TEXT("base_url"), LlmBaseUrl);
		(*Llm)->TryGetStringField(TEXT("model"), LlmModel);
		FString KeyFile;
		(*Llm)->TryGetStringField(TEXT("api_key_file"), KeyFile);
		LlmKey = ReadKeyFile(KeyFile);
		(*Llm)->TryGetNumberField(TEXT("temperature"), LlmTemperature);
		(*Llm)->TryGetNumberField(TEXT("max_tokens"), LlmMaxTokens);
	}
	if (const TSharedPtr<FJsonObject>* Tts; Root->TryGetObjectField(TEXT("tts"), Tts))
	{
		(*Tts)->TryGetStringField(TEXT("base_url"), TtsBaseUrl);
		(*Tts)->TryGetStringField(TEXT("model"), TtsModel);
		FString KeyFile;
		(*Tts)->TryGetStringField(TEXT("api_key_file"), KeyFile);
		TtsKey = ReadKeyFile(KeyFile);
		(*Tts)->TryGetBoolField(TEXT("enabled"), bTtsEnabled);
	}
	if (const TSharedPtr<FJsonObject>* Stt; Root->TryGetObjectField(TEXT("stt"), Stt))
	{
		bool bSttEnabled = true;
		(*Stt)->TryGetBoolField(TEXT("enabled"), bSttEnabled);
		if (bSttEnabled)
		{
			(*Stt)->TryGetStringField(TEXT("binary"), SttBinary);
			(*Stt)->TryGetStringField(TEXT("model"), SttModelPath);
			(*Stt)->TryGetStringField(TEXT("language"), SttLanguage);
			if (SttModelPath.StartsWith(TEXT("~/")))
			{
				SttModelPath = FPaths::Combine(FPlatformProcess::UserHomeDir(), SttModelPath.Mid(2));
			}
			// voice input quietly turns off when the tools are not on this machine
			if (!FPaths::FileExists(SttBinary) || !FPaths::FileExists(SttModelPath))
			{
				UE_LOG(LogNPCChat, Warning, TEXT("BR_NPCCHAT stt disabled: %s / %s not found"), *SttBinary, *SttModelPath);
				SttBinary.Empty();
			}
		}
	}
	Root->TryGetNumberField(TEXT("interact_radius_cm"), InteractRadius);
	if (const TArray<TSharedPtr<FJsonValue>>* Arr; Root->TryGetArrayField(TEXT("world_facts"), Arr))
	{
		for (const TSharedPtr<FJsonValue>& V : *Arr) { WorldFacts.Add(V->AsString()); }
	}
	if (const TSharedPtr<FJsonObject>* Peds; Root->TryGetObjectField(TEXT("peds"), Peds))
	{
		for (const auto& Pair : (*Peds)->Values)
		{
			const TSharedPtr<FJsonObject> P = Pair.Value->AsObject();
			if (!P.IsValid()) { continue; }
			FNPCPersona Out;
			P->TryGetStringField(TEXT("name"), Out.Name);
			P->TryGetStringField(TEXT("voice"), Out.Voice);
			P->TryGetStringField(TEXT("persona"), Out.Persona);
			Personas.Add(FString(Pair.Key), Out); // UE 5.8: json keys are TSharedString, not FString
		}
	}
	const bool bLlmOk = !LlmBaseUrl.IsEmpty() && !LlmModel.IsEmpty();
	UE_LOG(LogNPCChat, Display, TEXT("BR_NPCCHAT config: llm=%s model=%s key=%s, tts=%s key=%s, personas=%d"),
		*LlmBaseUrl, *LlmModel, LlmKey.IsEmpty() ? TEXT("MISSING") : TEXT("ok"),
		bTtsEnabled ? *TtsBaseUrl : TEXT("off"), TtsKey.IsEmpty() ? TEXT("MISSING") : TEXT("ok"), Personas.Num());
	return bLlmOk;
}

FString UNPCChatSubsystem::PedKeyOf(const ABullyCharacter* C)
{
	if (const AAmbientWalker* W = Cast<AAmbientWalker>(C))
	{
		if (!W->StudentId.IsEmpty()) { return W->StudentId; }
	}
	// "/Game/Imported/Characters/GN_Bully01/SK_GN_Bully01.SK_GN_Bully01" -> "GN_Bully01"
	FString Tail;
	if (C->MeshPath.Split(TEXT("Characters/"), nullptr, &Tail))
	{
		FString Ped;
		if (Tail.Split(TEXT("/"), &Ped, nullptr)) { return Ped; }
	}
	return FString();
}

FNPCPersona UNPCChatSubsystem::ResolvePersona(const FString& PedKey) const
{
	if (const FNPCPersona* P = Personas.Find(PedKey)) { return *P; }
	if (const FNPCPersona* P = Personas.Find(TEXT("*")))
	{
		FNPCPersona Out = *P;
		if (Out.Name.IsEmpty()) { Out.Name = TEXT("Student"); }
		return Out;
	}
	FNPCPersona Out;
	Out.Name = TEXT("Student");
	Out.Persona = TEXT("A Bullworth Academy student.");
	return Out;
}

// ---------------------------------------------------------------------------------------------------------------
// memory (Saved/NPCChat/<ped>.json: {"facts": ["..."]})

FString UNPCChatSubsystem::MemoryPath(const FString& PedKey) const
{
	return FPaths::ProjectSavedDir() / TEXT("NPCChat") / (PedKey + TEXT(".json"));
}

TArray<FString> UNPCChatSubsystem::LoadFacts(const FString& PedKey) const
{
	TArray<FString> Out;
	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *MemoryPath(PedKey))) { return Out; }
	TSharedPtr<FJsonObject> Root;
	if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), Root) && Root.IsValid())
	{
		if (const TArray<TSharedPtr<FJsonValue>>* Arr; Root->TryGetArrayField(TEXT("facts"), Arr))
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr) { Out.Add(V->AsString()); }
		}
	}
	return Out;
}

void UNPCChatSubsystem::SaveFacts(const FString& PedKey, const TArray<FString>& InFacts) const
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FString& F : InFacts) { Arr.Add(MakeShared<FJsonValueString>(F)); }
	Root->SetArrayField(TEXT("facts"), Arr);
	Root->SetStringField(TEXT("updated"), FDateTime::UtcNow().ToIso8601());
	FString Out;
	const TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, W);
	FFileHelper::SaveStringToFile(Out, *MemoryPath(PedKey));
}

// ---------------------------------------------------------------------------------------------------------------
// open / close

ABullyCharacter* UNPCChatSubsystem::FindNearestNpc(ABullyPlayerController* PC, FString& OutPedKey) const
{
	const APawn* Player = PC ? PC->GetPawn() : nullptr;
	if (!Player) { return nullptr; }
	const FVector From = Player->GetActorLocation();
	ABullyCharacter* Best = nullptr;
	float BestDist = InteractRadius;
	for (TActorIterator<ABullyCharacter> It(GetWorld()); It; ++It)
	{
		ABullyCharacter* C = *It;
		if (C == Player) { continue; }
		const float D = FVector::Dist2D(From, C->GetActorLocation());
		if (D < BestDist)
		{
			BestDist = D;
			Best = C;
		}
	}
	OutPedKey = Best ? PedKeyOf(Best) : FString();
	return Best;
}

void UNPCChatSubsystem::Toggle(ABullyPlayerController* PC)
{
	if (IsChatOpen())
	{
		CloseChat();
		return;
	}
	if (!bConfigLoaded)
	{
		UE_LOG(LogNPCChat, Warning, TEXT("BR_NPCCHAT toggle ignored: config missing/invalid"));
		return;
	}
	FString PedKey;
	ABullyCharacter* Npc = FindNearestNpc(PC, PedKey);
	if (!Npc)
	{
		UE_LOG(LogNPCChat, Display, TEXT("BR_NPCCHAT nobody within %.0f cm"), InteractRadius);
		return;
	}
	OpenChat(PC, Npc, PedKey);
}

void UNPCChatSubsystem::OpenChat(ABullyPlayerController* PC, ABullyCharacter* Npc, const FString& PedKey)
{
	if (!GEngine || !GEngine->GameViewport || !FSlateApplication::IsInitialized())
	{
		UE_LOG(LogNPCChat, Warning, TEXT("BR_NPCCHAT open refused: no game viewport / Slate"));
		return;
	}
	Controller = PC;
	NpcRef = Npc;
	NpcPedKey = PedKey;
	Persona = ResolvePersona(PedKey);
	Facts = LoadFacts(PedKey);
	Transcript.Reset();
	bWaiting = false;

	TSharedRef<SNPCChatPanel> NewPanel = SNew(SNPCChatPanel).Owner(this);
	Panel = NewPanel;
	GEngine->GameViewport->AddViewportWidgetContent(NewPanel, 90);

	// The world keeps running (the point is talking to a living schoolyard); only this NPC holds still.
	PC->FlushPressedKeys();
	PC->SetIgnoreLookInput(true);
	PC->SetIgnoreMoveInput(true);
	FInputModeGameAndUI Mode;
	Mode.SetWidgetToFocus(NewPanel);
	Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	Mode.SetHideCursorDuringCapture(false);
	PC->SetInputMode(Mode);
	PC->SetShowMouseCursor(true);
	NewPanel->RefocusInput();

	HoldNpc(true);
	UE_LOG(LogNPCChat, Display, TEXT("BR_NPCCHAT open: ped=%s name=%s facts=%d voice=%s"),
		*PedKey, *Persona.Name, Facts.Num(), Persona.Voice.IsEmpty() ? TEXT("none") : *Persona.Voice);
}

void UNPCChatSubsystem::HoldNpc(bool bHold)
{
	ABullyCharacter* Npc = NpcRef.Get();
	if (!Npc) { return; }
	if (AAmbientWalker* W = Cast<AAmbientWalker>(Npc))
	{
		W->bChatHold = bHold;
	}
	if (ABullyScriptedNPC* S = Cast<ABullyScriptedNPC>(Npc))
	{
		// A mission-busy NPC keeps its script; only an idle one turns to Jimmy.
		if (bHold && !S->IsBusy())
		{
			S->FacePlayer(true);
		}
	}
}

void UNPCChatSubsystem::CloseChat()
{
	if (bCapturing)
	{
		bCapturing = false;
		VoiceCapture.StopStream();
		VoiceCapture.CloseStream();
	}
	if (Panel.IsValid() && GEngine && GEngine->GameViewport)
	{
		GEngine->GameViewport->RemoveViewportWidgetContent(Panel.ToSharedRef());
	}
	Panel.Reset();
	HoldNpc(false);

	if (ABullyPlayerController* PC = Controller.Get())
	{
		PC->SetIgnoreLookInput(false);
		PC->SetIgnoreMoveInput(false);
		PC->FlushPressedKeys();
		// Re-grab the mouse exactly as the Esc menu does on close (windowed -game on macOS needs the explicit lock).
		if (GEngine && GEngine->GameViewport)
		{
			GEngine->GameViewport->SetMouseCaptureMode(EMouseCaptureMode::CapturePermanently);
			GEngine->GameViewport->SetMouseLockMode(EMouseLockMode::LockAlways);
			GEngine->GameViewport->SetHideCursorDuringCapture(true);
		}
		FInputModeGameOnly Mode;
		Mode.SetConsumeCaptureMouseDown(true);
		PC->SetInputMode(Mode);
		PC->SetShowMouseCursor(false);
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().SetUserFocusToGameViewport(0);
		}
	}

	// Fold what was said into the NPC's long-term facts (fire-and-forget; survives world teardown).
	if (Transcript.Num() > 0)
	{
		RequestMemorySummary(NpcPedKey, Transcript, Facts);
	}
	UE_LOG(LogNPCChat, Display, TEXT("BR_NPCCHAT close: ped=%s lines=%d"), *NpcPedKey, Transcript.Num());
	Transcript.Reset();
	bWaiting = false;
}

// ---------------------------------------------------------------------------------------------------------------
// LLM

TSharedRef<IHttpRequest, ESPMode::ThreadSafe> UNPCChatSubsystem::MakeLlmRequest(
	const TArray<TSharedPtr<FJsonValue>>& Messages, float Temperature, int32 MaxTokens) const
{
	TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("model"), LlmModel);
	Body->SetArrayField(TEXT("messages"), Messages);
	Body->SetNumberField(TEXT("temperature"), Temperature);
	Body->SetNumberField(TEXT("max_tokens"), MaxTokens);
	FString Json;
	const TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(Body, W);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(LlmBaseUrl / TEXT("chat/completions"));
	Req->SetVerb(TEXT("POST"));
	Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	if (!LlmKey.IsEmpty())
	{
		Req->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + LlmKey);
	}
	Req->SetContentAsString(Json);
	Req->SetTimeout(30.f);
	return Req;
}

static TSharedPtr<FJsonValue> ChatMsg(const TCHAR* Role, const FString& Content)
{
	TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
	M->SetStringField(TEXT("role"), Role);
	M->SetStringField(TEXT("content"), Content);
	return MakeShared<FJsonValueObject>(M);
}

/** choices[0].message.content, or empty. */
static FString ParseChatReply(const FHttpResponsePtr& Resp)
{
	if (!Resp.IsValid()) { return FString(); }
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Resp->GetContentAsString()), Root) || !Root.IsValid())
	{
		return FString();
	}
	const TArray<TSharedPtr<FJsonValue>>* Choices;
	if (!Root->TryGetArrayField(TEXT("choices"), Choices) || Choices->Num() == 0) { return FString(); }
	const TSharedPtr<FJsonObject> Choice = (*Choices)[0]->AsObject();
	if (!Choice.IsValid()) { return FString(); }
	const TSharedPtr<FJsonObject>* Msg;
	if (!Choice->TryGetObjectField(TEXT("message"), Msg)) { return FString(); }
	FString Content;
	(*Msg)->TryGetStringField(TEXT("content"), Content);
	// Reasoning models (MiniMax M2.x, DeepSeek R1, Qwen) interleave <think>...</think> before the spoken line.
	int32 Open;
	while ((Open = Content.Find(TEXT("<think>"))) != INDEX_NONE)
	{
		const int32 Close = Content.Find(TEXT("</think>"), ESearchCase::IgnoreCase, ESearchDir::FromStart, Open);
		if (Close == INDEX_NONE)
		{
			Content.LeftInline(Open); // unterminated think block (ran out of tokens): drop it
			break;
		}
		Content.RemoveAt(Open, Close + 8 - Open);
	}
	return Content.TrimStartAndEnd();
}

FString UNPCChatSubsystem::BuildSystemPrompt() const
{
	FString P;
	P += FString::Printf(TEXT("You are %s, a character in the world of Bully: Scholarship Edition at Bullworth Academy. %s\n\n"),
		*Persona.Name, *Persona.Persona);
	if (WorldFacts.Num() > 0)
	{
		P += TEXT("World facts:\n");
		for (const FString& F : WorldFacts) { P += TEXT("- ") + F + TEXT("\n"); }
		P += TEXT("\n");
	}
	if (Facts.Num() > 0)
	{
		P += TEXT("Things you remember about Jimmy Hopkins from before:\n");
		for (const FString& F : Facts) { P += TEXT("- ") + F + TEXT("\n"); }
		P += TEXT("\n");
	}
	else
	{
		P += TEXT("You have never spoken with Jimmy Hopkins before.\n\n");
	}
	P += TEXT("You are talking face to face with Jimmy Hopkins, the new kid, in the school grounds. ")
		TEXT("Stay completely in character. Reply with ONLY your spoken words: no stage directions, no quotes, ")
		TEXT("no emoji, 1-3 short sentences. Keep it PG-13 schoolyard.");
	return P;
}

void UNPCChatSubsystem::SendPlayerLine(const FString& Line)
{
	if (bWaiting || Line.TrimStartAndEnd().IsEmpty()) { return; }
	Transcript.Add({ true, Line.TrimStartAndEnd() });
	if (Panel.IsValid()) { Panel->Refresh(); }
	RequestReply();
}

void UNPCChatSubsystem::RequestReply()
{
	TArray<TSharedPtr<FJsonValue>> Messages;
	Messages.Add(ChatMsg(TEXT("system"), BuildSystemPrompt()));
	const int32 First = FMath::Max(0, Transcript.Num() - 16); // keep the prompt small; memory carries the rest
	for (int32 i = First; i < Transcript.Num(); ++i)
	{
		Messages.Add(ChatMsg(Transcript[i].bPlayer ? TEXT("user") : TEXT("assistant"), Transcript[i].Text));
	}
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = MakeLlmRequest(Messages, LlmTemperature, LlmMaxTokens);
	Req->OnProcessRequestComplete().BindUObject(this, &UNPCChatSubsystem::OnReply);
	PendingReply = Req;
	bWaiting = true;
	Req->ProcessRequest();
}

void UNPCChatSubsystem::OnReply(FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bOk)
{
	bWaiting = false;
	if (!IsChatOpen()) { return; } // closed while in flight: the memory summary on close already captured the transcript
	FString Reply = bOk ? ParseChatReply(Resp) : FString();
	if (Reply.IsEmpty())
	{
		UE_LOG(LogNPCChat, Warning, TEXT("BR_NPCCHAT llm reply failed (http %d): %s"),
			Resp.IsValid() ? Resp->GetResponseCode() : -1,
			Resp.IsValid() ? *Resp->GetContentAsString().Left(300) : TEXT("no response"));
		Reply = TEXT("...");
	}
	Transcript.Add({ false, Reply });
	if (Panel.IsValid()) { Panel->Refresh(); }
	if (Reply != TEXT("..."))
	{
		Speak(Reply);
	}
}

void UNPCChatSubsystem::RequestMemorySummary(const FString& PedKey, TArray<FNPCChatLine> Lines, TArray<FString> OldFacts)
{
	if (PedKey.IsEmpty() || LlmBaseUrl.IsEmpty()) { return; }
	FString Convo;
	for (const FNPCChatLine& L : Lines)
	{
		Convo += (L.bPlayer ? TEXT("Jimmy: ") : TEXT("You: ")) + L.Text + TEXT("\n");
	}
	FString Prompt = TEXT("You play an NPC in a game. Merge your existing memories with this new conversation you just had ")
		TEXT("with Jimmy Hopkins into an updated memory list. Keep at most 12 short, concrete, first-person facts worth ")
		TEXT("remembering (what Jimmy said or did, promises, insults, how you feel about him). Drop trivia. ")
		TEXT("Answer with ONLY a JSON array of strings.\n\nExisting memories:\n");
	for (const FString& F : OldFacts) { Prompt += TEXT("- ") + F + TEXT("\n"); }
	Prompt += TEXT("\nNew conversation:\n") + Convo;

	TArray<TSharedPtr<FJsonValue>> Messages;
	Messages.Add(ChatMsg(TEXT("user"), Prompt));
	// reasoning models think before the JSON array; a tight budget truncates mid-<think> and loses everything
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = MakeLlmRequest(Messages, 0.2f, 3000);

	// Self-contained completion (no `this`): the summary must land even if the world/subsystem is gone by then.
	const FString SavePath = MemoryPath(PedKey);
	Req->OnProcessRequestComplete().BindLambda([SavePath, OldFacts](FHttpRequestPtr, FHttpResponsePtr R, bool bSumOk)
	{
		const FString Content = bSumOk ? ParseChatReply(R) : FString();
		TArray<FString> NewFacts;
		if (!Content.IsEmpty())
		{
			// tolerate ```json fences and prose around the array
			const int32 A = Content.Find(TEXT("["));
			const int32 B = Content.Find(TEXT("]"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
			if (A != INDEX_NONE && B != INDEX_NONE && B > A)
			{
				TArray<TSharedPtr<FJsonValue>> Arr;
				if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Content.Mid(A, B - A + 1)), Arr))
				{
					for (const TSharedPtr<FJsonValue>& V : Arr)
					{
						FString S = V->AsString().TrimStartAndEnd();
						if (!S.IsEmpty()) { NewFacts.Add(S); }
					}
				}
			}
		}
		if (NewFacts.Num() == 0)
		{
			UE_LOG(LogNPCChat, Warning, TEXT("BR_NPCCHAT memory summary failed (http %d, %d chars); keeping old facts (%s)"),
				R.IsValid() ? R->GetResponseCode() : -1, Content.Len(), *SavePath);
			NewFacts = OldFacts;
			if (NewFacts.Num() == 0) { return; }
		}
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const FString& F : NewFacts) { Arr.Add(MakeShared<FJsonValueString>(F)); }
		Root->SetArrayField(TEXT("facts"), Arr);
		Root->SetStringField(TEXT("updated"), FDateTime::UtcNow().ToIso8601());
		FString Out;
		const TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Root, W);
		FFileHelper::SaveStringToFile(Out, *SavePath);
		UE_LOG(LogNPCChat, Display, TEXT("BR_NPCCHAT memory saved: %d facts -> %s"), NewFacts.Num(), *SavePath);
	});
	Req->ProcessRequest();
}

// ---------------------------------------------------------------------------------------------------------------
// STT: push-to-talk. Mic samples are mixed to mono on the audio thread; on release they are resampled to 16 kHz,
// written as a wav into Saved/NPCChat/, and handed to a local whisper.cpp binary on a worker thread.

void UNPCChatSubsystem::StartVoice()
{
	if (bCapturing || bTranscribing || SttBinary.IsEmpty() || !IsChatOpen()) { return; }
	{
		FScopeLock Lock(&VoiceLock);
		VoiceBuffer.Reset();
		VoiceSampleRate = 0;
	}
	Audio::FAudioCaptureDeviceParams Params; // default device
	Audio::FOnAudioCaptureFunction OnCapture = [this](const void* InAudio, int32 NumFrames, int32 NumChannels, int32 SampleRate, double, bool)
	{
		const float* Samples = static_cast<const float*>(InAudio);
		FScopeLock Lock(&VoiceLock);
		VoiceSampleRate = SampleRate;
		if (VoiceBuffer.Num() > SampleRate * 30) { return; } // 30 s cap
		VoiceBuffer.Reserve(VoiceBuffer.Num() + NumFrames);
		for (int32 F = 0; F < NumFrames; ++F)
		{
			float Mono = 0.f;
			for (int32 C = 0; C < NumChannels; ++C) { Mono += Samples[F * NumChannels + C]; }
			VoiceBuffer.Add(Mono / FMath::Max(1, NumChannels));
		}
	};
	if (!VoiceCapture.OpenAudioCaptureStream(Params, MoveTemp(OnCapture), 1024) || !VoiceCapture.StartStream())
	{
		UE_LOG(LogNPCChat, Warning, TEXT("BR_NPCCHAT mic open failed (permission denied, or no input device)"));
		return;
	}
	bCapturing = true;
	UE_LOG(LogNPCChat, Display, TEXT("BR_NPCCHAT listening..."));
}

void UNPCChatSubsystem::StopVoiceAndSend()
{
	if (!bCapturing) { return; }
	bCapturing = false;
	VoiceCapture.StopStream();
	VoiceCapture.CloseStream();

	TArray<float> Samples;
	int32 Rate = 0;
	{
		FScopeLock Lock(&VoiceLock);
		Samples = MoveTemp(VoiceBuffer);
		Rate = VoiceSampleRate;
	}
	if (Rate <= 0 || Samples.Num() < Rate / 4) // under ~250 ms is a key tap, not speech
	{
		UE_LOG(LogNPCChat, Display, TEXT("BR_NPCCHAT voice: too short, ignored (%d samples @ %d)"), Samples.Num(), Rate);
		return;
	}

	// resample to 16 kHz mono 16-bit (what whisper.cpp wants) and write a wav
	const int32 OutRate = 16000;
	const int32 OutFrames = static_cast<int32>(static_cast<int64>(Samples.Num()) * OutRate / Rate);
	TArray<uint8> Wav;
	Wav.Reserve(44 + OutFrames * 2);
	auto Put32 = [&Wav](uint32 V) { Wav.Append(reinterpret_cast<uint8*>(&V), 4); };
	auto Put16 = [&Wav](uint16 V) { Wav.Append(reinterpret_cast<uint8*>(&V), 2); };
	Wav.Append(reinterpret_cast<const uint8*>("RIFF"), 4); Put32(36 + OutFrames * 2);
	Wav.Append(reinterpret_cast<const uint8*>("WAVE"), 4);
	Wav.Append(reinterpret_cast<const uint8*>("fmt "), 4); Put32(16);
	Put16(1); Put16(1); Put32(OutRate); Put32(OutRate * 2); Put16(2); Put16(16);
	Wav.Append(reinterpret_cast<const uint8*>("data"), 4); Put32(OutFrames * 2);
	for (int32 F = 0; F < OutFrames; ++F)
	{
		const double SrcPos = static_cast<double>(F) * Rate / OutRate;
		const int32 I0 = FMath::Min(static_cast<int32>(SrcPos), Samples.Num() - 1);
		const int32 I1 = FMath::Min(I0 + 1, Samples.Num() - 1);
		const float Frac = static_cast<float>(SrcPos - I0);
		const float S = FMath::Clamp(FMath::Lerp(Samples[I0], Samples[I1], Frac), -1.f, 1.f);
		Put16(static_cast<uint16>(static_cast<int16>(S * 32767.f)));
	}
	const FString WavPath = FPaths::ProjectSavedDir() / TEXT("NPCChat") / TEXT("voice_input.wav");
	if (!FFileHelper::SaveArrayToFile(Wav, *WavPath))
	{
		UE_LOG(LogNPCChat, Warning, TEXT("BR_NPCCHAT voice: could not write %s"), *WavPath);
		return;
	}

	bTranscribing = true;
	const FString Binary = SttBinary, Model = SttModelPath;
	const FString Lang = SttLanguage.IsEmpty() ? TEXT("en") : SttLanguage;
	TWeakObjectPtr<UNPCChatSubsystem> WeakThis(this);
	Async(EAsyncExecution::Thread, [WeakThis, Binary, Model, Lang, WavPath]()
	{
		const FString Args = FString::Printf(TEXT("-m \"%s\" -f \"%s\" --language %s -nt -np"), *Model, *WavPath, *Lang);
		int32 Code = -1;
		FString StdOut, StdErr;
		FPlatformProcess::ExecProcess(*Binary, *Args, &Code, &StdOut, &StdErr);
		FString Text = StdOut.TrimStartAndEnd();
		if (Code != 0)
		{
			UE_LOG(LogNPCChat, Warning, TEXT("BR_NPCCHAT whisper exit %d: %s"), Code, *StdErr.Left(200));
			Text.Empty();
		}
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Text]()
		{
			if (UNPCChatSubsystem* Self = WeakThis.Get()) { Self->OnTranscribed(Text); }
		});
	});
}

void UNPCChatSubsystem::OnTranscribed(const FString& InText)
{
	bTranscribing = false;
	FString Text = InText;
	// whisper emits bracketed non-speech markers ("[BLANK_AUDIO]", "(wind blowing)") when there are no words
	Text.ReplaceInline(TEXT("\n"), TEXT(" "));
	Text.TrimStartAndEndInline();
	if (Text.IsEmpty() || (Text.StartsWith(TEXT("[")) && Text.EndsWith(TEXT("]"))) || (Text.StartsWith(TEXT("(")) && Text.EndsWith(TEXT(")"))))
	{
		UE_LOG(LogNPCChat, Display, TEXT("BR_NPCCHAT voice: no speech detected"));
		return;
	}
	UE_LOG(LogNPCChat, Display, TEXT("BR_NPCCHAT voice -> \"%s\""), *Text);
	if (IsChatOpen())
	{
		SendPlayerLine(Text);
		if (Panel.IsValid()) { Panel->Refresh(); }
	}
}

// ---------------------------------------------------------------------------------------------------------------
// TTS (Fish Audio /v1/tts, wav -> USoundWaveProcedural at the NPC)

void UNPCChatSubsystem::Speak(const FString& Text)
{
	if (!bTtsEnabled || TtsKey.IsEmpty() || Persona.Voice.IsEmpty() || TtsBaseUrl.IsEmpty()) { return; }
	TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("text"), Text);
	Body->SetStringField(TEXT("reference_id"), Persona.Voice);
	Body->SetStringField(TEXT("format"), TEXT("wav"));
	FString Json;
	const TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(Body, W);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(TtsBaseUrl / TEXT("v1/tts"));
	Req->SetVerb(TEXT("POST"));
	Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Req->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + TtsKey);
	if (!TtsModel.IsEmpty())
	{
		Req->SetHeader(TEXT("model"), TtsModel);
	}
	Req->SetContentAsString(Json);
	Req->SetTimeout(30.f);
	Req->OnProcessRequestComplete().BindUObject(this, &UNPCChatSubsystem::OnSpeech);
	Req->ProcessRequest();
}

bool UNPCChatSubsystem::ParseWav(const TArray<uint8>& Bytes, int32& OutRate, int32& OutChannels, TArray<uint8>& OutPcm)
{
	if (Bytes.Num() < 44 || FMemory::Memcmp(Bytes.GetData(), "RIFF", 4) != 0 || FMemory::Memcmp(Bytes.GetData() + 8, "WAVE", 4) != 0)
	{
		return false;
	}
	// Chunk sizes come off the network: walk with 64-bit offsets and never trust Size (a hostile/truncated
	// wav must not overflow Pos or read past the buffer).
	int64 Pos = 12;
	const int64 Total = Bytes.Num();
	OutRate = 0;
	OutChannels = 0;
	int16 Bits = 0;
	while (Pos + 8 <= Total)
	{
		const uint8* P = Bytes.GetData() + Pos;
		const int64 Size = static_cast<int64>(P[4]) | (static_cast<int64>(P[5]) << 8) | (static_cast<int64>(P[6]) << 16) | (static_cast<int64>(P[7]) << 24);
		if (FMemory::Memcmp(P, "fmt ", 4) == 0 && Size >= 16 && Pos + 8 + 16 <= Total)
		{
			OutChannels = P[8 + 2] | (P[8 + 3] << 8);
			OutRate = P[8 + 4] | (P[8 + 5] << 8) | (P[8 + 6] << 16) | (P[8 + 7] << 24);
			Bits = P[8 + 14] | (P[8 + 15] << 8);
		}
		else if (FMemory::Memcmp(P, "data", 4) == 0)
		{
			const int64 Have = FMath::Min<int64>(Size, Total - Pos - 8);
			if (Have > 0) { OutPcm.Append(P + 8, Have); }
			break;
		}
		Pos += 8 + Size + (Size & 1); // always advances (Size >= 0)
	}
	return OutRate > 0 && OutChannels > 0 && Bits == 16 && OutPcm.Num() > 0;
}

void UNPCChatSubsystem::OnSpeech(FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bOk)
{
	ABullyCharacter* Npc = NpcRef.Get();
	if (!bOk || !Resp.IsValid() || Resp->GetResponseCode() != 200 || !Npc)
	{
		UE_LOG(LogNPCChat, Warning, TEXT("BR_NPCCHAT tts failed (http %d)"), Resp.IsValid() ? Resp->GetResponseCode() : -1);
		return;
	}
	int32 Rate = 0, Channels = 0;
	TArray<uint8> Pcm;
	if (!ParseWav(Resp->GetContent(), Rate, Channels, Pcm))
	{
		UE_LOG(LogNPCChat, Warning, TEXT("BR_NPCCHAT tts returned an unparseable wav (%d bytes)"), Resp->GetContent().Num());
		return;
	}
	if (SpeechAudio) { SpeechAudio->Stop(); } // a new line cuts off the previous one

	USoundWaveProcedural* Wave = NewObject<USoundWaveProcedural>(this);
	Wave->SetSampleRate(Rate);
	Wave->NumChannels = Channels;
	Wave->Duration = static_cast<float>(Pcm.Num()) / (Rate * Channels * 2);
	Wave->SoundGroup = SOUNDGROUP_Voice;
	Wave->bLooping = false;
	Wave->QueueAudio(Pcm.GetData(), Pcm.Num());
	SpeechWave = Wave;

	SpeechAudio = UGameplayStatics::SpawnSoundAttached(Wave, Npc->GetMesh(), NAME_None, FVector(0, 0, 60.f),
		EAttachLocation::KeepRelativeOffset, true);
	UE_LOG(LogNPCChat, Display, TEXT("BR_NPCCHAT speaking %.1f s @ %d Hz x%d"), Wave->Duration, Rate, Channels);
}
