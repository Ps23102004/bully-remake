// OWNER: builder-B2 (doors)
#include "BullyWalkRunner.h"

#include "BullyPawn.h"
#include "Components/CapsuleComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HighResScreenshot.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UnrealClient.h"
#if WITH_EDITOR
#include "AssetCompilingManager.h"
#include "ShaderCompiler.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogBullyWalk, Log, All);

bool UBullyWalkRunner::ShouldCreateSubsystem(UObject* Outer) const
{
	FString Unused;
	return FParse::Value(FCommandLine::Get(), TEXT("BRWalkFile="), Unused);
}

void UBullyWalkRunner::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	FString File;
	FParse::Value(FCommandLine::Get(), TEXT("BRWalkFile="), File);
	if (!FParse::Value(FCommandLine::Get(), TEXT("BRWalkDir="), OutDir))
	{
		OutDir = FPaths::ProjectSavedDir() / TEXT("BullyWalk");
	}
	IFileManager::Get().MakeDirectory(*OutDir, true);
	bActive = LoadScript(File);
	UE_LOG(LogBullyWalk, Display, TEXT("BR_WALK runner %s: %d steps from %s -> %s"),
		bActive ? TEXT("active") : TEXT("INACTIVE (bad script)"), Steps.Num(), *File, *OutDir);
}

void UBullyWalkRunner::Deinitialize()
{
	bActive = false;
	Super::Deinitialize();
}

TStatId UBullyWalkRunner::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UBullyWalkRunner, STATGROUP_Tickables);
}

bool UBullyWalkRunner::LoadScript(const FString& Path)
{
	FString Text;
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(Text, *Path) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		UE_LOG(LogBullyWalk, Error, TEXT("BR_WALK cannot read/parse %s"), *Path);
		return false;
	}
	Root->TryGetNumberField(TEXT("warmup_seconds"), WarmupSeconds);
	Root->TryGetNumberField(TEXT("settle_seconds"), SettleSeconds);
	Root->TryGetNumberField(TEXT("max_compile_wait_seconds"), MaxCompileWaitSeconds);
	Root->TryGetStringArrayField(TEXT("exec"), ExecCommands);
	const TArray<TSharedPtr<FJsonValue>>* HR = nullptr;
	if (Root->TryGetArrayField(TEXT("highres_size"), HR) && HR->Num() == 2)
	{
		HighResX = int32((*HR)[0]->AsNumber());
		HighResY = int32((*HR)[1]->AsNumber());
	}
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (!Root->TryGetArrayField(TEXT("steps"), Arr))
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& V : *Arr)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		if (!O.IsValid()) { continue; }
		FBullyWalkStep S;
		O->TryGetStringField(TEXT("do"), S.Do);
		O->TryGetStringField(TEXT("name"), S.Name);
		double D = 0;
		if (O->TryGetNumberField(TEXT("x"), D)) { S.Pos.X = D; }
		if (O->TryGetNumberField(TEXT("y"), D)) { S.Pos.Y = D; }
		if (O->TryGetNumberField(TEXT("z"), D)) { S.Pos.Z = D; S.bHasZ = true; }
		if (O->TryGetNumberField(TEXT("yaw"), D)) { S.Yaw = float(D); }
		if (O->TryGetNumberField(TEXT("pitch"), D)) { S.Pitch = float(D); }
		if (O->TryGetNumberField(TEXT("seconds"), D)) { S.Seconds = float(D); }
		if (O->TryGetNumberField(TEXT("timeout"), D)) { S.Seconds = float(D); }
		if (O->TryGetNumberField(TEXT("radius"), D)) { S.Radius = float(D); }
		O->TryGetBoolField(TEXT("run"), S.bRun);
		O->TryGetBoolField(TEXT("stop_on_door"), S.bStopOnDoor);
		Steps.Add(S);
	}
	return Steps.Num() > 0;
}

void UBullyWalkRunner::Note(const FString& Line)
{
	Log.Add(FString::Printf(TEXT("[t=%.1f] %s"), Elapsed, *Line));
	UE_LOG(LogBullyWalk, Display, TEXT("BR_WALK %s"), *Line);
}

void UBullyWalkRunner::BeginStep(UWorld* World, ABullyPawn* Pawn, FBullyWalkStep& S)
{
	StepStart = Elapsed;
	LastProgressAt = Elapsed;
	BestDist = 1e9f;
	DoorCountAtStart = Pawn->DoorUseCount;
	APlayerController* PC = Cast<APlayerController>(Pawn->GetController());
	if (S.Do == TEXT("face") && PC)
	{
		PC->SetControlRotation(FRotator(S.Pitch, S.Yaw, 0.f));
		Pawn->SetActorRotation(FRotator(0.f, S.Yaw, 0.f));
	}
	else if (S.Do == TEXT("teleport"))
	{
		FVector L = S.Pos;
		if (!S.bHasZ) { L.Z = Pawn->GetActorLocation().Z; }
		Pawn->TeleportTo(L, FRotator(0.f, S.Yaw, 0.f), false, true);
		Pawn->GetCharacterMovement()->StopMovementImmediately();
		if (PC) { PC->SetControlRotation(FRotator(S.Pitch, S.Yaw, 0.f)); }
		Note(FString::Printf(TEXT("teleport '%s' -> %s"), *S.Name, *Pawn->GetActorLocation().ToString()));
	}
	else if (S.Do == TEXT("trace_ground"))
	{
		FHitResult Hit;
		FCollisionQueryParams P(SCENE_QUERY_STAT(BullyWalkTrace), true, Pawn);
		const bool bHit = World->LineTraceSingleByChannel(Hit, FVector(S.Pos.X, S.Pos.Y, S.bHasZ ? S.Pos.Z : 6000.f), FVector(S.Pos.X, S.Pos.Y, -3000.f), ECC_Visibility, P);
		Note(FString::Printf(TEXT("trace_ground (%.0f,%.0f) -> %s z=%.1f actor=%s"), S.Pos.X, S.Pos.Y, bHit ? TEXT("HIT") : TEXT("MISS"), bHit ? Hit.ImpactPoint.Z : 0.f, bHit ? *GetNameSafe(Hit.GetActor()) : TEXT("-")));
	}
	else if (S.Do == TEXT("jump"))
	{
		Pawn->Jump();
	}
	else if (S.Do == TEXT("log"))
	{
		Note(FString::Printf(TEXT("log '%s' pawn %s doors=%d"), *S.Name, *Pawn->GetActorLocation().ToString(), Pawn->DoorUseCount));
	}
	else if (S.Do == TEXT("walk_to"))
	{
		Pawn->SetRunning(S.bRun);
		Note(FString::Printf(TEXT("walk_to '%s' from %s to (%.0f,%.0f) %s timeout %.0fs"), *S.Name, *Pawn->GetActorLocation().ToString(), S.Pos.X, S.Pos.Y, S.bRun ? TEXT("run") : TEXT("walk"), S.Seconds));
	}
	else if (S.Do == TEXT("shot"))
	{
		ShotRequestedAt = -1.0;
		if (PC && S.Yaw != 0.f)
		{
			PC->SetControlRotation(FRotator(S.Pitch, S.Yaw, 0.f));
		}
	}
}

// returns true when the step is finished
bool UBullyWalkRunner::TickStep(UWorld* World, ABullyPawn* Pawn, FBullyWalkStep& S, float Dt)
{
	const double T = Elapsed - StepStart;
	APlayerController* PC = Cast<APlayerController>(Pawn->GetController());
	if (S.Do == TEXT("wait"))
	{
		return T >= S.Seconds;
	}
	if (S.Do == TEXT("jump"))
	{
		return T >= 1.0;
	}
	if (S.Do == TEXT("wait_door"))
	{
		if (Pawn->DoorUseCount > DoorCountAtStart && !Pawn->bDoorBusy)
		{
			Note(FString::Printf(TEXT("wait_door OK: now at %s (doors used %d)"), *Pawn->GetActorLocation().ToString(), Pawn->DoorUseCount));
			return true;
		}
		if (T >= S.Seconds)
		{
			++Failures;
			Note(TEXT("wait_door TIMEOUT: no door transition happened"));
			return true;
		}
		return false;
	}
	if (S.Do == TEXT("walk_to"))
	{
		if (Pawn->bDoorBusy)
		{
			return false;   // mid-transition; resumed or ended below
		}
		const FVector Loc = Pawn->GetActorLocation();
		FVector To = S.Pos - Loc; To.Z = 0;
		const float Dist = To.Size();
		if (Pawn->DoorUseCount > DoorCountAtStart && S.bStopOnDoor)
		{
			Note(FString::Printf(TEXT("walk_to '%s' ended by a door transition at %s"), *S.Name, *Loc.ToString()));
			return true;
		}
		if (Dist <= S.Radius)
		{
			Pawn->SetRunning(false);
			Note(FString::Printf(TEXT("walk_to '%s' ARRIVED at %s after %.1fs"), *S.Name, *Loc.ToString(), T));
			return true;
		}
		if (Dist < BestDist - 20.f) { BestDist = Dist; LastProgressAt = Elapsed; }
		if (Elapsed - LastProgressAt > 6.0 || T >= S.Seconds)
		{
			++Failures;
			Note(FString::Printf(TEXT("walk_to '%s' STUCK/TIMEOUT at %s, %.0f cm short of target (best %.0f) after %.1fs"), *S.Name, *Loc.ToString(), Dist, BestDist, T));
			return true;
		}
		const FVector Dir = To.GetSafeNormal();
		if (PC)
		{
			PC->SetControlRotation(FRotator(-12.f, Dir.Rotation().Yaw, 0.f));
		}
		Pawn->AddMovementInput(Dir, 1.f);
		return false;
	}
	if (S.Do == TEXT("shot"))
	{
		const bool bWarm = ReadyAt >= 0.0 && Elapsed >= ReadyAt + WarmupSeconds;
		if (ShotRequestedAt < 0.0 && bWarm && T >= SettleSeconds)
		{
			const FString File = OutDir / FString::Printf(TEXT("%02d_%s.png"), ShotIndex++, *S.Name);
			if (HighResX > 0 && HighResY > 0)
			{
				FHighResScreenshotConfig& HR = GetHighResScreenshotConfig();
				HR.SetResolution(uint32(HighResX), uint32(HighResY), 1.0f);
				HR.SetFilename(File);
				HR.bMaskEnabled = false;
				HR.bDumpBufferVisualizationTargets = false;
				if (!(GEngine->GameViewport && GEngine->GameViewport->Viewport && GEngine->GameViewport->Viewport->TakeHighResScreenShot()))
				{
					Note(TEXT("HighResShot refused for ") + File);
					++Failures;
				}
			}
			else
			{
				FScreenshotRequest::RequestScreenshot(File, false, false);
			}
			ShotRequestedAt = Elapsed;
			Note(FString::Printf(TEXT("shot '%s' -> %s; pawn %s yaw %.0f; doors used %d"), *S.Name, *File, *Pawn->GetActorLocation().ToString(),
				PC ? PC->GetControlRotation().Yaw : 0.f, Pawn->DoorUseCount));
		}
		return ShotRequestedAt >= 0.0 && Elapsed >= ShotRequestedAt + 2.5;
	}
	return true;   // face / teleport / trace_ground / log: instantaneous
}

void UBullyWalkRunner::Tick(float DeltaTime)
{
	const UGameInstance* GI = GetGameInstance();
	UWorld* World = GI ? GI->GetWorld() : nullptr;
	if (!World || !World->IsGameWorld())
	{
		return;
	}
	Elapsed += DeltaTime;
	if (!bExecDone)
	{
		bExecDone = true;
		for (const FString& Cmd : ExecCommands)
		{
			GEngine->Exec(World, *Cmd);
		}
	}
	if (ReadyAt < 0.0)
	{
		int32 Assets = 0, ShaderJobs = 0;
#if WITH_EDITOR
		Assets = FAssetCompilingManager::Get().GetNumRemainingAssets();
		ShaderJobs = GShaderCompilingManager ? GShaderCompilingManager->GetNumRemainingJobs() : 0;
#endif
		IdleSince = (Assets == 0 && ShaderJobs == 0) ? (IdleSince < 0.0 ? Elapsed : IdleSince) : -1.0;
		if (Elapsed - LastCompileLog >= 10.0)
		{
			LastCompileLog = Elapsed;
			UE_LOG(LogBullyWalk, Display, TEXT("BR_WALK waiting for compilation: %d assets, %d shader jobs (t=%.0f)"), Assets, ShaderJobs, Elapsed);
		}
		if ((IdleSince >= 0.0 && Elapsed - IdleSince >= 3.0) || Elapsed >= MaxCompileWaitSeconds)
		{
			ReadyAt = Elapsed;
			Note(FString::Printf(TEXT("compilation idle; warm-up %.0f s"), WarmupSeconds));
		}
	}
	// Steps start only after the warm-up so the first walk happens on a settled scene (and physics is live).
	if (ReadyAt < 0.0 || Elapsed < ReadyAt + WarmupSeconds || bQuit)
	{
		return;
	}
	APlayerController* PC = UGameplayStatics::GetPlayerController(World, 0);
	ABullyPawn* Pawn = PC ? Cast<ABullyPawn>(PC->GetPawn()) : nullptr;
	if (!Pawn)
	{
		if (Elapsed > ReadyAt + WarmupSeconds + 30.0)
		{
			Note(TEXT("no ABullyPawn possessed after 30 s; aborting"));
			++Failures;
			bQuit = true;
			WriteResult();
			GEngine->Exec(World, TEXT("quit"));
		}
		return;
	}
	if (Current >= Steps.Num())
	{
		bQuit = true;
		Note(FString::Printf(TEXT("script complete, %d failures"), Failures));
		WriteResult();
		GEngine->Exec(World, TEXT("quit"));
		return;
	}
	FBullyWalkStep& S = Steps[Current];
	if (!bStepBegun)
	{
		bStepBegun = true;
		BeginStep(World, Pawn, S);
	}
	if (TickStep(World, Pawn, S, DeltaTime))
	{
		++Current;
		bStepBegun = false;
	}
}

void UBullyWalkRunner::WriteResult() const
{
	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	W->WriteObjectStart();
	W->WriteValue(TEXT("format"), TEXT("bully-remake.walk-result/1"));
	W->WriteValue(TEXT("failures"), Failures);
	W->WriteArrayStart(TEXT("log"));
	for (const FString& L : Log) { W->WriteValue(L); }
	W->WriteArrayEnd();
	W->WriteObjectEnd();
	W->Close();
	FFileHelper::SaveStringToFile(Out, *(OutDir / TEXT("walk_result.json")));
}
