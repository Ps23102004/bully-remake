// OWNER: builder-B4 (ambient)
#include "AmbientSubsystem.h"

#include "AmbientFootsteps.h"
#include "AmbientWalker.h"
#include "Components/AudioComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "AudioDevice.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/FileHelper.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundWave.h"
#include "TimerManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogAmbientSys, Log, All);

namespace
{
	/** Same rule as tools/ue/import_ambient.py asset_name(): runs of non [A-Za-z0-9_] -> "_", trimmed. */
	FString AssetName(const FString& In)
	{
		FString Out;
		bool bLastUnderscore = false;
		for (TCHAR C : In)
		{
			const bool bOk = FChar::IsAlnum(C) && C < 128 || C == TEXT('_');
			if (bOk) { Out.AppendChar(C); bLastUnderscore = false; }
			else if (!bLastUnderscore) { Out.AppendChar(TEXT('_')); bLastUnderscore = true; }
		}
		Out.TrimCharInline(TEXT('_'), nullptr);
		return Out;
	}

	TSharedPtr<FJsonObject> ReadAmbientConfig()
	{
		const FString Path = FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("AmbientLoops.json"));
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path)) { return nullptr; }
		TSharedPtr<FJsonObject> Obj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
		{
			UE_LOG(LogAmbientSys, Error, TEXT("BR_AMBIENT %s is not valid JSON"), *Path);
			return nullptr;
		}
		return Obj;
	}

	FVector ReadVec(const TArray<TSharedPtr<FJsonValue>>& A)
	{
		return A.Num() >= 3 ? FVector(A[0]->AsNumber(), A[1]->AsNumber(), A[2]->AsNumber()) : FVector::ZeroVector;
	}
}

// ---------------------------------------------------------------------------------------------------------------

AAmbientSoundActor::AAmbientSoundActor()
{
	PrimaryActorTick.bCanEverTick = true;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
}

void AAmbientSoundActor::Start(const TArray<USoundBase*>& Sounds, const TArray<float>& Volumes, float InnerRadius, float Falloff)
{
	Attenuation = NewObject<USoundAttenuation>(this);
	Attenuation->Attenuation.bAttenuate = true;
	Attenuation->Attenuation.bSpatialize = true;
	Attenuation->Attenuation.AttenuationShape = EAttenuationShape::Sphere;
	Attenuation->Attenuation.AttenuationShapeExtents = FVector(InnerRadius, 0.f, 0.f);
	Attenuation->Attenuation.FalloffDistance = Falloff;
	Attenuation->Attenuation.DistanceAlgorithm = EAttenuationDistanceModel::Linear;
	for (int32 i = 0; i < Sounds.Num(); ++i)
	{
		UAudioComponent* A = NewObject<UAudioComponent>(this);
		A->SetupAttachment(GetRootComponent());
		A->bAutoActivate = false;
		A->bAllowSpatialization = true;
		A->AttenuationSettings = Attenuation;
		A->SetSound(Sounds[i]);
		A->SetVolumeMultiplier(Volumes.IsValidIndex(i) ? Volumes[i] : 1.f);
		A->RegisterComponent();
		A->Play();
		Components.Add(A);
		UE_LOG(LogAmbientSys, Display, TEXT("BR_AUDIO ambient loop play %s vol=%.2f looping=%d duration=%.2f s at %s inner=%.0f falloff=%.0f isPlaying=%d audioDevice=%d"),
			*Sounds[i]->GetName(), Volumes.IsValidIndex(i) ? Volumes[i] : 1.f, Sounds[i]->IsLooping() ? 1 : 0, Sounds[i]->GetDuration(),
			*GetActorLocation().ToString(), InnerRadius, Falloff, A->IsPlaying() ? 1 : 0, GEngine && GEngine->GetMainAudioDevice() ? 1 : 0);
	}
}

void AAmbientSoundActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const double Now = GetWorld()->GetTimeSeconds();
	if (Now >= NextLog)
	{
		NextLog = Now + 10.0;
		for (UAudioComponent* A : Components)
		{
			UE_LOG(LogAmbientSys, Display, TEXT("BR_AUDIO ambient t=%.1f %s isPlaying=%d audioDevice=%d"), Now, A && A->Sound ? *A->Sound->GetName() : TEXT("?"), A && A->IsPlaying() ? 1 : 0, GEngine && GEngine->GetMainAudioDevice() ? 1 : 0);
		}
	}
}

void AAmbientSoundActor::EndPlay(const EEndPlayReason::Type Reason)
{
	for (UAudioComponent* A : Components) { if (A) { A->Stop(); } }
	Super::EndPlay(Reason);
}

// ---------------------------------------------------------------------------------------------------------------

bool UAmbientSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* W = Cast<UWorld>(Outer);
	if (!W || !W->IsGameWorld() || FParse::Param(FCommandLine::Get(), TEXT("BRNoAmbient")))
	{
		return false;
	}
	return W->GetName().Contains(TEXT("Bullworth")) && FPaths::FileExists(FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("AmbientLoops.json")));
}

USoundBase* UAmbientSubsystem::LoadGameSound(const FString& Bank, const FString& Name) const
{
	const FString A = AssetName(Name);
	const FString Path = FString::Printf(TEXT("/Game/Imported/Audio/%s/%s.%s"), *Bank, *A, *A);
	USoundBase* S = LoadObject<USoundBase>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!S)
	{
		UE_LOG(LogAmbientSys, Warning, TEXT("BR_AMBIENT sound missing (run tools/ue/run_import_ambient.sh): %s"), *Path);
	}
	return S;
}

void UAmbientSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	FString ProbeArgs;
	if (FParse::Value(FCommandLine::Get(), TEXT("BRAmbientProbe="), ProbeArgs, false))
	{
		FTimerHandle H;
		InWorld.GetTimerManager().SetTimer(H, FTimerDelegate::CreateLambda([this, &InWorld, ProbeArgs]() { RunProbe(InWorld, ProbeArgs); }), 4.f, false);
		return;
	}
	Spawn(InWorld);
}

void UAmbientSubsystem::RunProbe(UWorld& World, const FString& Args)
{
	TArray<FString> P;
	Args.ParseIntoArray(P, TEXT(","));
	if (P.Num() < 5) { UE_LOG(LogAmbientSys, Error, TEXT("BR_PROBE needs x0,y0,x1,y1,step[,zTop,zBottom]")); FPlatformMisc::RequestExit(false); return; }
	const float X0 = FCString::Atof(*P[0]), Y0 = FCString::Atof(*P[1]), X1 = FCString::Atof(*P[2]), Y1 = FCString::Atof(*P[3]), Step = FMath::Max(25.f, FCString::Atof(*P[4]));
	const float ZTop = P.Num() > 5 ? FCString::Atof(*P[5]) : 900.f, ZBot = P.Num() > 6 ? FCString::Atof(*P[6]) : 300.f;
	FCollisionQueryParams QP(SCENE_QUERY_STAT(AmbientProbe), /*bTraceComplex*/ true);
	const FCollisionShape Cap = FCollisionShape::MakeCapsule(30.f, 85.f);
	UE_LOG(LogAmbientSys, Display, TEXT("BR_PROBE legend: '.' open ground (capsule free, slope<25deg), ',' open but sloped, '#' ground but capsule blocked, '_' no ground. rows = X (cm), columns = Y from %.0f step %.0f; ground z in the second map = (z - zBottom)/10 as 0-9A-Za-z (10 cm levels)"), Y0, Step);
	for (float X = X0; X <= X1 + 0.1f; X += Step)
	{
		FString Row, ZRow;
		for (float Y = Y0; Y <= Y1 + 0.1f; Y += Step)
		{
			TArray<FHitResult> Hits;
			World.LineTraceMultiByChannel(Hits, FVector(X, Y, ZTop), FVector(X, Y, ZBot), ECC_Visibility, QP);
			TCHAR C = TEXT('_');
			float Z = 0.f;
			for (const FHitResult& H : Hits)
			{
				if (H.ImpactNormal.Z < 0.5f) { continue; }
				const FVector Centre = H.ImpactPoint + FVector(0, 0, 87.f);
				const bool bBlocked = World.OverlapBlockingTestByChannel(Centre, FQuat::Identity, ECC_Pawn, Cap, QP);
				C = bBlocked ? TEXT('#') : (H.ImpactNormal.Z > 0.906f ? TEXT('.') : TEXT(','));
				Z = H.ImpactPoint.Z;
				if (!bBlocked) { break; }
			}
			Row.AppendChar(C);
			static const TCHAR Levels[] = TEXT("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
			ZRow.AppendChar(C == TEXT('_') ? TEXT(' ') : Levels[FMath::Clamp(int32((Z - ZBot) / 10.f), 0, 61)]);
		}
		UE_LOG(LogAmbientSys, Display, TEXT("BR_PROBE x=%6.0f |%s| z|%s"), X, *Row, *ZRow);
	}
	UE_LOG(LogAmbientSys, Display, TEXT("BR_PROBE done"));
	FPlatformMisc::RequestExit(false);
}

void UAmbientSubsystem::Deinitialize()
{
	if (CamTicker.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(CamTicker); }
	if (UWorld* W = GetWorld()) { W->GetTimerManager().ClearTimer(PlayerAttachTimer); }
	Super::Deinitialize();
}

void UAmbientSubsystem::ValidateLoop(UWorld& World, const FString& Id, const TArray<FVector>& Pts)
{
	// Static check of the authored path against the level's collision, with the walkers' own capsule: a ground hit under
	// every waypoint (within the authored z band), and a capsule sweep along each segment with the capsule lifted by the max step height (45 cm).
	FCollisionQueryParams P(SCENE_QUERY_STAT(AmbientValidate), /*bTraceComplex*/ true);
	const FCollisionShape Cap = FCollisionShape::MakeCapsule(30.f, 85.f);
	TArray<FVector> Ground;
	int32 Bad = 0;
	for (const FVector& A : Pts)
	{
		FHitResult Hit;
		const bool bGround = World.LineTraceSingleByChannel(Hit, FVector(A.X, A.Y, A.Z + 150.f), FVector(A.X, A.Y, A.Z - 250.f), ECC_Visibility, P);
		Ground.Add(bGround ? Hit.ImpactPoint : A);
		if (!bGround) { ++Bad; }
	}
	for (int32 i = 0; i < Pts.Num(); ++i)
	{
		const int32 j = (i + 1) % Pts.Num();
		const FVector From = Ground[i] + FVector(0, 0, 132.f), To = Ground[j] + FVector(0, 0, 132.f);   // 85 + 45 cm step height: low kerbs/cobbles are steppable
		FHitResult Seg;
		const bool bBlocked = World.SweepSingleByChannel(Seg, From, To, FQuat::Identity, ECC_Pawn, Cap, P);
		if (bBlocked) { ++Bad; }
		const FString SegStr = bBlocked ? FString::Printf(TEXT("BLOCKED by %s at %s"), *GetNameSafe(Seg.GetActor()), *Seg.ImpactPoint.ToString()) : FString(TEXT("clear"));
		UE_LOG(LogAmbientSys, Display, TEXT("BR_AMBIENT path-check loop=%s wp%d ground z=%.0f -> wp%d (%.1f m): %s"), *Id, i, Ground[i].Z, j, FVector::Dist2D(From, To) / 100.f, *SegStr);
	}
	UE_LOG(LogAmbientSys, Display, TEXT("BR_AMBIENT path-check loop=%s: %d waypoints, %d problems"), *Id, Pts.Num(), Bad);
}

void UAmbientSubsystem::Spawn(UWorld& World)
{
	TSharedPtr<FJsonObject> Cfg = ReadAmbientConfig();
	if (!Cfg) { return; }

	// footstep sets
	TMap<FString, TArray<TObjectPtr<USoundBase>>> StepSets;
	if (const TSharedPtr<FJsonObject>* Fs; Cfg->TryGetObjectField(TEXT("footsteps"), Fs))
	{
		for (const auto& KV : (*Fs)->Values)
		{
			const FString KeyStr = FString(KV.Key);
			TArray<TObjectPtr<USoundBase>>& Set = StepSets.FindOrAdd(KeyStr);
			for (const TSharedPtr<FJsonValue>& V : KV.Value->AsArray())
			{
				const TSharedPtr<FJsonObject> O = V->AsObject();
				if (USoundBase* S = O ? LoadGameSound(O->GetStringField(TEXT("bank")), O->GetStringField(TEXT("name"))) : nullptr) { Set.Add(S); }
			}
			UE_LOG(LogAmbientSys, Display, TEXT("BR_AMBIENT footstep set '%s': %d sounds loaded"), *KeyStr, Set.Num());
		}
	}
	PlayerSteps = StepSets.FindRef(TEXT("player"));

	// ambient bed
	if (const TSharedPtr<FJsonObject>* Amb; Cfg->TryGetObjectField(TEXT("ambient"), Amb))
	{
		TArray<USoundBase*> Sounds;
		TArray<float> Vols;
		for (const TSharedPtr<FJsonValue>& V : (*Amb)->GetArrayField(TEXT("sounds")))
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			if (USoundBase* S = LoadGameSound(O->GetStringField(TEXT("bank")), O->GetStringField(TEXT("name"))))
			{
				Sounds.Add(S);
				Vols.Add(O->HasField(TEXT("volume")) ? O->GetNumberField(TEXT("volume")) : 1.f);
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* Loc;
		FVector At = FVector(26000, 7300, 1300);
		if ((*Amb)->TryGetArrayField(TEXT("location_cm"), Loc)) { At = ReadVec(*Loc); }
		if (Sounds.Num())
		{
			FActorSpawnParameters SP;
			SP.Name = TEXT("AmbientSoundActor");
			AAmbientSoundActor* A = World.SpawnActor<AAmbientSoundActor>(At, FRotator::ZeroRotator, SP);
			double Inner = (*Amb)->HasField(TEXT("inner_cm")) ? (*Amb)->GetNumberField(TEXT("inner_cm")) : 2500.0;
			double Fall = (*Amb)->HasField(TEXT("falloff_cm")) ? (*Amb)->GetNumberField(TEXT("falloff_cm")) : 9000.0;
			if (A) { A->Start(Sounds, Vols, Inner, Fall); AmbientActor = A; }
		}
	}

	// students and loops
	TMap<FString, TSharedPtr<FJsonObject>> Students;
	for (const TSharedPtr<FJsonValue>& V : Cfg->GetArrayField(TEXT("students")))
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		Students.Add(O->GetStringField(TEXT("id")), O);
	}
	int32 Count = 0;
	for (const TSharedPtr<FJsonValue>& V : Cfg->GetArrayField(TEXT("loops")))
	{
		const TSharedPtr<FJsonObject> L = V->AsObject();
		const FString LoopId = L->GetStringField(TEXT("id"));
		TArray<FVector> Pts;
		for (const TSharedPtr<FJsonValue>& P : L->GetArrayField(TEXT("waypoints_cm"))) { Pts.Add(ReadVec(P->AsArray())); }
		if (Pts.Num() < 2) { continue; }
		ValidateLoop(World, LoopId, Pts);
		// one or more walkers per loop, spread along it
		for (const TSharedPtr<FJsonValue>& WV : L->GetArrayField(TEXT("walkers")))
		{
			const TSharedPtr<FJsonObject> WO = WV->AsObject();
			const FString SId = WO->GetStringField(TEXT("student"));
			const TSharedPtr<FJsonObject>* St = Students.Find(SId);
			if (!St) { UE_LOG(LogAmbientSys, Warning, TEXT("BR_AMBIENT loop %s: unknown student %s"), *LoopId, *SId); continue; }
			const FString Model = (*St)->GetStringField(TEXT("model"));
			const FString MeshPath = FString::Printf(TEXT("/Game/Imported/Characters/%s/SK_%s.SK_%s"), *Model, *Model, *Model);
			const int32 Start = WO->HasField(TEXT("start_index")) ? (int32)WO->GetNumberField(TEXT("start_index")) : 0;
			AAmbientWalker* Wk = World.SpawnActorDeferred<AAmbientWalker>(AAmbientWalker::StaticClass(),
				FTransform(FRotator::ZeroRotator, Pts[Start % Pts.Num()] + FVector(0, 0, 90.f)), nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			if (!Wk) { continue; }
			Wk->MeshPath = MeshPath;
			Wk->LoopId = LoopId;
			Wk->StudentId = SId;
			Wk->Waypoints = Pts;
			Wk->StartIndex = Start;
			Wk->WalkSpeed = WO->HasField(TEXT("speed_cm_s")) ? WO->GetNumberField(TEXT("speed_cm_s")) : (L->HasField(TEXT("speed_cm_s")) ? L->GetNumberField(TEXT("speed_cm_s")) : 185.0);
			if (L->HasField(TEXT("idle_min"))) { Wk->IdleSecondsRange = FVector2D(L->GetNumberField(TEXT("idle_min")), L->GetNumberField(TEXT("idle_max"))); }
			Wk->FinishSpawning(Wk->GetActorTransform());
			if (UBullyFootstepComponent* F = NewObject<UBullyFootstepComponent>(Wk, TEXT("Footsteps")))
			{
				F->Sounds = StepSets.FindRef((*St)->GetStringField(TEXT("footsteps")));
				F->Role = (*St)->GetStringField(TEXT("footsteps")) == TEXT("female") ? TEXT("female") : TEXT("student");
				F->Tag = SId;
				F->RegisterComponent();
			}
			Walkers.Add(Wk);
			++Count;
		}
	}
	UE_LOG(LogAmbientSys, Display, TEXT("BR_AMBIENT spawned %d walkers"), Count);

	World.GetTimerManager().SetTimer(PlayerAttachTimer, FTimerDelegate::CreateUObject(this, &UAmbientSubsystem::AttachPlayerFootsteps), 0.5f, true);

	FString CamSpec;
	if (FParse::Value(FCommandLine::Get(), TEXT("BRAmbientCam="), CamSpec, false))
	{
		StartEvidenceCamera(World, CamSpec);
	}
}

void UAmbientSubsystem::StartEvidenceCamera(UWorld& World, const FString& Spec)
{
	TArray<FString> Parts;
	Spec.ParseIntoArray(Parts, TEXT(","));
	for (const FString& Part : Parts)
	{
		TArray<FString> F;
		Part.ParseIntoArray(F, TEXT(":"));
		if (F.Num() >= 7 && F[0] == TEXT("fixed"))
		{
			FCamShot S;
			S.bFixed = true;
			S.Loc = FVector(FCString::Atof(*F[1]), FCString::Atof(*F[2]), FCString::Atof(*F[3]));
			S.Look = FVector(FCString::Atof(*F[4]), FCString::Atof(*F[5]), FCString::Atof(*F[6]));
			S.Seconds = F.Num() > 7 ? FCString::Atof(*F[7]) : 1e9f;
			CamShots.Add(S);
		}
		else if (F.Num() >= 1)
		{
			FCamShot S;
			S.Student = F[0];
			S.Seconds = F.Num() > 1 ? FCString::Atof(*F[1]) : 8.f;
			CamShots.Add(S);
		}
	}
	FActorSpawnParameters SP;
	SP.Name = TEXT("AmbientEvidenceCamera");
	ACameraActor* Cam = World.SpawnActor<ACameraActor>(FVector::ZeroVector, FRotator::ZeroRotator, SP);
	if (!Cam) { return; }
	Cam->GetCameraComponent()->SetFieldOfView(70.f);
	CamActor = Cam;
	CamShotStart = World.GetTimeSeconds();
	if (APlayerController* PC = UGameplayStatics::GetPlayerController(&World, 0)) { PC->SetViewTarget(Cam); }
	CamTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UAmbientSubsystem::TickEvidenceCamera), 0.f);
	UE_LOG(LogAmbientSys, Display, TEXT("BR_AMBIENT evidence camera (%s): %d shots"), TEXT("spec"), CamShots.Num());
}

bool UAmbientSubsystem::TickEvidenceCamera(float Dt)
{
	UWorld* W = GetWorld();
	AActor* Cam = CamActor.Get();
	if (!W || !Cam) { return false; }
	if (APlayerController* PC = UGameplayStatics::GetPlayerController(W, 0))
	{
		if (PC->GetViewTarget() != Cam) { PC->SetViewTarget(Cam); }
	}
	if (CamShots.Num() == 0) { return true; }
	const double Now = W->GetTimeSeconds();
	if (Now - CamShotStart > CamShots[CamIndex].Seconds)
	{
		CamIndex = (CamIndex + 1) % CamShots.Num();
		CamShotStart = Now;
		CamPos = FVector::ZeroVector;
	}
	if (CamShots[CamIndex].bFixed)
	{
		Cam->SetActorLocationAndRotation(CamShots[CamIndex].Loc, (CamShots[CamIndex].Look - CamShots[CamIndex].Loc).Rotation());
		return true;
	}
	AAmbientWalker* Target = nullptr;
	for (const TWeakObjectPtr<AAmbientWalker>& Wk : Walkers)
	{
		if (Wk.IsValid() && Wk->StudentId == CamShots[CamIndex].Student) { Target = Wk.Get(); break; }
	}
	if (!Target) { return true; }
	const FVector V = FVector(Target->GetVelocity().X, Target->GetVelocity().Y, 0.f);
	if (V.Size() > 40.f) { CamHeading = FMath::VInterpNormalRotationTo(CamHeading, V.GetSafeNormal(), Dt, 90.f); }
	const FVector Right = FVector::CrossProduct(FVector::UpVector, CamHeading).GetSafeNormal();
	const FVector Foot = Target->GetActorLocation() - FVector(0, 0, 85.f);
	const FVector Want = Foot - CamHeading * 350.f + Right * 150.f + FVector(0, 0, 140.f);
	CamPos = CamPos.IsZero() ? Want : FMath::VInterpTo(CamPos, Want, Dt, 4.f);
	Cam->SetActorLocationAndRotation(CamPos, (Foot + FVector(0, 0, 105.f) - CamPos).Rotation());
	return true;
}

void UAmbientSubsystem::AttachPlayerFootsteps()
{
	UWorld* W = GetWorld();
	APawn* P = W ? UGameplayStatics::GetPlayerPawn(W, 0) : nullptr;
	if (!P || bPlayerAttached || (PlayerSteps.Num() == 0 && !UBullyFootstepComponent::TablesAvailable())) { return; }
	UBullyFootstepComponent* F = NewObject<UBullyFootstepComponent>(P, TEXT("Footsteps"));
	F->Sounds = PlayerSteps;
	F->Role = TEXT("player");
	F->Tag = TEXT("player");
	F->Volume = 0.9f;
	F->RegisterComponent();
	bPlayerAttached = true;
	W->GetTimerManager().ClearTimer(PlayerAttachTimer);
	UE_LOG(LogAmbientSys, Display, TEXT("BR_AMBIENT footsteps attached to player pawn %s (%d sounds)"), *P->GetName(), PlayerSteps.Num());
}
