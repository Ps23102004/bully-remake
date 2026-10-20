// OWNER: lane-C (engine) - physical body response
#include "BullyRagdollComponent.h"

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "BullyCharacter.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"

DEFINE_LOG_CATEGORY_STATIC(LogBullyRagdoll, Log, All);

UBullyRagdollComponent::UBullyRagdollComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;   // only while a ragdoll runs
	PrimaryComponentTick.TickGroup = TG_PostPhysics;      // read the pelvis after Chaos stepped
}

ABullyCharacter* UBullyRagdollComponent::GetChar() const { return Cast<ABullyCharacter>(GetOwner()); }
USkeletalMeshComponent* UBullyRagdollComponent::GetMeshComp() const { ABullyCharacter* C = GetChar(); return C ? C->GetMesh() : nullptr; }

namespace
{
	FString Norm(const FName& N) { return N.ToString().Replace(TEXT(" "), TEXT("-")).ToLower(); }
}

bool UBullyRagdollComponent::ResolveBones()
{
	USkeletalMeshComponent* Mesh = GetMeshComp();
	USkeletalMesh* SK = Mesh ? Mesh->GetSkeletalMeshAsset() : nullptr;
	if (!SK) { return false; }
	const FReferenceSkeleton& RS = SK->GetRefSkeleton();
	BonePelvis = BoneHead = BoneLThigh = BoneRThigh = INDEX_NONE;
	for (int32 i = 0; i < RS.GetNum(); ++i)
	{
		const FString N = Norm(RS.GetBoneName(i));
		if (BonePelvis == INDEX_NONE && N.Contains(TEXT("pelvis"))) { BonePelvis = i; }
		else if (BoneHead == INDEX_NONE && (N == TEXT("head") || N.EndsWith(TEXT("-head")))) { BoneHead = i; }
		else if (BoneLThigh == INDEX_NONE && N.EndsWith(TEXT("l-thigh"))) { BoneLThigh = i; }
		else if (BoneRThigh == INDEX_NONE && N.EndsWith(TEXT("r-thigh"))) { BoneRThigh = i; }
	}
	PelvisName = BonePelvis != INDEX_NONE ? RS.GetBoneName(BonePelvis) : NAME_None;
	// the pelvis must carry a physics body, otherwise ride on the first body of the asset
	if (const UPhysicsAsset* PA = Mesh->GetPhysicsAsset())
	{
		if (PA->SkeletalBodySetups.Num() > 0 && (PelvisName.IsNone() || PA->FindBodyIndex(PelvisName) == INDEX_NONE))
		{
			PelvisName = PA->SkeletalBodySetups[0]->BoneName;
			BonePelvis = RS.FindBoneIndex(PelvisName);
		}
	}
	bBonesResolved = BonePelvis != INDEX_NONE;
	return bBonesResolved;
}

bool UBullyRagdollComponent::CanRagdoll() const
{
	USkeletalMeshComponent* Mesh = GetMeshComp();
	if (!Mesh || !Mesh->GetSkeletalMeshAsset() || !Mesh->IsVisible()) { return false; }
	const UPhysicsAsset* PA = Mesh->GetPhysicsAsset();
	if (!PA || PA->SkeletalBodySetups.Num() == 0) { return false; }
	if (!bBonesResolved) { return const_cast<UBullyRagdollComponent*>(this)->ResolveBones(); }
	return true;
}

int32 UBullyRagdollComponent::GetBodyCount() const
{
	USkeletalMeshComponent* Mesh = GetMeshComp();
	const UPhysicsAsset* PA = Mesh ? Mesh->GetPhysicsAsset() : nullptr;
	return PA ? PA->SkeletalBodySetups.Num() : 0;
}

int32 UBullyRagdollComponent::GetConstraintCount() const
{
	USkeletalMeshComponent* Mesh = GetMeshComp();
	const UPhysicsAsset* PA = Mesh ? Mesh->GetPhysicsAsset() : nullptr;
	return PA ? PA->ConstraintSetup.Num() : 0;
}

bool UBullyRagdollComponent::WantsRagdoll(bool bKnockdown, bool bKO, bool bStagger) const
{
	if (!bEnabled) { return false; }
	const ABullyCharacter* C = GetChar();
	if (C && C->IsPlayerControlled() && !bPlayerRagdoll) { return false; }
	return (bKnockdown && bOnKnockdown) || (bKO && bOnKO) || (bStagger && bOnStagger);
}

float UBullyRagdollComponent::GetActiveSeconds() const
{
	return bActive && GetWorld() ? float(GetWorld()->GetTimeSeconds() - StartTime) : 0.f;
}

FVector UBullyRagdollComponent::GetPelvisLocation() const
{
	USkeletalMeshComponent* Mesh = GetMeshComp();
	if (Mesh && !PelvisName.IsNone()) { return Mesh->GetSocketLocation(PelvisName); }
	return GetOwner() ? GetOwner()->GetActorLocation() : FVector::ZeroVector;
}

// Front direction of the body from the pelvis/thigh/head joints. The skeleton's left/right naming is not trusted: the
// sign is calibrated once per ragdoll against the standing actor (CalibrateFront).
static FVector BodyFront(USkeletalMeshComponent* M, const FReferenceSkeleton& RS,
	int32 Pelvis, int32 Head, int32 LT, int32 RT, FVector* OutUp = nullptr)
{
	const FVector P = M->GetSocketLocation(RS.GetBoneName(Pelvis));
	const FVector H = M->GetSocketLocation(RS.GetBoneName(Head));
	const FVector L = M->GetSocketLocation(RS.GetBoneName(LT));
	const FVector R = M->GetSocketLocation(RS.GetBoneName(RT));
	if (OutUp) { *OutUp = (H - P).GetSafeNormal(); }
	return FVector::CrossProduct(R - L, H - P).GetSafeNormal();
}

void UBullyRagdollComponent::CalibrateFront()
{
	USkeletalMeshComponent* Mesh = GetMeshComp();
	USkeletalMesh* SK = Mesh ? Mesh->GetSkeletalMeshAsset() : nullptr;
	if (!SK || BoneHead == INDEX_NONE || BoneLThigh == INDEX_NONE || BoneRThigh == INDEX_NONE) { return; }
	FVector Up;
	const FVector F = BodyFront(Mesh, SK->GetRefSkeleton(), BonePelvis, BoneHead, BoneLThigh, BoneRThigh, &Up);
	// only trust it when the body is upright (spine near vertical) and the front is horizontal
	const FVector Fwd = GetOwner()->GetActorForwardVector().GetSafeNormal2D();
	const float D = FVector::DotProduct(F.GetSafeNormal2D(), Fwd);
	if (Up.Z > 0.7f && FMath::Abs(F.Z) < 0.5f && FMath::Abs(D) > 0.5f)
	{
		FrontSign = D >= 0.f ? 1.f : -1.f;
	}
}

bool UBullyRagdollComponent::Start(const FVector& LaunchVelocity, FName InReason)
{
	if (!bEnabled || !CanRagdoll()) { return false; }
	if (bActive)
	{
		AddLaunch(LaunchVelocity);
		return true;
	}
	ABullyCharacter* C = GetChar();
	USkeletalMeshComponent* Mesh = GetMeshComp();
	UWorld* W = GetWorld();
	if (!C || !Mesh || !W) { return false; }
	CalibrateFront();

	UCharacterMovementComponent* Move = C->GetCharacterMovement();
	UCapsuleComponent* Cap = C->GetCapsuleComponent();
	const FVector PreVel = Move ? Move->Velocity : FVector::ZeroVector;

	SavedRelLoc = Mesh->GetRelativeLocation();
	SavedRelRot = Mesh->GetRelativeRotation();
	SavedProfile = Mesh->GetCollisionProfileName();
	SavedMeshCollision = Mesh->GetCollisionEnabled();
	SavedAnimTick = uint8(Mesh->VisibilityBasedAnimTickOption);
	SavedCapsuleCollision = Cap->GetCollisionEnabled();

	Cap->SetCollisionEnabled(ECollisionEnabled::NoCollision);   // the capsule just tracks the pelvis now
	if (Move)
	{
		Move->StopMovementImmediately();
		Move->DisableMovement();
	}
	Mesh->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
	Mesh->SetCollisionProfileName(TEXT("Ragdoll"));
	Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Mesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);        // walk over a body, never get stuck on one
	Mesh->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	Mesh->bBlendPhysics = true;
	Mesh->SetAllBodiesSimulatePhysics(true);
	Mesh->SetSimulatePhysics(true);
	Mesh->SetAllBodiesPhysicsBlendWeight(1.f);
	Mesh->SetAllUseCCD(true);
	for (FBodyInstance* B : Mesh->Bodies)
	{
		if (B) { B->LinearDamping = LinearDamping; B->AngularDamping = AngularDamping; B->UpdateDampingProperties(); }
	}
	Mesh->WakeAllRigidBodies();

	FVector V = PreVel + LaunchVelocity;
	if (V.Size() > MaxLaunchSpeed) { V = V.GetSafeNormal() * MaxLaunchSpeed; }
	Mesh->SetAllPhysicsLinearVelocity(V);
	// tumble: spin about the axis perpendicular to the launch so the body pitches over instead of sliding upright
	const FVector Flat = FVector(LaunchVelocity.X, LaunchVelocity.Y, 0.f);
	if (Flat.Size() > 1.f)
	{
		Mesh->SetAllPhysicsAngularVelocityInDegrees(FVector::CrossProduct(FVector::UpVector, Flat.GetSafeNormal()) * FMath::Min(Flat.Size() * 0.5f, 360.f));
	}

	bActive = true;
	bSettled = false;
	SettledSince = -1.0;
	StartTime = W->GetTimeSeconds();
	StartLocation = LastSnapLoc = C->GetActorLocation();
	PeakSpeed = V.Size();
	Reason = InReason;
	++StartCount;
	SetComponentTickEnabled(true);
	if (bLog)
	{
		UE_LOG(LogBullyRagdoll, Display, TEXT("BR_RAGDOLL start %s reason=%s launch=%.0f cm/s (%s) bodies=%d constraints=%d pelvis=%s front_sign=%+.0f t=%.2f"),
			*C->GetName(), *InReason.ToString(), V.Size(), *V.ToCompactString(), GetBodyCount(), GetConstraintCount(), *PelvisName.ToString(), FrontSign, StartTime);
	}
	return true;
}

void UBullyRagdollComponent::AddLaunch(const FVector& Velocity)
{
	USkeletalMeshComponent* Mesh = GetMeshComp();
	if (!bActive || !Mesh) { return; }
	Mesh->WakeAllRigidBodies();
	Mesh->SetAllPhysicsLinearVelocity(Velocity, /*bAddToCurrent*/ true);
	bSettled = false;
	SettledSince = -1.0;
	PeakSpeed = FMath::Max(PeakSpeed, Velocity.Size());
	if (bLog) { UE_LOG(LogBullyRagdoll, Display, TEXT("BR_RAGDOLL shove %s +%.0f cm/s"), *GetNameSafe(GetOwner()), Velocity.Size()); }
}

void UBullyRagdollComponent::TickComponent(float Dt, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(Dt, TickType, ThisTickFunction);
	if (!bActive) { SetComponentTickEnabled(false); return; }
	ABullyCharacter* C = GetChar();
	USkeletalMeshComponent* Mesh = GetMeshComp();
	UWorld* W = GetWorld();
	if (!C || !Mesh || !W) { return; }
	const double Now = W->GetTimeSeconds();
	// Possessing a ragdolled pawn (a student hit into a ragdoll whose brawl AI takes over) restarts it, and ACharacter::Restart puts the movement
	// component back into Falling: the capsule would fall away from the body every frame. Keep it off while the body is simulated.
	if (UCharacterMovementComponent* Move = C->GetCharacterMovement())
	{
		if (Move->MovementMode != MOVE_None) { Move->DisableMovement(); }
	}
	if (FVector::Dist(C->GetActorLocation(), LastSnapLoc) > ExternalMoveTolerance)
	{
		// safety net for every teleport that did not call Abort() first: keep the new position, just stand the body down there
		UE_LOG(LogBullyRagdoll, Display, TEXT("BR_RAGDOLL %s: capsule was moved %.0f cm by someone else (teleport): ragdoll dropped at the new position"),
			*C->GetName(), float(FVector::Dist(C->GetActorLocation(), LastSnapLoc)));
		Restore(false);
		return;
	}
	const FVector P = GetPelvisLocation();
	if (P.ContainsNaN() || P.Z < StartLocation.Z - FallThroughDepth)
	{
		UE_LOG(LogBullyRagdoll, Warning, TEXT("BR_RAGDOLL %s lost (%s): recovering at the start spot"), *C->GetName(), P.ContainsNaN() ? TEXT("NaN") : TEXT("fell through the world"));
		C->SetActorLocation(StartLocation, false, nullptr, ETeleportType::TeleportPhysics);
		Restore(true);
		return;
	}
	C->SetActorLocation(P, false, nullptr, ETeleportType::TeleportPhysics);   // capsule rides the pelvis
	LastSnapLoc = C->GetActorLocation();

	const FVector Vel = Mesh->GetPhysicsLinearVelocity(PelvisName);
	const float Speed = Vel.Size();
	PeakSpeed = FMath::Max(PeakSpeed, Speed);
	if (Speed > 4000.f)   // a solver blow-up must never launch a student across the map
	{
		Mesh->SetAllPhysicsLinearVelocity(FVector::ZeroVector);
		UE_LOG(LogBullyRagdoll, Warning, TEXT("BR_RAGDOLL %s velocity spike %.0f cm/s clamped"), *C->GetName(), Speed);
	}
	const float Ang = Mesh->GetPhysicsAngularVelocityInDegrees(PelvisName).Size();
	const double Age = Now - StartTime;
	if (Age >= MinSeconds && Speed < SettleLinear && Ang < SettleAngularDeg)
	{
		if (SettledSince < 0.0) { SettledSince = Now; }
	}
	else
	{
		SettledSince = -1.0;
	}
	if (!bSettled && ((SettledSince >= 0.0 && Now - SettledSince >= SettleHold) || Age >= MaxSeconds))
	{
		bSettled = true;
		if (bLog) { UE_LOG(LogBullyRagdoll, Display, TEXT("BR_RAGDOLL %s settled after %.2fs (peak %.0f cm/s) pelvis=%s"), *C->GetName(), Age, PeakSpeed, *P.ToCompactString()); }
	}
}

bool UBullyRagdollComponent::ClipBoneCS(UAnimSequence* Clip, int32 MeshBone, FTransform& Out) const
{
	USkeletalMeshComponent* Mesh = GetMeshComp();
	USkeletalMesh* SK = Mesh ? Mesh->GetSkeletalMeshAsset() : nullptr;
	USkeleton* Skel = Clip ? Clip->GetSkeleton() : nullptr;
	if (!SK || !Skel || MeshBone == INDEX_NONE) { return false; }
	const FReferenceSkeleton& MRS = SK->GetRefSkeleton();
	const FReferenceSkeleton& SRS = Skel->GetReferenceSkeleton();
	// chain from the root down to the bone, by name (mesh and clip skeleton share names, not necessarily indices)
	TArray<int32> Chain;
	for (int32 B = MeshBone; B != INDEX_NONE; B = MRS.GetParentIndex(B)) { Chain.Insert(B, 0); }
	FTransform CS = FTransform::Identity;
	for (int32 B : Chain)
	{
		const int32 SI = SRS.FindBoneIndex(MRS.GetBoneName(B));
		if (SI == INDEX_NONE) { return false; }
		FTransform Local;
		Clip->GetBoneTransform(Local, FSkeletonPoseBoneIndex(SI), FAnimExtractContext(0.0, false), false);
		CS = Local * CS;
	}
	Out = CS;
	return true;
}

bool UBullyRagdollComponent::MeasureBody(FVector& OutHeadDir, bool& bOutBelly) const
{
	USkeletalMeshComponent* Mesh = GetMeshComp();
	USkeletalMesh* SK = Mesh ? Mesh->GetSkeletalMeshAsset() : nullptr;
	if (!SK || BoneHead == INDEX_NONE) { return false; }
	const FReferenceSkeleton& RS = SK->GetRefSkeleton();
	const FVector P = Mesh->GetSocketLocation(RS.GetBoneName(BonePelvis));
	const FVector H = Mesh->GetSocketLocation(RS.GetBoneName(BoneHead));
	OutHeadDir = FVector(H.X - P.X, H.Y - P.Y, 0.f);
	bOutBelly = false;
	if (BoneLThigh != INDEX_NONE && BoneRThigh != INDEX_NONE)
	{
		const FVector F = BodyFront(Mesh, RS, BonePelvis, BoneHead, BoneLThigh, BoneRThigh) * FrontSign;
		bOutBelly = F.Z < 0.f;
	}
	if (OutHeadDir.SizeSquared() < 25.f * 25.f) { return false; }   // sitting up: no lying direction, keep the actor's yaw
	OutHeadDir.Normalize();
	return true;
}

void UBullyRagdollComponent::Restore(bool bPlaceOnFloor)
{
	ABullyCharacter* C = GetChar();
	USkeletalMeshComponent* Mesh = GetMeshComp();
	if (!C || !Mesh) { bActive = false; SetComponentTickEnabled(false); return; }
	Mesh->SetAllBodiesSimulatePhysics(false);
	Mesh->SetSimulatePhysics(false);
	Mesh->SetAllBodiesPhysicsBlendWeight(0.f);
	Mesh->bBlendPhysics = false;
	Mesh->SetCollisionProfileName(SavedProfile);
	Mesh->SetCollisionEnabled(SavedMeshCollision);
	Mesh->AttachToComponent(C->GetCapsuleComponent(), FAttachmentTransformRules::SnapToTargetNotIncludingScale);
	Mesh->SetRelativeLocationAndRotation(SavedRelLoc, SavedRelRot);
	C->GetCapsuleComponent()->SetCollisionEnabled(SavedCapsuleCollision);
	if (UCharacterMovementComponent* Move = C->GetCharacterMovement())
	{
		Move->Velocity = FVector::ZeroVector;
		Move->SetMovementMode(MOVE_Falling);   // lands on the next tick
	}
	bActive = false;
	bSettled = false;
	SetComponentTickEnabled(false);
	(void)bPlaceOnFloor;
}

UAnimSequence* UBullyRagdollComponent::Recover(UAnimSequence* BackClip, UAnimSequence* BellyClip, bool* bOutBelly)
{
	if (!bActive) { return nullptr; }
	ABullyCharacter* C = GetChar();
	USkeletalMeshComponent* Mesh = GetMeshComp();
	if (!C || !Mesh) { Restore(false); return nullptr; }

	FVector HeadDir = FVector::ForwardVector;
	bool bBelly = false;
	const bool bHaveDir = MeasureBody(HeadDir, bBelly);
	UAnimSequence* Clip = (bBelly && BellyClip) ? BellyClip : BackClip;

	// where the get-up clip's first (lying) frame has the pelvis and which way its head points, in the actor's frame
	float ClipHeadYaw = 0.f;
	FVector PelvisInActor = FVector::ZeroVector;
	FTransform P0, H0;
	if (Clip && ClipBoneCS(Clip, BonePelvis, P0) && ClipBoneCS(Clip, BoneHead, H0))
	{
		const FVector HA = SavedRelRot.RotateVector(H0.GetLocation() - P0.GetLocation());
		ClipHeadYaw = FMath::RadiansToDegrees(FMath::Atan2(HA.Y, HA.X));
		PelvisInActor = SavedRelRot.RotateVector(P0.GetLocation()) + SavedRelLoc;
	}
	const FVector Pelvis = GetPelvisLocation();
	float ActorYaw = C->GetActorRotation().Yaw;
	if (bHaveDir) { ActorYaw = FMath::RadiansToDegrees(FMath::Atan2(HeadDir.Y, HeadDir.X)) - ClipHeadYaw; }
	const FRotator Rot(0.f, ActorYaw, 0.f);
	FVector Loc = Pelvis - Rot.RotateVector(FVector(PelvisInActor.X, PelvisInActor.Y, 0.f));
	// floor under the body (the capsule centre sits half a capsule above it)
	FHitResult Hit;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(BullyRagdollFloor), /*bTraceComplex*/ true, C);
	float FloorZ = Pelvis.Z - C->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	if (GetWorld()->LineTraceSingleByChannel(Hit, FVector(Pelvis.X, Pelvis.Y, Pelvis.Z + 80.f), FVector(Pelvis.X, Pelvis.Y, Pelvis.Z - 400.f), ECC_Visibility, Q))
	{
		FloorZ = Hit.ImpactPoint.Z;
	}
	Loc.Z = FloorZ + C->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.f;
	C->SetActorLocationAndRotation(Loc, Rot, false, nullptr, ETeleportType::TeleportPhysics);
	if (bLog)
	{
		UE_LOG(LogBullyRagdoll, Display, TEXT("BR_RAGDOLL recover %s after %.2fs: %s, head yaw world %.0f - clip %.0f = actor yaw %.0f, capsule %s clip %s"),
			*C->GetName(), GetActiveSeconds(), bBelly ? TEXT("belly-down") : TEXT("belly-up"),
			bHaveDir ? FMath::RadiansToDegrees(FMath::Atan2(HeadDir.Y, HeadDir.X)) : 0.f, ClipHeadYaw, ActorYaw, *Loc.ToCompactString(), *GetNameSafe(Clip));
	}
	Restore(true);
	bLastBelly = bBelly;
	if (bOutBelly) { *bOutBelly = bBelly; }
	return Clip;
}

void UBullyRagdollComponent::Abort()
{
	if (!bActive) { return; }
	ABullyCharacter* C = GetChar();
	if (C)
	{
		// stand where the body lies (floor-snapped); no get-up animation
		FHitResult Hit;
		const FVector P = GetPelvisLocation();
		FCollisionQueryParams Q(SCENE_QUERY_STAT(BullyRagdollAbort), true, C);
		const float HH = C->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		FVector Loc(P.X, P.Y, P.Z);
		if (GetWorld()->LineTraceSingleByChannel(Hit, P + FVector(0, 0, 80.f), P - FVector(0, 0, 400.f), ECC_Visibility, Q)) { Loc.Z = Hit.ImpactPoint.Z + HH + 2.f; }
		C->SetActorLocation(Loc, false, nullptr, ETeleportType::TeleportPhysics);
	}
	Restore(true);
}
