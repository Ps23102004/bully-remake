// OWNER: slice-combat
#include "BullyCombatAnimInstance.h"

#include "Animation/AnimSequence.h"
#include "BullyCharacter.h"
#include "GameFramework/Character.h"

void UBullyCombatAnimInstance::NativeInitializeAnimation()
{
	Super::NativeInitializeAnimation();
	Weight = 0.f;
	Cur = FBullyAnimLayer();
	Prev = FBullyAnimLayer();
}

UAnimSequence* UBullyCombatAnimInstance::Strafe(const TCHAR* Name)
{
	const FName Key(Name);
	if (TObjectPtr<UAnimSequence>* F = StrafeClips.Find(Key)) { return F->Get(); }
	UBullyCombatComponent* CC = nullptr;
	if (const APawn* P = TryGetPawnOwner()) { CC = P->FindComponentByClass<UBullyCombatComponent>(); }
	const FString Root = CC ? CC->ClipRoot : TEXT("/Game/Imported/Characters/Jimmy/Anims");
	UAnimSequence* S = LoadObject<UAnimSequence>(nullptr, *FString::Printf(TEXT("%s/%s.%s"), *Root, Name, Name), nullptr, LOAD_NoWarn | LOAD_Quiet);
	StrafeClips.Add(Key, S);
	return S;
}

void UBullyCombatAnimInstance::NativeUpdateAnimation(float Dt)
{
	Super::NativeUpdateAnimation(Dt);   // locomotion layers -> proxy
	const APawn* Owner = TryGetPawnOwner();
	UBullyCombatComponent* CC = Owner ? Owner->FindComponentByClass<UBullyCombatComponent>() : nullptr;
	if (!CC)
	{
		return;
	}
	FBullyAnimInstanceProxy& Proxy = GetProxyOnGameThread<FBullyAnimInstanceProxy>();

	// locked-on strafing: replace the locomotion cycle with a 4-way strafe blend in the body's frame
	if (bStrafeWhenLocked && CC->IsLockedOn() && CC->GetState() == EBullyCombatState::Idle)
	{
		const FVector V = Owner->GetVelocity();
		const float Speed = V.Size2D();
		if (Speed > StrafeMinSpeed)
		{
			const FVector Local = Owner->GetActorRotation().UnrotateVector(FVector(V.X, V.Y, 0.f)) / Speed;
			UAnimSequence* Clips[4] = { Strafe(TEXT("STRAFE_FORWARD")), Strafe(TEXT("STRAFE_BACK")), Strafe(TEXT("STRAFE_RIGHT")), Strafe(TEXT("STRAFE_LEFT")) };
			const float W4[4] = { FMath::Max(0.f, Local.X), FMath::Max(0.f, -Local.X), FMath::Max(0.f, Local.Y), FMath::Max(0.f, -Local.Y) };
			StrafeTime += Dt;
			const float Blend = FMath::Clamp((Speed - StrafeMinSpeed) / 80.f, 0.f, 1.f);
			float Sum = 0.f;
			for (int32 i = 0; i < 4; ++i) { if (Clips[i]) { Sum += W4[i]; } }
			if (Sum > KINDA_SMALL_NUMBER)
			{
				for (FBullyAnimLayer& L : Proxy.Layers) { L.Weight *= (1.f - Blend); }
				for (int32 i = 0; i < 4; ++i)
				{
					if (!Clips[i] || W4[i] <= 0.f) { continue; }
					FBullyAnimLayer L; L.Seq = Clips[i]; L.bLoop = true; L.Weight = Blend * W4[i] / Sum;
					L.Time = FMath::Fmod(StrafeTime, FMath::Max(0.01f, Clips[i]->GetPlayLength()));
					Proxy.Layers.Add(L);
				}
			}
		}
	}

	// combat clip request
	FBullyCombatPose Pose;
	const bool bWant = CC->GetPose(Pose);
	if (bWant)
	{
		if (Pose.Serial != Serial)
		{
			if (Cur.Seq && Weight > 0.01f)
			{
				Prev = Cur;
				PrevAlpha = 1.f;
			}
			Serial = Pose.Serial;
		}
		Cur.Seq = Pose.Seq;
		Cur.bLoop = Pose.bLoop;
		const float Len = Pose.Seq ? Pose.Seq->GetPlayLength() : 0.f;
		Cur.Time = Pose.bLoop && Len > 0.f ? FMath::Fmod(Pose.Time, Len) : FMath::Clamp(Pose.Time, 0.f, Len);
		Weight = CombatBlendIn > 0.f ? FMath::Min(1.f, Weight + Dt / CombatBlendIn) : 1.f;
	}
	else
	{
		Weight = CombatBlendOut > 0.f ? FMath::Max(0.f, Weight - Dt / CombatBlendOut) : 0.f;   // Cur holds its last frame while fading
	}
	if (PrevAlpha > 0.f)
	{
		PrevAlpha = ClipCrossFade > 0.f ? FMath::Max(0.f, PrevAlpha - Dt / ClipCrossFade) : 0.f;
	}
	if (Weight <= 0.f || !Cur.Seq)
	{
		return;
	}
	for (FBullyAnimLayer& L : Proxy.Layers) { L.Weight *= (1.f - Weight); }
	FBullyAnimLayer A = Cur;
	A.Weight = Weight * (1.f - PrevAlpha);
	Proxy.Layers.Add(A);
	if (PrevAlpha > 0.f && Prev.Seq)
	{
		FBullyAnimLayer B = Prev;
		B.Weight = Weight * PrevAlpha;
		Proxy.Layers.Add(B);
	}
}
