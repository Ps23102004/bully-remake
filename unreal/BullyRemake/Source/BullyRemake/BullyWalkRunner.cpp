// OWNER: builder-B2 (doors); M3 steps builder-B3 (jimmy)
#include "BullyWalkRunner.h"

#include "BullyAnimInstance.h"
#include "BullyCharacter.h"
#include "Misc/App.h"
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
	if (!bActive)
	{
		// an evidence run with no script must not idle forever as a headless game
		UE_LOG(LogBullyWalk, Error, TEXT("BR_WALK no usable script; requesting exit"));
		FPlatformMisc::RequestExit(false);
	}
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
		O->TryGetStringField(TEXT("gait"), S.Gait);
		O->TryGetStringField(TEXT("cam"), S.Cam);
		O->TryGetBoolField(TEXT("jump"), S.bJump);
		O->TryGetStringField(TEXT("trigger"), S.Trigger);
		if (O->TryGetNumberField(TEXT("jump_at"), D)) { S.JumpAt = float(D); }
		if (O->TryGetNumberField(TEXT("trigger_time"), D)) { S.TriggerTime = float(D); }
		if (O->TryGetNumberField(TEXT("after"), D)) { S.After = float(D); }
		if (O->TryGetNumberField(TEXT("fps"), D)) { S.Fps = float(D); }
		S.bHasPos = O->HasField(TEXT("x")) && O->HasField(TEXT("y"));
		Steps.Add(S);
	}
	return Steps.Num() > 0;
}

void UBullyWalkRunner::Note(const FString& Line)
{
	Log.Add(FString::Printf(TEXT("[t=%.1f] %s"), Elapsed, *Line));
	UE_LOG(LogBullyWalk, Display, TEXT("BR_WALK %s"), *Line);
}

namespace
{
	EBullyGait ParseGait(const FString& G, bool bRun)
	{
		if (G == TEXT("walk")) { return EBullyGait::Walk; }
		if (G == TEXT("sprint")) { return EBullyGait::Sprint; }
		if (G == TEXT("run")) { return EBullyGait::Run; }
		return bRun ? EBullyGait::Run : EBullyGait::Walk;
	}
}

FString UBullyWalkRunner::PawnState(ABullyCharacter* Pawn) const
{
	const UBullyAnimInstance* AI = Cast<UBullyAnimInstance>(Pawn->GetMesh()->GetAnimInstance());
	return FString::Printf(TEXT("speed %.0f vz %.0f rise %.0f gait %d anim %s phase %.2f layers [%s] jumps %d lands %d"),
		Pawn->GetHorizontalSpeed(), Pawn->GetVelocity().Z, Pawn->GetActorLocation().Z - Pawn->LastJumpZ, int32(Pawn->GetGait()),
		AI ? AI->GetStateName() : TEXT("none"), AI ? AI->GetPhase() : 0.f, AI ? *AI->DescribeLayers() : TEXT(""), Pawn->JumpCount, Pawn->LandCount);
}

void UBullyWalkRunner::RequestShot(ABullyCharacter* Pawn, const FString& Name, bool bHighRes)
{
	const FString File = OutDir / FString::Printf(TEXT("%02d_%s.png"), ShotIndex++, *Name);
	if (bHighRes && HighResX > 0 && HighResY > 0)
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
	const APlayerController* PC = Cast<APlayerController>(Pawn->GetController());
	Note(FString::Printf(TEXT("shot '%s' -> %s; pawn %s yaw %.0f cam yaw %.0f pitch %.0f; %s; doors used %d"), *Name, *File, *Pawn->GetActorLocation().ToString(),
		Pawn->GetActorRotation().Yaw, PC ? PC->GetControlRotation().Yaw : 0.f, PC ? FRotator::NormalizeAxis(PC->GetControlRotation().Pitch) : 0.f, *PawnState(Pawn), Pawn->DoorUseCount));
}

void UBullyWalkRunner::BeginStep(UWorld* World, ABullyCharacter* Pawn, FBullyWalkStep& S)
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
		Pawn->SetScriptedGait(ParseGait(S.Gait, S.bRun));
		Note(FString::Printf(TEXT("walk_to '%s' from %s to (%.0f,%.0f) gait %d cam %s timeout %.0fs"), *S.Name, *Pawn->GetActorLocation().ToString(), S.Pos.X, S.Pos.Y, int32(Pawn->GetGait()), *S.Cam, S.Seconds));
	}
	else if (S.Do == TEXT("action_shot"))
	{
		ActionPhase = 0;
		bJumpIssued = false;
		JumpCountAtStart = Pawn->JumpCount;
		if (S.Gait != TEXT("none")) { Pawn->SetScriptedGait(ParseGait(S.Gait, S.bRun)); }
		Note(FString::Printf(TEXT("action_shot '%s' from %s gait %d jump %d trigger %s"), *S.Name, *Pawn->GetActorLocation().ToString(), int32(Pawn->GetGait()), S.bJump ? 1 : 0, *S.Trigger));
	}
	else if (S.Do == TEXT("record_start"))
	{
		RecordDir = OutDir / TEXT("frames");
		IFileManager::Get().MakeDirectory(*RecordDir, true);
		FApp::SetUseFixedTimeStep(true);
		FApp::SetFixedDeltaTime(1.0 / FMath::Max(1.f, S.Fps));
		bRecording = true;
		Note(FString::Printf(TEXT("record_start %.0f fps (fixed time step) -> %s"), S.Fps, *RecordDir));
	}
	else if (S.Do == TEXT("record_stop"))
	{
		bRecording = false;
		FApp::SetUseFixedTimeStep(false);
		Note(FString::Printf(TEXT("record_stop after %d frames"), RecordFrame));
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
bool UBullyWalkRunner::TickStep(UWorld* World, ABullyCharacter* Pawn, FBullyWalkStep& S, float Dt)
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
		if (Pawn->DoorUseCount > DoorsAcknowledged && !Pawn->bDoorBusy)
		{
			DoorsAcknowledged = Pawn->DoorUseCount;
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
		if (PC && S.Cam != TEXT("follow"))
		{
			PC->SetControlRotation(FRotator(-12.f, Dir.Rotation().Yaw, 0.f));
		}
		Pawn->AddMovementInput(Dir, 1.f);
		return false;
	}
	if (S.Do == TEXT("action_shot"))
	{
		if (ActionPhase == 0 || ActionPhase == 3)
		{
			// keep moving (toward the target, else along the step yaw)
			FVector Dir = S.bHasPos ? (S.Pos - Pawn->GetActorLocation()) : FRotator(0.f, S.Yaw, 0.f).Vector();
			Dir.Z = 0.f;
			if (S.Gait != TEXT("none") && Dir.SizeSquared() > 40.f * 40.f)
			{
				Pawn->AddMovementInput(Dir.GetSafeNormal(), 1.f);
			}
			// camera: follow = the character's own follow cam; lock = behind the travel direction; side = 90 deg off it
			if (PC && S.Cam != TEXT("follow") && Dir.SizeSquared() > 0.01f)
			{
				const float Yaw = Dir.Rotation().Yaw + (S.Cam == TEXT("side") ? 90.f : S.Cam == TEXT("front") ? 180.f : 0.f);
				PC->SetControlRotation(FRotator(S.Pitch, Yaw, 0.f));
			}
		}
		if (ActionPhase == 0)
		{
			if (S.bJump && !bJumpIssued && T >= S.JumpAt)
			{
				bJumpIssued = true;
				Pawn->Jump();
				ActionMark = Elapsed;
			}
			const bool bJumped = Pawn->JumpCount > JumpCountAtStart;
			bool bFire = false;
			if (S.Trigger == TEXT("apex")) { bFire = bJumped && Pawn->GetVelocity().Z <= 0.f; }
			else if (S.Trigger == TEXT("air")) { bFire = bJumped && Elapsed - ActionMark >= S.TriggerTime; }
			else if (S.Trigger == TEXT("land") || S.Trigger == TEXT("none_land")) { bFire = bJumped && !Pawn->GetCharacterMovement()->IsFalling() && Elapsed - ActionMark > 0.15 && Elapsed - ActionMark >= S.TriggerTime; }
			else { bFire = T >= S.TriggerTime; }
			if (bFire && (S.Trigger == TEXT("none") || S.Trigger == TEXT("none_land")))
			{
				// no shot: just a scripted move (+ jump) for recordings
				Note(FString::Printf(TEXT("action '%s' done at t=%.2f: %s; %s"), *S.Name, T, *Pawn->GetActorLocation().ToString(), *PawnState(Pawn)));
				ActionPhase = 3;
				ActionMark = Elapsed;
				return false;
			}
			if (bFire)
			{
				UGameplayStatics::SetGamePaused(World, true);
				Note(FString::Printf(TEXT("action_shot '%s' trigger %s at t=%.2f: paused at %s; %s"), *S.Name, *S.Trigger, T, *Pawn->GetActorLocation().ToString(), *PawnState(Pawn)));
				ActionPhase = 1;
				ActionMark = Elapsed;
			}
			else if (T > 30.0)
			{
				++Failures;
				Note(FString::Printf(TEXT("action_shot '%s' TIMEOUT (trigger %s never fired)"), *S.Name, *S.Trigger));
				return true;
			}
			return false;
		}
		if (ActionPhase == 1)
		{
			if (Elapsed - ActionMark >= SettleSeconds)
			{
				RequestShot(Pawn, S.Name, true);
				ActionPhase = 2;
				ActionMark = Elapsed;
			}
			return false;
		}
		if (ActionPhase == 2)
		{
			if (Elapsed - ActionMark >= 2.5)
			{
				UGameplayStatics::SetGamePaused(World, false);
				ActionPhase = 3;
				ActionMark = Elapsed;
			}
			return false;
		}
		return Elapsed - ActionMark >= S.After;
	}
	if (S.Do == TEXT("shot"))
	{
		const bool bWarm = ReadyAt >= 0.0 && Elapsed >= ReadyAt + WarmupSeconds;
		if (ShotRequestedAt < 0.0 && bWarm && T >= SettleSeconds)
		{
			RequestShot(Pawn, S.Name, true);
			ShotRequestedAt = Elapsed;
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
	ABullyCharacter* Pawn = PC ? Cast<ABullyCharacter>(PC->GetPawn()) : nullptr;
	if (!Pawn)
	{
		if (Elapsed > ReadyAt + WarmupSeconds + 30.0)
		{
			Note(TEXT("no ABullyCharacter possessed after 30 s; aborting"));
			++Failures;
			bQuit = true;
			WriteResult();
			GEngine->Exec(World, TEXT("quit"));
		}
		return;
	}
	if (Current >= Steps.Num())
	{
		FApp::SetUseFixedTimeStep(false);
		bQuit = true;
		Note(FString::Printf(TEXT("script complete, %d failures"), Failures));
		WriteResult();
		GEngine->Exec(World, TEXT("quit"));
		return;
	}
	if (bRecording && !FScreenshotRequest::IsScreenshotRequested())
	{
		FScreenshotRequest::RequestScreenshot(RecordDir / FString::Printf(TEXT("f_%05d.png"), RecordFrame++), false, false);
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
