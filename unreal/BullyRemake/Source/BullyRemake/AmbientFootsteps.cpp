// OWNER: builder-B4 (ambient)
#include "AmbientFootsteps.h"

#include "BullyCharacter.h"
#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Texture.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundBase.h"

DEFINE_LOG_CATEGORY_STATIC(LogBullyAudio, Log, All);

// ---------------------------------------------------------------------------------------------------------------------
// Surface + cue tables (loaded once per process, read-only afterwards)
// ---------------------------------------------------------------------------------------------------------------------

namespace
{
	struct FStepCue
	{
		FString Cue, Bank, Wave, Asset;
		float PitchCents = 0.f, PitchMin = 0.f, PitchMax = 0.f;
	};
	struct FStepRule
	{
		FString Surface, Scope;
		TArray<FString> Needles;
	};
	struct FStepData
	{
		bool bTried = false;
		bool bOk = false;
		TArray<FStepRule> Rules;
		TArray<FString> InteriorPrefixes;
		TMap<FString, FString> DefaultSurface;                    // context -> surface
		float StartUp = 40.f, Down = 140.f;
		// context -> surface -> role -> gait -> cues
		TMap<FString, TMap<FString, TMap<FString, TMap<FString, TArray<FStepCue>>>>> Table;
	};

	FStepData& StepData()
	{
		static FStepData D;
		if (D.bTried) { return D; }
		D.bTried = true;
		TSharedPtr<FJsonObject> Cfg, Cues;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *(FPaths::ProjectConfigDir() / TEXT("FootstepSurfaces.json"))) ||
			!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Cfg) || !Cfg.IsValid())
		{
			UE_LOG(LogBullyAudio, Warning, TEXT("BR_AUDIO Config/FootstepSurfaces.json missing or invalid: per-surface footsteps off"));
			return D;
		}
		if (!FFileHelper::LoadFileToString(Text, *(FPaths::ProjectContentDir() / TEXT("Imported/Audio/footstep_cues.json"))) ||
			!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Cues) || !Cues.IsValid())
		{
			UE_LOG(LogBullyAudio, Warning, TEXT("BR_AUDIO Content/Imported/Audio/footstep_cues.json missing (run python3 -m tools.importers audio footsteps; tools/ue/run_import_ambient.sh --steps audio): per-surface footsteps off"));
			return D;
		}
		for (const TSharedPtr<FJsonValue>& V : Cfg->GetArrayField(TEXT("rules")))
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			FStepRule R;
			R.Surface = O->GetStringField(TEXT("surface"));
			O->TryGetStringField(TEXT("scope"), R.Scope);
			for (const TSharedPtr<FJsonValue>& N : O->GetArrayField(TEXT("match"))) { R.Needles.Add(N->AsString().ToLower()); }
			D.Rules.Add(R);
		}
		for (const TSharedPtr<FJsonValue>& V : Cfg->GetArrayField(TEXT("interior_scene_prefixes"))) { D.InteriorPrefixes.Add(V->AsString().ToLower()); }
		if (const TSharedPtr<FJsonObject>* Def; Cfg->TryGetObjectField(TEXT("default_surface"), Def))
		{
			for (const auto& KV : (*Def)->Values) { D.DefaultSurface.Add(FString(KV.Key), KV.Value->AsString()); }
		}
		if (const TSharedPtr<FJsonObject>* Pr; Cfg->TryGetObjectField(TEXT("probe"), Pr))
		{
			double X;
			if ((*Pr)->TryGetNumberField(TEXT("start_up_cm"), X)) { D.StartUp = float(X); }
			if ((*Pr)->TryGetNumberField(TEXT("down_cm"), X)) { D.Down = float(X); }
		}
		int32 NCues = 0;
		const TSharedPtr<FJsonObject>* Ctxs = nullptr;
		if (Cues->TryGetObjectField(TEXT("contexts"), Ctxs))
		{
			for (const auto& C : (*Ctxs)->Values)
			{
				for (const auto& Sf : C.Value->AsObject()->Values)
				{
					for (const auto& Ro : Sf.Value->AsObject()->Values)
					{
						for (const auto& Ga : Ro.Value->AsObject()->Values)
						{
							TArray<FStepCue>& Out = D.Table.FindOrAdd(FString(C.Key)).FindOrAdd(FString(Sf.Key)).FindOrAdd(FString(Ro.Key)).FindOrAdd(FString(Ga.Key));
							for (const TSharedPtr<FJsonValue>& CV : Ga.Value->AsArray())
							{
								const TSharedPtr<FJsonObject> O = CV->AsObject();
								FStepCue Q;
								Q.Cue = O->GetStringField(TEXT("cue"));
								Q.Bank = O->GetStringField(TEXT("bank"));
								Q.Wave = O->GetStringField(TEXT("wave"));
								Q.Asset = O->GetStringField(TEXT("asset"));
								Q.PitchCents = float(O->GetNumberField(TEXT("pitch_cents")));
								Q.PitchMin = float(O->GetNumberField(TEXT("pitch_min")));
								Q.PitchMax = float(O->GetNumberField(TEXT("pitch_max")));
								Out.Add(Q);
								++NCues;
							}
						}
					}
				}
			}
		}
		D.bOk = NCues > 0 && D.Rules.Num() > 0;
		UE_LOG(LogBullyAudio, Display, TEXT("BR_AUDIO footstep tables: %d rules, %d cue entries, contexts %d -> %s"), D.Rules.Num(), NCues, D.Table.Num(), D.bOk ? TEXT("ON") : TEXT("OFF"));
		return D;
	}

	const TCHAR* GaitKey(EBullyGait G)
	{
		return G == EBullyGait::Walk ? TEXT("walk") : G == EBullyGait::Sprint ? TEXT("sprint") : TEXT("run");
	}
}

bool UBullyFootstepComponent::TablesAvailable()
{
	return StepData().bOk;
}

USoundBase* UBullyFootstepComponent::LoadCueSound(const FString& Asset)
{
	if (TObjectPtr<USoundBase>* Hit = CueSounds.Find(Asset)) { return Hit->Get(); }
	const FString Short = FPackageName::GetShortName(Asset);
	USoundBase* S = LoadObject<USoundBase>(nullptr, *FString::Printf(TEXT("%s.%s"), *Asset, *Short), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!S) { UE_LOG(LogBullyAudio, Warning, TEXT("BR_AUDIO cue wave %s not found (re-run tools/ue/run_import_ambient.sh --steps audio)"), *Asset); }
	CueSounds.Add(Asset, S);
	return S;
}

FString UBullyFootstepComponent::ProbeSurface(const FVector& Where, FString& OutWhy, bool& bOutInterior)
{
	FStepData& D = StepData();
	UWorld* W = GetWorld();
	bOutInterior = false;
	if (!W) { OutWhy = TEXT("no world"); return D.DefaultSurface.FindRef(TEXT("exterior")); }
	FHitResult Hit;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(BullyFootSurface), /*bTraceComplex*/ true, GetOwner());
	Q.bReturnFaceIndex = true;
	FCollisionObjectQueryParams Obj;
	Obj.AddObjectTypesToQuery(ECC_WorldStatic);
	Obj.AddObjectTypesToQuery(ECC_WorldDynamic);
	const bool bHit = W->LineTraceSingleByObjectType(Hit, Where + FVector(0.f, 0.f, D.StartUp), Where - FVector(0.f, 0.f, D.Down), Obj, Q);
	FString Tex, Model, SceneName;
	if (bHit && Hit.GetActor())
	{
		for (const FName& T : Hit.GetActor()->Tags)
		{
			const FString TS = T.ToString();
			if (TS.StartsWith(TEXT("br_model:"))) { Model = TS.Mid(9); }
			else if (TS.StartsWith(TEXT("br_scene:"))) { SceneName = TS.Mid(9).ToLower(); }
		}
		for (const FString& P : D.InteriorPrefixes)
		{
			if (SceneName.StartsWith(P)) { bOutInterior = true; break; }
		}
		UPrimitiveComponent* Comp = Hit.GetComponent();
		int32 Section = 0;
		UMaterialInterface* Mat = Comp ? Comp->GetMaterialFromCollisionFaceIndex(Hit.FaceIndex, Section) : nullptr;
		if (!Mat && Comp) { Mat = Comp->GetMaterial(0); }
		if (Mat)
		{
			UTexture* T = nullptr;
			if (Mat->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("BaseColor")), T) && T) { Tex = T->GetName(); }
		}
		OutWhy = FString::Printf(TEXT("tex=%s model=%s scene=%s face=%d"), Tex.IsEmpty() ? TEXT("-") : *Tex, *Model, *SceneName, Hit.FaceIndex);
	}
	else
	{
		OutWhy = TEXT("no ground hit");
	}
	const FString Ctx = bOutInterior ? TEXT("interior") : TEXT("exterior");
	const FString Hay = (Tex + TEXT(" ") + Model).ToLower();
	if (!Hay.TrimStartAndEnd().IsEmpty())
	{
		for (const FStepRule& R : D.Rules)
		{
			if (!R.Scope.IsEmpty() && R.Scope != Ctx) { continue; }
			for (const FString& N : R.Needles)
			{
				if (Hay.Contains(N)) { return R.Surface; }
			}
		}
	}
	return D.DefaultSurface.FindRef(Ctx);
}

UBullyFootstepComponent::UBullyFootstepComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

void UBullyFootstepComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	ACharacter* C = Cast<ACharacter>(GetOwner());
	UWorld* W = GetWorld();
	if (!C || !W || (Sounds.Num() == 0 && !StepData().bOk) || DeltaTime <= 0.f)
	{
		return;
	}
	USkeletalMeshComponent* M = C->GetMesh();
	static const FName Feet[2] = { TEXT("Root-L-Foot"), TEXT("Root-R-Foot") };
	if (!M || !M->GetSkeletalMeshAsset() || M->GetBoneIndex(Feet[0]) == INDEX_NONE || M->GetBoneIndex(Feet[1]) == INDEX_NONE)
	{
		return;
	}
	const double Now = W->GetTimeSeconds();
	if (!bInit)
	{
		Born = Now;
		bInit = true;
	}
	const bool bGrounded = !C->GetCharacterMovement() || !C->GetCharacterMovement()->IsFalling();
	const float Speed = FVector(C->GetVelocity().X, C->GetVelocity().Y, 0.f).Size();
	const float Bottom = C->GetActorLocation().Z - C->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	FVector P[2];
	float H[2];
	for (int32 i = 0; i < 2; ++i)
	{
		P[i] = M->GetBoneLocation(Feet[i]);
		H[i] = P[i].Z - Bottom;
		MinZ[i] = FMath::Min(MinZ[i], H[i]);
	}
	// shared floor reference: the lowest ankle height ever seen on either foot (both feet are the same height planted)
	const float Floor = FMath::Min(MinZ[0], MinZ[1]);
	for (int32 i = 0; i < 2; ++i)
	{
		const bool bNowDown = H[i] < Floor + PlantTolerance;
		// ignore the first 0.4 s (pose still settling from the reference pose)
		if (!bDown[i] && bNowDown && Now - Born > 0.4 && bGrounded && Speed > MinSpeed && Now - LastStep[i] > MinStepInterval)
		{
			LastStep[i] = Now;
			PlayStep(i, FVector(P[i].X, P[i].Y, Bottom));
		}
		bDown[i] = bNowDown;
	}
}

void UBullyFootstepComponent::PlayStep(int32 Foot, const FVector& Where)
{
	UWorld* W = GetWorld();
	APlayerController* PC = UGameplayStatics::GetPlayerController(W, 0);
	const APawn* Listener = PC ? PC->GetPawn() : nullptr;
	const float Dist = Listener ? FVector::Dist(Listener->GetActorLocation(), Where) : 0.f;
	if (Listener && Dist > MaxAudibleDistance)
	{
		return;
	}
	// --- per-surface cue (polish1) ---------------------------------------------------------------------------------------
	FString SurfaceNote;
	float CuePitchCents = 0.f;
	bool bCue = false;
	USoundBase* S = nullptr;
	if (StepData().bOk)
	{
		FStepData& D = StepData();
		FString Why;
		bool bInterior = false;
		const FString Surface = ProbeSurface(Where, Why, bInterior);
		const FString Ctx = bInterior ? TEXT("interior") : TEXT("exterior");
		const ABullyCharacter* BC = Cast<ABullyCharacter>(GetOwner());
		const FString Gait = GaitKey(BC ? BC->GetGait() : EBullyGait::Walk);
		const TArray<FStepCue>* List = nullptr;
		FString UsedRole = Role, UsedSurface = Surface;
		for (const FString& Rl : { Role, FString(TEXT("student")) })
		{
			for (const FString& Sf : { Surface, D.DefaultSurface.FindRef(Ctx) })
			{
				const auto* CtxMap = D.Table.Find(Ctx);
				const auto* SfMap = CtxMap ? CtxMap->Find(Sf) : nullptr;
				const auto* RoleMap = SfMap ? SfMap->Find(Rl) : nullptr;
				if (RoleMap && RoleMap->Find(Gait)) { List = RoleMap->Find(Gait); UsedRole = Rl; UsedSurface = Sf; break; }
			}
			if (List) { break; }
		}
		if (Surface != LastSurface)
		{
			UE_LOG(LogBullyAudio, Display, TEXT("BR_AUDIO surface %s (%s): %s -> %s [%s, %s]"), *Tag, *Role, LastSurface.IsEmpty() ? TEXT("-") : *LastSurface, *Surface, *Why, *Ctx);
			LastSurface = Surface;
		}
		if (List && List->Num() > 0)
		{
			int32 Idx = FMath::RandRange(0, List->Num() - 1);
			if (List->Num() > 1 && (*List)[Idx].Cue == LastCue)
			{
				Idx = (Idx + 1 + FMath::RandRange(0, List->Num() - 2)) % List->Num();
			}
			const FStepCue& C = (*List)[Idx];
			LastCue = C.Cue;
			S = LoadCueSound(C.Asset);
			CuePitchCents = C.PitchCents + FMath::FRandRange(C.PitchMin, C.PitchMax);
			bCue = S != nullptr;
			SurfaceNote = FString::Printf(TEXT("surface=%s role=%s gait=%s cue=%s"), *UsedSurface, *UsedRole, *Gait, *C.Cue);
		}
	}
	if (!S && Sounds.Num() > 0)
	{
		int32 Idx = FMath::RandRange(0, Sounds.Num() - 1);
		if (Sounds.Num() > 1 && Idx == LastIndex)
		{
			Idx = (Idx + 1 + FMath::RandRange(0, Sounds.Num() - 2)) % Sounds.Num();
		}
		LastIndex = Idx;
		S = Sounds[Idx];
		SurfaceNote = TEXT("surface=legacy-cement");
	}
	if (!S)
	{
		return;
	}
	if (!Attenuation)
	{
		Attenuation = NewObject<USoundAttenuation>(this);
		Attenuation->Attenuation.bAttenuate = true;
		Attenuation->Attenuation.bSpatialize = true;
		Attenuation->Attenuation.AttenuationShape = EAttenuationShape::Sphere;
		Attenuation->Attenuation.AttenuationShapeExtents = FVector(InnerRadius, 0.f, 0.f);
		Attenuation->Attenuation.FalloffDistance = FalloffDistance;
		Attenuation->Attenuation.DistanceAlgorithm = EAttenuationDistanceModel::Linear;
	}
	// XACT pitch units are cents (the waves named '8va' are stored an octave up and cued at -1200)
	const float Pitch = bCue ? FMath::Pow(2.f, CuePitchCents / 1200.f) : 1.f + FMath::FRandRange(-PitchVariation, PitchVariation);
	const float Vol = Volume * FMath::FRandRange(0.85f, 1.f);
	UGameplayStatics::PlaySoundAtLocation(W, S, Where, FRotator::ZeroRotator, Vol, Pitch, 0.f, Attenuation);
	++StepCount;
	UE_LOG(LogBullyAudio, Display, TEXT("BR_AUDIO step #%d %s foot=%s %s wave=%s vol=%.2f pitch=%.3f dist=%.0f t=%.2f"),
		StepCount, *Tag, Foot == 0 ? TEXT("L") : TEXT("R"), *SurfaceNote, *S->GetName(), Vol, Pitch, Dist, W->GetTimeSeconds());
}
