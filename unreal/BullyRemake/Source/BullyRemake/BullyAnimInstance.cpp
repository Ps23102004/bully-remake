// OWNER: builder-B3 (jimmy)
#include "BullyAnimInstance.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimationPoseData.h"
#include "Animation/AttributesRuntime.h"
#include "AnimationRuntime.h"
#include "BonePose.h"
#include "BullyCharacter.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Misc/PackageName.h"
#include "ReferenceSkeleton.h"

DEFINE_LOG_CATEGORY_STATIC(LogBullyAnim, Log, All);

namespace
{
	float Frac(float X) { return X - FMath::FloorToFloat(X); }

	void AdvanceLayer(FBullyAnimLayer& L, float Dt, float Rate = 1.f)
	{
		if (!L.Seq) { return; }
		const float Len = L.Seq->GetPlayLength();
		L.Time += Dt * Rate;
		if (L.bLoop && Len > KINDA_SMALL_NUMBER) { L.Time = FMath::Fmod(L.Time, Len); if (L.Time < 0.f) { L.Time += Len; } }
		else { L.Time = FMath::Clamp(L.Time, 0.f, Len); }
	}
}

// ---------------------------------------------------------------------------------------------------------------
// proxy

void FBullyAnimInstanceProxy::PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds)
{
	FAnimInstanceProxy::PreUpdate(InAnimInstance, DeltaSeconds);
}

bool FBullyAnimInstanceProxy::Evaluate(FPoseContext& Output)
{
	TArray<FCompactPose, TInlineAllocator<8>> Poses;
	TArray<FBlendedCurve, TInlineAllocator<8>> Curves;
	TArray<UE::Anim::FStackAttributeContainer, TInlineAllocator<8>> Attrs;
	TArray<float, TInlineAllocator<8>> Weights;

	float Total = 0.f;
	for (const FBullyAnimLayer& L : Layers)
	{
		if (L.Seq && L.Weight > ZERO_ANIMWEIGHT_THRESH) { Total += L.Weight; }
	}
	if (Total <= ZERO_ANIMWEIGHT_THRESH)
	{
		Output.ResetToRefPose();
		return true;
	}

	FCompactPoseBoneIndex RootIdx(INDEX_NONE);
	if (RootMeshBoneIndex != INDEX_NONE)
	{
		RootIdx = Output.Pose.GetBoneContainer().MakeCompactPoseIndex(FMeshPoseBoneIndex(RootMeshBoneIndex));
	}

	for (const FBullyAnimLayer& L : Layers)
	{
		if (!L.Seq || L.Weight <= ZERO_ANIMWEIGHT_THRESH) { continue; }
		FPoseContext PoseContext(Output);
		FAnimationPoseData PoseData(PoseContext);
		L.Seq->GetAnimationPose(PoseData, FAnimExtractContext(double(L.Time), false, {}, L.bLoop));
		if (L.bClampRootRise && RootIdx != INDEX_NONE && PoseContext.Pose.IsValidIndex(RootIdx))
		{
			FTransform& T = PoseContext.Pose[RootIdx];
			FVector P = T.GetTranslation();
			P[RootUpAxis] = RootMaxUp >= 0.f ? FMath::Min(P[RootUpAxis], RootMaxUp) : FMath::Max(P[RootUpAxis], RootMaxUp);
			T.SetTranslation(P);
		}
		Poses.AddDefaulted_GetRef().MoveBonesFrom(PoseContext.Pose);
		Curves.AddDefaulted_GetRef().MoveFrom(PoseContext.Curve);
		Attrs.AddDefaulted_GetRef().MoveFrom(PoseContext.CustomAttributes);
		Weights.Add(L.Weight / Total);
	}

	FAnimationPoseData OutData(Output);
	if (Poses.Num() == 1)
	{
		Output.Pose.CopyBonesFrom(Poses[0]);
		Output.Curve.CopyFrom(Curves[0]);
		Output.CustomAttributes.CopyFrom(Attrs[0]);
	}
	else
	{
		FAnimationRuntime::BlendPosesTogether(Poses, Curves, Attrs, Weights, OutData);
		Output.Pose.NormalizeRotations();
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------
// instance

const TCHAR* UBullyAnimInstance::GetStateName() const
{
	switch (State)
	{
	case EBullyAnimState::Loco: return TEXT("loco");
	case EBullyAnimState::Jump: return TEXT("jump");
	case EBullyAnimState::Fall: return TEXT("fall");
	case EBullyAnimState::Land: return TEXT("land");
	}
	return TEXT("?");
}

FString UBullyAnimInstance::DescribeLayers() const
{
	FString S;
	for (const FBullyAnimLayer& L : Final)
	{
		if (L.Seq && L.Weight > 0.01f)
		{
			S += FString::Printf(TEXT("%s%s@%.2f*%.2f"), S.IsEmpty() ? TEXT("") : TEXT(" "), *L.Seq->GetName(), L.Time, L.Weight);
		}
	}
	return S;
}

UAnimSequence* UBullyAnimInstance::Clip(const TCHAR* Name)
{
	const FString Path = FString::Printf(TEXT("%s/%s.%s"), *ClipRoot, Name, Name);
	UAnimSequence* S = LoadObject<UAnimSequence>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!S)
	{
		UE_LOG(LogBullyAnim, Warning, TEXT("BR_ANIM clip missing: %s"), *Path);
	}
	else
	{
		Clips.Add(FName(Name), S);
	}
	return S;
}

void UBullyAnimInstance::LoadClips()
{
	Idle = Clip(TEXT("IDLE"));
	Walk = Clip(TEXT("PLAYERWALK"));
	Run = Clip(TEXT("PLAYERRUN"));
	Sprint = Clip(TEXT("RUNFAST"));
	IdleJump = Clip(TEXT("IDLEJUMP"));
	JogJumpL = Clip(TEXT("JOGJUMP_L"));
	JogJumpR = Clip(TEXT("JOGJUMP_R"));
	Fall = Clip(TEXT("FALL"));
	FallLand = Clip(TEXT("FALL_LAND"));
	LandHurt = Clip(TEXT("PLAY_LAND_HURT"));
	bClipsOk = Idle && Walk && Run && Sprint && IdleJump && JogJumpL && JogJumpR && Fall && FallLand;
	UE_LOG(LogBullyAnim, Display, TEXT("BR_ANIM clips %s from %s (%d loaded); lengths idle %.3f walk %.3f run %.3f sprint %.3f idlejump %.3f jogjump %.3f fall %.3f land %.3f"),
		bClipsOk ? TEXT("OK") : TEXT("INCOMPLETE"), *ClipRoot, Clips.Num(), ClipLength(Idle), ClipLength(Walk), ClipLength(Run), ClipLength(Sprint),
		ClipLength(IdleJump), ClipLength(JogJumpL), ClipLength(Fall), ClipLength(FallLand));
}

float UBullyAnimInstance::ClipLength(const UAnimSequence* S) const
{
	return S ? S->GetPlayLength() : 0.f;
}

void UBullyAnimInstance::NativeInitializeAnimation()
{
	Super::NativeInitializeAnimation();
	if (Clips.Num() == 0)
	{
		// Milestone 4: every imported ped has its own skeleton and clips next to its mesh
		// (/Game/Imported/Characters/<Name>/SK_<Name> -> .../<Name>/Anims). Jimmy resolves to the default ClipRoot.
		if (const USkeletalMeshComponent* SMC0 = GetSkelMeshComponent())
		{
			if (const USkeletalMesh* SK0 = SMC0->GetSkeletalMeshAsset())
			{
				const FString Pkg = SK0->GetOutermost()->GetName();
				if (Pkg.StartsWith(TEXT("/Game/Imported/Characters/")))
				{
					ClipRoot = FPackageName::GetLongPackagePath(Pkg) + TEXT("/Anims");
				}
			}
		}
		LoadClips();
	}
	// Root (pelvis carrier) reference translation: its largest component is the up axis in bone space.
	if (const USkeletalMeshComponent* SMC = GetSkelMeshComponent())
	{
		if (const USkeletalMesh* SK = SMC->GetSkeletalMeshAsset())
		{
			const FReferenceSkeleton& Ref = SK->GetRefSkeleton();
			RootMeshBoneIndex = Ref.FindBoneIndex(TEXT("Root"));
			if (RootMeshBoneIndex != INDEX_NONE)
			{
				const FVector T = Ref.GetRefBonePose()[RootMeshBoneIndex].GetTranslation();
				RootUpAxis = FMath::Abs(T.X) > FMath::Abs(T.Y) ? (FMath::Abs(T.X) > FMath::Abs(T.Z) ? 0 : 2) : (FMath::Abs(T.Y) > FMath::Abs(T.Z) ? 1 : 2);
				const float Up = T[RootUpAxis];
				RootMaxUp = Up + FMath::Sign(Up) * 2.f;
				UE_LOG(LogBullyAnim, Display, TEXT("BR_ANIM skeleton %s: %d bones; Root ref translation %s -> up axis %d, standing-jump pelvis cap %.1f"),
					*SK->GetName(), Ref.GetNum(), *T.ToString(), RootUpAxis, RootMaxUp);
			}
		}
	}
	State = EBullyAnimState::Loco;
	BlendAlpha = 1.f;
}

void UBullyAnimInstance::EnterState(EBullyAnimState NewState, float BlendTime)
{
	// Outgoing = what was on screen last frame (keeps playing while it fades). Keep the six heaviest layers.
	Outgoing.Reset();
	for (const FBullyAnimLayer& L : Current)
	{
		if (L.Seq && L.Weight > 0.01f) { Outgoing.Add(L); }
	}
	Outgoing.Sort([](const FBullyAnimLayer& A, const FBullyAnimLayer& B) { return A.Weight > B.Weight; });
	if (Outgoing.Num() > 6) { Outgoing.SetNum(6); }
	float W = 0.f;
	for (const FBullyAnimLayer& L : Outgoing) { W += L.Weight; }
	for (FBullyAnimLayer& L : Outgoing) { L.Weight = W > 0.f ? L.Weight / W : 0.f; }
	BlendDuration = BlendTime;
	BlendAlpha = (BlendTime > 0.f && Outgoing.Num() > 0) ? 0.f : 1.f;
	State = NewState;
	StateTime = 0.f;
}

void UBullyAnimInstance::BuildLoco(float Dt, float Speed, TArray<FBullyAnimLayer, TInlineAllocator<8>>& Out)
{
	const float S = FMath::Max(0.f, Speed);
	const float WIdle = FMath::Clamp(1.f - (S - 5.f) / FMath::Max(1.f, IdleBlendSpeed - 5.f), 0.f, 1.f);
	const float Lw = ClipLength(Walk), Lr = ClipLength(Run), Ls = ClipLength(Sprint);
	const float StrideW = WalkFootSpeed * Lw, StrideR = RunFootSpeed * Lr, StrideS = SprintFootSpeed * Ls;

	UAnimSequence* A; UAnimSequence* B; float OffA, OffB, StrideA, StrideB, Alpha;
	if (S <= RunBlendSpeed)
	{
		A = Walk; B = Run; OffA = WalkPhaseOffset; OffB = RunPhaseOffset; StrideA = StrideW; StrideB = StrideR;
		Alpha = FMath::Clamp((S - WalkBlendSpeed) / FMath::Max(1.f, RunBlendSpeed - WalkBlendSpeed), 0.f, 1.f);
	}
	else
	{
		A = Run; B = Sprint; OffA = RunPhaseOffset; OffB = SprintPhaseOffset; StrideA = StrideR; StrideB = StrideS;
		Alpha = FMath::Clamp((S - RunBlendSpeed) / FMath::Max(1.f, SprintBlendSpeed - RunBlendSpeed), 0.f, 1.f);
	}
	const float Stride = FMath::Lerp(StrideA, StrideB, Alpha);
	float CycleRate = Stride > 1.f ? S / Stride : 0.f;
	CycleRate = FMath::Max(CycleRate, MinWalkPlayRate / FMath::Max(0.1f, Lw));
	Phase = Frac(Phase + CycleRate * Dt);

	auto Add = [&Out](UAnimSequence* Seq, float Time, float Weight, bool bLoop)
	{
		if (Seq && Weight > 0.001f)
		{
			FBullyAnimLayer L; L.Seq = Seq; L.Time = Time; L.Weight = Weight; L.bLoop = bLoop;
			Out.Add(L);
		}
	};
	Add(A, Frac(Phase + OffA) * ClipLength(A), (1.f - WIdle) * (1.f - Alpha), true);
	Add(B, Frac(Phase + OffB) * ClipLength(B), (1.f - WIdle) * Alpha, true);
	const float Li = ClipLength(Idle);
	Add(Idle, Li > 0.f ? FMath::Fmod(IdleTime, Li) : 0.f, WIdle, true);
}

void UBullyAnimInstance::NativeUpdateAnimation(float Dt)
{
	Super::NativeUpdateAnimation(Dt);
	ABullyCharacter* C = Cast<ABullyCharacter>(TryGetPawnOwner());
	if (!C || !bClipsOk)
	{
		Final.Reset();
		GetProxyOnGameThread<FBullyAnimInstanceProxy>().Layers.Reset();
		return;
	}
	const UCharacterMovementComponent* M = C->GetCharacterMovement();
	const float Speed = C->GetHorizontalSpeed();
	const bool bFalling = M->IsFalling();
	const float Z = C->GetActorLocation().Z;
	const float Vz = C->GetVelocity().Z;
	const float G = FMath::Max(1.f, FMath::Abs(M->GetGravityZ()));
	StateTime += Dt;
	IdleTime += Dt;
	AirTime = bFalling ? AirTime + Dt : 0.f;

	// ---- events from the character
	if (C->JumpCount != SeenJumpCount)
	{
		SeenJumpCount = C->JumpCount;
		JumpStartZ = C->LastJumpZ;
		const bool bMoving = Speed > MovingJumpSpeed;
		if (bMoving)
		{
			// phase 0 = left mid-stance; in [0.5, 1) the left foot is the forward / next planted one -> left take-off
			ActionClip = Frac(Phase) >= 0.5f ? JogJumpL : JogJumpR;
			ActionTime = JogJumpStart; ActionTakeoff = JogJumpTakeoff; ActionAirEnd = JogJumpAirEnd; bActionClampRoot = false;
		}
		else
		{
			ActionClip = IdleJump;
			ActionTime = IdleJumpStart; ActionTakeoff = IdleJumpTakeoff; ActionAirEnd = IdleJumpAirEnd; bActionClampRoot = true;
		}
		EnterState(EBullyAnimState::Jump, bMoving ? 0.08f : 0.1f);
	}
	const bool bLandedNow = C->LandCount != SeenLandCount;
	SeenLandCount = C->LandCount;

	auto StartLanding = [&]()
	{
		const bool bHurt = LandHurt && C->LastLandDrop > C->FallDamageHeight;
		if (bHurt)
		{
			ActionClip = LandHurt; ActionTime = 0.f; bActionClampRoot = false; bLandMoving = false;
			EnterState(EBullyAnimState::Land, LandBlendTime);
		}
		else if (State == EBullyAnimState::Jump && (ActionClip == IdleJump || Speed > MovingJumpSpeed))
		{
			// the jump clip carries its own landing: continue it (no cross-fade, same clip)
			ActionTime = FMath::Max(ActionTime, ActionAirEnd);
			bLandMoving = ActionClip != IdleJump;
			State = EBullyAnimState::Land;
			StateTime = 0.f;
		}
		else if (Speed > MovingJumpSpeed)
		{
			ActionClip = JogJumpL; ActionTime = JogJumpAirEnd; bActionClampRoot = false; bLandMoving = true;
			EnterState(EBullyAnimState::Land, LandBlendTime);
		}
		else
		{
			ActionClip = FallLand; ActionTime = 0.f; bActionClampRoot = false; bLandMoving = false;
			EnterState(EBullyAnimState::Land, LandBlendTime);
		}
	};

	// ---- state update
	switch (State)
	{
	case EBullyAnimState::Loco:
		if (bFalling && (AirTime > LedgeFallDelay || Vz < -FMath::Sqrt(2.f * G * FallDropThreshold)))
		{
			ActionTime = 0.f;
			EnterState(EBullyAnimState::Fall, FallBlendTime);
		}
		break;
	case EBullyAnimState::Jump:
		if (!bFalling && (bLandedNow || StateTime > 0.1f))
		{
			StartLanding();
			break;
		}
		if (ActionTime < ActionTakeoff)
		{
			ActionTime = FMath::Min(ActionTime + Dt, ActionTakeoff);
		}
		else
		{
			// time-scale the air part so that it ends on the predicted landing (flat ground at take-off height)
			const float H = Z - JumpStartZ;
			const float Disc = Vz * Vz + 2.f * G * FMath::Max(0.f, H);
			const float Remaining = FMath::Max(0.05f, (Vz + FMath::Sqrt(Disc)) / G);
			const float Rate = FMath::Clamp((ActionAirEnd - ActionTime) / Remaining, 0.f, 3.f);
			ActionTime = FMath::Min(ActionTime + Rate * Dt, ActionAirEnd);
		}
		if (bFalling && Vz < 0.f && Z < JumpStartZ - FallDropThreshold)
		{
			ActionTime = 0.f;
			EnterState(EBullyAnimState::Fall, FallBlendTime);
		}
		break;
	case EBullyAnimState::Fall:
		ActionTime += Dt;
		if (!bFalling)
		{
			StartLanding();
		}
		break;
	case EBullyAnimState::Land:
	{
		ActionTime += Dt;
		const float Len = ClipLength(ActionClip);
		if (bLandMoving)
		{
			if (StateTime >= LandBlendTime)
			{
				// moving landing: fade into locomotion across the rest of the landing segment, feet in step
				Phase = ActionClip == JogJumpL ? 0.88f : 0.38f;
				EnterState(EBullyAnimState::Loco, FMath::Clamp(Len - ActionTime, 0.08f, 0.3f));
			}
		}
		else if (ActionTime >= Len || (ActionClip != LandHurt && Speed > MovingJumpSpeed && StateTime > LandInterruptTime))
		{
			EnterState(EBullyAnimState::Loco, StateBlendTime);
		}
		if (bFalling && AirTime > LedgeFallDelay)
		{
			ActionTime = 0.f;
			EnterState(EBullyAnimState::Fall, FallBlendTime);
		}
		break;
	}
	}
	if (bLandedNow && State == EBullyAnimState::Loco && C->LastLandDrop > C->FallDamageHeight && LandHurt)
	{
		StartLanding();
	}

	// ---- this state's layers
	TArray<FBullyAnimLayer, TInlineAllocator<8>> In;
	switch (State)
	{
	case EBullyAnimState::Loco:
		BuildLoco(Dt, Speed, In);
		break;
	case EBullyAnimState::Fall:
	{
		FBullyAnimLayer L; L.Seq = Fall; L.Weight = 1.f; L.bLoop = true;
		const float Len = ClipLength(Fall);
		L.Time = Len > 0.f ? FMath::Fmod(ActionTime, Len) : 0.f;
		In.Add(L);
		break;
	}
	default:
	{
		FBullyAnimLayer L; L.Seq = ActionClip; L.Weight = 1.f; L.bLoop = false; L.bClampRootRise = bActionClampRoot;
		L.Time = FMath::Clamp(ActionTime, 0.f, ClipLength(ActionClip));
		In.Add(L);
		break;
	}
	}

	// ---- cross-fade with the outgoing layers
	Final.Reset();
	if (BlendAlpha < 1.f)
	{
		BlendAlpha = BlendDuration > 0.f ? FMath::Min(1.f, BlendAlpha + Dt / BlendDuration) : 1.f;
		for (FBullyAnimLayer& L : Outgoing)
		{
			AdvanceLayer(L, Dt);
		}
	}
	for (FBullyAnimLayer L : In)
	{
		L.Weight *= BlendAlpha;
		Final.Add(L);
	}
	if (BlendAlpha < 1.f)
	{
		for (FBullyAnimLayer L : Outgoing)
		{
			L.Weight *= (1.f - BlendAlpha);
			Final.Add(L);
		}
	}
	Current = Final;

	FBullyAnimInstanceProxy& P = GetProxyOnGameThread<FBullyAnimInstanceProxy>();
	P.Layers = Final;
	P.RootMeshBoneIndex = RootMeshBoneIndex;
	P.RootUpAxis = RootUpAxis;
	P.RootMaxUp = RootMaxUp;
}
