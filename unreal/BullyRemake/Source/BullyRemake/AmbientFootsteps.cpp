// OWNER: builder-B4 (ambient)
#include "AmbientFootsteps.h"

#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundBase.h"

DEFINE_LOG_CATEGORY_STATIC(LogBullyAudio, Log, All);

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
	if (!C || !W || Sounds.Num() == 0 || DeltaTime <= 0.f)
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
	int32 Idx = FMath::RandRange(0, Sounds.Num() - 1);
	if (Sounds.Num() > 1 && Idx == LastIndex)
	{
		Idx = (Idx + 1 + FMath::RandRange(0, Sounds.Num() - 2)) % Sounds.Num();
	}
	LastIndex = Idx;
	USoundBase* S = Sounds[Idx];
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
	const float Pitch = 1.f + FMath::FRandRange(-PitchVariation, PitchVariation);
	const float Vol = Volume * FMath::FRandRange(0.85f, 1.f);
	UGameplayStatics::PlaySoundAtLocation(W, S, Where, FRotator::ZeroRotator, Vol, Pitch, 0.f, Attenuation);
	++StepCount;
	UE_LOG(LogBullyAudio, Display, TEXT("BR_AUDIO step #%d %s foot=%s wave=%s vol=%.2f pitch=%.3f dist=%.0f t=%.2f"),
		StepCount, *Tag, Foot == 0 ? TEXT("L") : TEXT("R"), *S->GetName(), Vol, Pitch, Dist, W->GetTimeSeconds());
}
