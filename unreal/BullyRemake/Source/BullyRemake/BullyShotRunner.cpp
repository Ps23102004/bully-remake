// OWNER: builder-B (ue)
#include "BullyShotRunner.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UnrealClient.h"
#include "HighResScreenshot.h"
#include "Engine/GameViewportClient.h"
#if WITH_EDITOR
#include "AssetCompilingManager.h"
#include "ShaderCompiler.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogBullyShot, Log, All);

namespace
{
	bool ReadVec(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, FVector& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Obj->TryGetArrayField(Key, Arr) || Arr->Num() != 3)
		{
			return false;
		}
		Out = FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
		return true;
	}
}

bool UBullyShotRunner::ShouldCreateSubsystem(UObject* Outer) const
{
	FString Unused;
	return FParse::Value(FCommandLine::Get(), TEXT("BRShotFile="), Unused);
}

void UBullyShotRunner::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	FString File;
	FParse::Value(FCommandLine::Get(), TEXT("BRShotFile="), File);
	if (!FParse::Value(FCommandLine::Get(), TEXT("BRShotDir="), ShotDir))
	{
		ShotDir = FPaths::ProjectSavedDir() / TEXT("BullyShots");
	}
	IFileManager::Get().MakeDirectory(*ShotDir, true);
	bActive = LoadShots(File);
	UE_LOG(LogBullyShot, Display, TEXT("BR_SHOT runner %s: %d shots from %s -> %s"),
		bActive ? TEXT("active") : TEXT("INACTIVE (bad shot file)"), Shots.Num(), *File, *ShotDir);
}

void UBullyShotRunner::Deinitialize()
{
	bActive = false;
	Super::Deinitialize();
}

TStatId UBullyShotRunner::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UBullyShotRunner, STATGROUP_Tickables);
}

bool UBullyShotRunner::LoadShots(const FString& Path)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		UE_LOG(LogBullyShot, Error, TEXT("BR_SHOT cannot read %s"), *Path);
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		UE_LOG(LogBullyShot, Error, TEXT("BR_SHOT bad JSON in %s"), *Path);
		return false;
	}
	Root->TryGetNumberField(TEXT("warmup_seconds"), WarmupSeconds);
	Root->TryGetNumberField(TEXT("settle_seconds"), SettleSeconds);
	Root->TryGetNumberField(TEXT("quit_after_seconds"), QuitAfterLastShot);
	Root->TryGetStringArrayField(TEXT("exec"), ExecCommands);
	Root->TryGetNumberField(TEXT("max_compile_wait_seconds"), MaxCompileWaitSeconds);
	const TArray<TSharedPtr<FJsonValue>>* HR = nullptr;
	if (Root->TryGetArrayField(TEXT("highres_size"), HR) && HR->Num() == 2)
	{
		HighResX = int32((*HR)[0]->AsNumber());
		HighResY = int32((*HR)[1]->AsNumber());
	}

	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (!Root->TryGetArrayField(TEXT("shots"), Arr))
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& V : *Arr)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		if (!O.IsValid())
		{
			continue;
		}
		FBullyShot S;
		O->TryGetStringField(TEXT("name"), S.Name);
		if (!ReadVec(O, TEXT("location_cm"), S.Location) || !ReadVec(O, TEXT("look_at_cm"), S.LookAt))
		{
			UE_LOG(LogBullyShot, Error, TEXT("BR_SHOT shot '%s' lacks location_cm/look_at_cm"), *S.Name);
			continue;
		}
		double D = 0.0;
		if (O->TryGetNumberField(TEXT("fov"), D)) { S.Fov = float(D); }
		O->TryGetBoolField(TEXT("ground_trace"), S.bGroundTrace);
		if (O->TryGetNumberField(TEXT("eye_height_cm"), D)) { S.EyeHeight = float(D); }
		O->TryGetBoolField(TEXT("look_at_ground_trace"), S.bLookAtGroundTrace);
		if (O->TryGetNumberField(TEXT("look_at_height_cm"), D)) { S.LookAtHeight = float(D); }
		Shots.Add(S);
	}
	return Shots.Num() > 0;
}

bool UBullyShotRunner::TraceGround(UWorld* World, const FVector& At, FVector& OutHit) const
{
	FCollisionQueryParams Params(SCENE_QUERY_STAT(BullyShotGround), /*bTraceComplex*/ true);
	FHitResult Hit;
	const FVector Start(At.X, At.Y, At.Z + 5000.0);
	const FVector End(At.X, At.Y, At.Z - 20000.0);
	if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params))
	{
		OutHit = Hit.ImpactPoint;
		return true;
	}
	return false;
}

bool UBullyShotRunner::PlaceShot(UWorld* World, FBullyShot& Shot)
{
	APlayerController* PC = UGameplayStatics::GetPlayerController(World, 0);
	if (!PC)
	{
		return false;
	}
	FVector Loc = Shot.Location;
	FVector Target = Shot.LookAt;
	FVector Hit;
	if (Shot.bGroundTrace)
	{
		if (TraceGround(World, Loc, Hit))
		{
			Shot.GroundInfo += FString::Printf(TEXT("camera ground z=%.1f; "), Hit.Z);
			Loc.Z = Hit.Z + Shot.EyeHeight;
		}
		else
		{
			Shot.GroundInfo += TEXT("camera ground trace MISSED (kept given z); ");
		}
	}
	if (Shot.bLookAtGroundTrace)
	{
		if (TraceGround(World, Target, Hit))
		{
			Shot.GroundInfo += FString::Printf(TEXT("target ground z=%.1f; "), Hit.Z);
			Target.Z = Hit.Z + Shot.LookAtHeight;
		}
		else
		{
			Shot.GroundInfo += TEXT("target ground trace MISSED; ");
		}
	}
	Shot.Location = Loc;
	Shot.LookAt = Target;

	if (!Camera.IsValid())
	{
		FActorSpawnParameters SP;
		SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Camera = World->SpawnActor<ACameraActor>(Loc, FRotator::ZeroRotator, SP);
	}
	ACameraActor* Cam = Camera.Get();
	if (!Cam)
	{
		return false;
	}
	Cam->SetActorLocationAndRotation(Loc, FRotationMatrix::MakeFromX((Target - Loc).GetSafeNormal()).Rotator());
	UCameraComponent* CC = Cam->GetCameraComponent();
	CC->SetFieldOfView(Shot.Fov);
	CC->SetConstraintAspectRatio(false);
	CC->PostProcessSettings.bOverride_MotionBlurAmount = true;
	CC->PostProcessSettings.MotionBlurAmount = 0.f;
	PC->SetViewTarget(Cam);
	if (APawn* Pawn = PC->GetPawn())
	{
		Pawn->SetActorHiddenInGame(true);
		Pawn->SetActorLocation(Loc);   // keep streaming/culling centred on the shot
	}
	UE_LOG(LogBullyShot, Display, TEXT("BR_SHOT place '%s' loc (%.0f %.0f %.0f) look (%.0f %.0f %.0f) fov %.0f %s"),
		*Shot.Name, Loc.X, Loc.Y, Loc.Z, Target.X, Target.Y, Target.Z, Shot.Fov, *Shot.GroundInfo);
	return true;
}

void UBullyShotRunner::Tick(float DeltaTime)
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
			UE_LOG(LogBullyShot, Display, TEXT("BR_SHOT exec %s"), *Cmd);
			GEngine->Exec(World, *Cmd);
		}
	}

	// Uncooked -game runs compile shaders, textures and distance fields on demand. Hold the first
	// shot until both queues have been empty for 3 s (cap: MaxCompileWaitSeconds), then warm up.
	if (ReadyAt < 0.0)
	{
		int32 Assets = 0, ShaderJobs = 0;
#if WITH_EDITOR
		Assets = FAssetCompilingManager::Get().GetNumRemainingAssets();
		ShaderJobs = GShaderCompilingManager ? GShaderCompilingManager->GetNumRemainingJobs() : 0;
#endif
		if (Assets == 0 && ShaderJobs == 0)
		{
			IdleSince = (IdleSince < 0.0) ? Elapsed : IdleSince;
		}
		else
		{
			IdleSince = -1.0;
		}
		if (Elapsed - LastCompileLog >= 10.0)
		{
			LastCompileLog = Elapsed;
			UE_LOG(LogBullyShot, Display, TEXT("BR_SHOT waiting for compilation: %d assets, %d shader jobs (t=%.0f)"), Assets, ShaderJobs, Elapsed);
		}
		if ((IdleSince >= 0.0 && Elapsed - IdleSince >= 3.0) || Elapsed >= MaxCompileWaitSeconds)
		{
			ReadyAt = Elapsed;
			UE_LOG(LogBullyShot, Display, TEXT("BR_SHOT compilation %s at t=%.1f; warm-up %.0f s"),
				(Elapsed >= MaxCompileWaitSeconds) ? TEXT("wait CAPPED") : TEXT("idle"), Elapsed, WarmupSeconds);
		}
	}

	if (Current < Shots.Num())
	{
		FBullyShot& S = Shots[Current];
		// First shot also waits for the warm-up (shader compile, streaming, Lumen surface cache).
		const double PlaceAt = (Current == 0) ? 0.5 : 0.0;
		if (!S.bPlaced && Elapsed >= PlaceAt)
		{
			if (PlaceShot(World, S))
			{
				S.bPlaced = true;
				S.PlacedAt = Elapsed;
			}
		}
		const bool bWarm = ReadyAt >= 0.0 && Elapsed >= ReadyAt + WarmupSeconds;
		if (S.bPlaced && !S.bRequested && bWarm && Elapsed >= S.PlacedAt + SettleSeconds)
		{
			S.File = ShotDir / FString::Printf(TEXT("%02d_%s.png"), Current, *S.Name);
			if (HighResX > 0 && HighResY > 0)
			{
				// Exact output size independent of the (display-clamped) viewport; re-renders the
				// current view at HighResX x HighResY after r.HighResScreenshotDelay frames.
				FHighResScreenshotConfig& HR = GetHighResScreenshotConfig();
				HR.SetResolution(uint32(HighResX), uint32(HighResY), 1.0f);
				HR.SetFilename(S.File);
				HR.bMaskEnabled = false;
				HR.bDumpBufferVisualizationTargets = false;
				const bool bOk = GEngine->GameViewport && GEngine->GameViewport->Viewport
					&& GEngine->GameViewport->Viewport->TakeHighResScreenShot();
				if (!bOk)
				{
					UE_LOG(LogBullyShot, Error, TEXT("BR_SHOT HighResShot refused for %s"), *S.File);
				}
			}
			else
			{
				FScreenshotRequest::RequestScreenshot(S.File, /*bShowUI*/ false, /*bAddFilenameSuffix*/ false);
			}
			S.bRequested = true;
			LastShotAt = Elapsed;
			UE_LOG(LogBullyShot, Display, TEXT("BR_SHOT request %s at t=%.1f"), *S.File, Elapsed);
		}
		// Give the screenshot two frames to be captured before moving the camera.
		if (S.bRequested && Elapsed >= LastShotAt + 2.0)
		{
			++Current;
		}
		return;
	}

	if (!bQuitRequested && LastShotAt >= 0.0 && Elapsed >= LastShotAt + QuitAfterLastShot)
	{
		bQuitRequested = true;
		WriteSidecar();
		UE_LOG(LogBullyShot, Display, TEXT("BR_SHOT done, quitting at t=%.1f"), Elapsed);
		GEngine->Exec(World, TEXT("quit"));
	}
}

void UBullyShotRunner::WriteSidecar() const
{
	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	W->WriteObjectStart();
	W->WriteValue(TEXT("format"), TEXT("bully-remake.shots/1"));
	W->WriteArrayStart(TEXT("shots"));
	for (const FBullyShot& S : Shots)
	{
		W->WriteObjectStart();
		W->WriteValue(TEXT("name"), S.Name);
		W->WriteValue(TEXT("file"), S.File);
		W->WriteValue(TEXT("exists"), IFileManager::Get().FileExists(*S.File));
		W->WriteArrayStart(TEXT("location_cm"));
		W->WriteValue(S.Location.X); W->WriteValue(S.Location.Y); W->WriteValue(S.Location.Z);
		W->WriteArrayEnd();
		W->WriteArrayStart(TEXT("look_at_cm"));
		W->WriteValue(S.LookAt.X); W->WriteValue(S.LookAt.Y); W->WriteValue(S.LookAt.Z);
		W->WriteArrayEnd();
		W->WriteValue(TEXT("fov"), S.Fov);
		W->WriteValue(TEXT("ground"), S.GroundInfo);
		W->WriteObjectEnd();
	}
	W->WriteArrayEnd();
	W->WriteObjectEnd();
	W->Close();
	FFileHelper::SaveStringToFile(Out, *(ShotDir / TEXT("shots_result.json")));
}
