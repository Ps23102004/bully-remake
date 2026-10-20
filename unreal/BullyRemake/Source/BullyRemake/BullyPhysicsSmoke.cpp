// OWNER: lane-C (engine)
#include "BullyPhysicsSmoke.h"

#include "AmbientSubsystem.h"
#include "AmbientWalker.h"
#include "BullyCharacter.h"
#include "BullyCombatAI.h"
#include "BullyCombatComponent.h"
#include "BullyDoorLink.h"
#include "BullyRagdollComponent.h"
#include "BullyScriptedNPC.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "MissionTrigger.h"
#include "PhysicsEngine/PhysicsAsset.h"

DEFINE_LOG_CATEGORY_STATIC(LogBullyPhys, Log, All);

bool UBullyPhysicsSmoke::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* W = Cast<UWorld>(Outer);
	return W && W->IsGameWorld() && FString(FCommandLine::Get()).Contains(TEXT("-BRPhysicsSmoke"));
}

TStatId UBullyPhysicsSmoke::GetStatId() const { RETURN_QUICK_DECLARE_CYCLE_STAT(UBullyPhysicsSmoke, STATGROUP_Tickables); }

void UBullyPhysicsSmoke::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	if (!FParse::Value(FCommandLine::Get(), TEXT("-BRPhysicsSmoke="), OutDir)) { OutDir = FPaths::ProjectSavedDir() / TEXT("BullyPhysics"); }
	IFileManager::Get().MakeDirectory(*OutDir, true);
	bActive = true;
	StepStart = InWorld.GetTimeSeconds();
	Note(FString::Printf(TEXT("smoke armed -> %s"), *OutDir));
}

void UBullyPhysicsSmoke::Note(const FString& S)
{
	UE_LOG(LogBullyPhys, Display, TEXT("BR_PHYS %s"), *S);
	Lines.Add(FString::Printf(TEXT("t=%.2f %s"), Elapsed, *S));
}
void UBullyPhysicsSmoke::Pass(const FString& Name, const FString& Detail) { ++Passes; Note(FString::Printf(TEXT("PASS %s: %s"), *Name, *Detail)); }
void UBullyPhysicsSmoke::Fail(const FString& Name, const FString& Detail) { ++Failures; Note(FString::Printf(TEXT("FAIL %s: %s"), *Name, *Detail)); }

void UBullyPhysicsSmoke::Next(EStep S)
{
	Step = S;
	StepStart = GetWorld()->GetTimeSeconds();
	Phase = 0;
}

float UBullyPhysicsSmoke::FloorZ(const FVector& At, float Above) const
{
	FHitResult Hit;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(BullyPhysFloor), true);
	if (GetWorld()->LineTraceSingleByChannel(Hit, At + FVector(0, 0, Above), At - FVector(0, 0, 800.f), ECC_Visibility, Q)) { return Hit.ImpactPoint.Z; }
	return -1e9f;
}

AAmbientWalker* UBullyPhysicsSmoke::SpawnTestWalker(const FVector& At, const FString& Ped)
{
	UWorld* W = GetWorld();
	AAmbientWalker* K = W->SpawnActorDeferred<AAmbientWalker>(AAmbientWalker::StaticClass(), FTransform(FRotator::ZeroRotator, At + FVector(0, 0, 100.f)), nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);
	if (!K) { return nullptr; }
	K->MeshPath = FString::Printf(TEXT("/Game/Imported/Characters/%s/SK_%s.SK_%s"), *Ped, *Ped, *Ped);
	K->StudentId = Ped;
	K->LoopId = TEXT("physics_smoke");
	K->bChatHold = true;   // stand still (the conversation hold), turn to the player
	K->FinishSpawning(K->GetActorTransform());
	return K;
}

void UBullyPhysicsSmoke::Place(ABullyCharacter* P, const AActor* Target, float Dist)
{
	const FVector T = Target->GetActorLocation();
	const FVector Dir = FVector(1, 0, 0);
	const FVector At = T - Dir * Dist;
	P->SetActorLocation(At, false, nullptr, ETeleportType::TeleportPhysics);
	P->SetActorRotation(FRotator(0.f, 0.f, 0.f));
	P->GetCharacterMovement()->StopMovementImmediately();
}

void UBullyPhysicsSmoke::Tick(float Dt)
{
	Super::Tick(Dt);
	UWorld* W = GetWorld();
	if (!bActive || bDone || !W) { return; }
	const double Now = W->GetTimeSeconds();
	Elapsed = Now;
	const double T = Now - StepStart;
	ABullyCharacter* P = Player.Get();
	UBullyCombatComponent* PC = P ? P->GetCombat() : nullptr;
	auto CombatOf = [](AAmbientWalker* K) { return K ? K->GetCombat() : nullptr; };
	auto RagOf = [](AAmbientWalker* K) { return K ? K->GetRagdoll() : nullptr; };

	if (!bBystanderChecked && HitAt > 0.0 && Now - HitAt >= 1.0) { CheckBystanders(); }

	switch (Step)
	{
	case EStep::Warmup:
	{
		if (T < 4.0) { break; }
		APlayerController* Ctl = W->GetFirstPlayerController();
		P = Ctl ? Cast<ABullyCharacter>(Ctl->GetPawn()) : nullptr;
		if (!P) { if (T > 20.0) { Fail(TEXT("setup"), TEXT("no player pawn")); Next(EStep::Finish); } break; }
		Player = P;
		Next(EStep::Setup);
		break;
	}
	case EStep::Setup:
	{
		P->GetCombat()->bInvulnerable = true;   // takes reactions, no damage: the fights below must not end the run
		// a REAL ambient walker (spawned by UAmbientSubsystem from Config/AmbientLoops.json) is target #1
		UAmbientSubsystem* Amb = W->GetSubsystem<UAmbientSubsystem>();
		AAmbientWalker* Real = nullptr;
		int32 NWalk = 0;
		if (Amb)
		{
			for (const TWeakObjectPtr<AAmbientWalker>& K : Amb->GetWalkers()) { if (K.IsValid()) { ++NWalk; if (!Real) { Real = K.Get(); } } }
		}
		bW0Real = Real != nullptr;
		Note(FString::Printf(TEXT("ambient walkers alive: %d (%s)"), NWalk, bW0Real ? TEXT("using a real one as target 1") : TEXT("ambient subsystem off: spawning a stand-in")));
		const FVector Base = Real ? Real->GetActorLocation() : P->GetActorLocation() + FVector(600, 0, 0);
		AAmbientWalker* K0 = Real ? Real : SpawnTestWalker(Base, TEXT("GN_Boy01"));
		if (Real) { Real->bChatHold = true; }
		W0 = K0;
		W1 = SpawnTestWalker(Base + FVector(0, 700, 0), TEXT("GN_Greekboy"));
		W2 = SpawnTestWalker(Base + FVector(0, -700, 0), TEXT("GN_Boy02"));
		if (!K0 || !W1.IsValid() || !W2.IsValid()) { Fail(TEXT("setup"), TEXT("could not get 3 target students")); Next(EStep::Finish); break; }
		Note(FString::Printf(TEXT("targets: %s (%s, team %s, hp %.0f) at %s"), *K0->GetName(), *K0->StudentId,
			K0->GetCombat()->Team == EBullyCombatTeam::Enemy ? TEXT("Enemy") : TEXT("Player"), K0->GetCombat()->Health, *K0->GetActorLocation().ToString()));
		Check(K0->GetCombat()->Team == EBullyCombatTeam::Enemy, TEXT("student_is_valid_target"), TEXT("ambient walker is on the Enemy team (FindTarget/TickHits consider it)"));
		// bystanders: only BJoin / BFlee may react to the first hit on W0 (everybody else in range is told to ignore it, so the later steps stay deterministic)
		auto Quiet = [](AAmbientWalker* K) { if (K) { K->BystanderRules.Reset(); K->DefaultBystanderReaction = EAmbientReact::Ignore; } };
		Quiet(W1.Get()); Quiet(W2.Get());
		if (Amb) { for (const TWeakObjectPtr<AAmbientWalker>& K : Amb->GetWalkers()) { if (K.IsValid()) { Quiet(K.Get()); } } }
		auto Bystander = [&](const FVector& Off, const TCHAR* Mesh, const TCHAR* Id) -> AAmbientWalker*
		{
			AAmbientWalker* K = SpawnTestWalker(Base + Off, Mesh);
			if (K) { K->StudentId = Id; }
			return K;
		};
		BJoin = Bystander(FVector(1000.f, 450.f, 0.f), TEXT("GN_Boy02"), TEXT("boy01"));              // rule "boy" = join
		BFlee = Bystander(FVector(300.f, -500.f, 0.f), TEXT("NDH3_Bucky"), TEXT("nerd_bucky"));        // rule "nerd" = flee
		BFar = Bystander(FVector(2600.f, 0.f, 0.f), TEXT("GN_Boy01"), TEXT("boy02"));                   // 26 m away: out of range
		Check(BJoin.IsValid() && BFlee.IsValid() && BFar.IsValid(), TEXT("bystanders_spawned"), TEXT("joiner (boy01), runner (nerd_bucky) and an out-of-range boy (boy02) are standing by"));
		if (BJoin.IsValid()) { Check(BJoin->GetBystanderReaction() == EAmbientReact::Join, TEXT("rule_boy_joins"), TEXT("Config rule: student id 'boy01' joins")); }
		if (BFlee.IsValid()) { Check(BFlee->GetBystanderReaction() == EAmbientReact::Flee, TEXT("rule_nerd_flees"), TEXT("Config rule: student id 'nerd_bucky' flees")); }
		Next(EStep::Assets);
		break;
	}
	case EStep::Assets:
	{
		static const TCHAR* Peds[] = { TEXT("Jimmy"), TEXT("GN_Asiangirl"), TEXT("GN_Boy01"), TEXT("GN_Boy02"), TEXT("GN_Bully01"), TEXT("GN_Bully02"), TEXT("GN_Bully03"),
			TEXT("GN_Bully04"), TEXT("GN_Bully05"), TEXT("GN_Greekboy"), TEXT("NDH3_Bucky"), TEXT("PFH1_Seth") };
		int32 Good = 0;
		for (const TCHAR* Ped : Peds)
		{
			UPhysicsAsset* PA = LoadObject<UPhysicsAsset>(nullptr, *FString::Printf(TEXT("/Game/Imported/Characters/%s/PHYS_%s.PHYS_%s"), Ped, Ped, Ped), nullptr, LOAD_NoWarn | LOAD_Quiet);
			const int32 B = PA ? PA->SkeletalBodySetups.Num() : 0;
			const int32 C = PA ? PA->ConstraintSetup.Num() : 0;
			Note(FString::Printf(TEXT("PHYS_%s: %s bodies=%d constraints=%d"), Ped, PA ? TEXT("loaded") : TEXT("MISSING"), B, C));
			if (B >= 8 && C >= B - 2) { ++Good; }
		}
		Check(Good == UE_ARRAY_COUNT(Peds), TEXT("physics_assets_usable"), FString::Printf(TEXT("%d/%d human peds have a PhysicsAsset with >=8 bodies and a connected constraint tree"), Good, int32(UE_ARRAY_COUNT(Peds))));
		for (AAmbientWalker* K : { W0.Get(), W1.Get(), W2.Get() })
		{
			if (K) { Note(FString::Printf(TEXT("%s mesh=%s physics_asset=%s can_ragdoll=%d bodies=%d constraints=%d"), *K->GetName(),
				*GetNameSafe(K->GetMesh()->GetSkeletalMeshAsset()), *GetNameSafe(K->GetMesh()->GetPhysicsAsset()), K->GetRagdoll() && K->GetRagdoll()->CanRagdoll(),
				K->GetRagdoll() ? K->GetRagdoll()->GetBodyCount() : 0, K->GetRagdoll() ? K->GetRagdoll()->GetConstraintCount() : 0)); }
		}
		Check(RagOf(W0.Get()) && RagOf(W0.Get())->CanRagdoll(), TEXT("target_can_ragdoll"), TEXT("target student's component reports CanRagdoll"));
		Check(P->GetRagdoll() && P->GetRagdoll()->CanRagdoll(), TEXT("player_can_ragdoll"), TEXT("Jimmy's component reports CanRagdoll"));
		if (IConsoleVariable* CV = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Collision.OneSidedTriangleMesh")))
		{
			Check(CV->GetInt() == 0, TEXT("chaos_trimesh_two_sided"), FString::Printf(TEXT("p.Chaos.Collision.OneSidedTriangleMesh = %d (0: simulated bodies hit walls from both sides)"), CV->GetInt()));
		}
		else { Fail(TEXT("chaos_trimesh_two_sided"), TEXT("p.Chaos.Collision.OneSidedTriangleMesh does not exist in this engine")); }
		Place(P, W0.Get(), 140.f);
		HpBefore = CombatOf(W0.Get())->Health;
		PosBeforeHit = W0->GetActorLocation();
		Next(EStep::Punch);
		break;
	}
	case EStep::Punch:
	{
		AAmbientWalker* K = W0.Get();
		UBullyCombatComponent* KC = CombatOf(K);
		if (!K || !KC) { Fail(TEXT("punch"), TEXT("target vanished")); Next(EStep::Finish); break; }
		UBullyRagdollComponent* RD = RagOf(K);
		// keep the player on the target's west side facing east, 140 cm away, until the target starts reacting
		if (!bHitSeen && PC->GetState() == EBullyCombatState::Idle && FVector::Dist2D(P->GetActorLocation(), K->GetActorLocation()) > 170.f) { Place(P, K, 140.f); }
		if (Now - LastPress > 0.22) { LastPress = Now; PC->RequestAttack(); }   // what IA_Attack does when no weapon is equipped
		if (!bHitSeen && KC->Health < HpBefore - 0.01f)
		{
			bHitSeen = true;
			HitAt = Now;
			if (BFlee.IsValid()) { FleeD0 = float(FVector::Dist2D(BFlee->GetActorLocation(), P->GetActorLocation())); }
			Pass(TEXT("punch_connects_with_student"), FString::Printf(TEXT("ambient student hp %.0f -> %.0f after %.2fs of punching (state %s)"), HpBefore, KC->Health, T, UBullyCombatComponent::StateName(KC->GetState())));
			Check(Cast<ABullyCombatAIController>(K->GetController()) != nullptr, TEXT("student_fights_back"), TEXT("AI controller possessed the student after its first health loss (brawl)"));
		}
		if (bHitSeen && HitAt > 0.0 && Now - HitAt < 0.7) { HitPush = FMath::Max(HitPush, FVector::Dist2D(K->GetActorLocation(), PosBeforeHit)); }
		if (RD && RD->IsActive())
		{
			Pass(TEXT("finisher_throws_body"), FString::Printf(TEXT("3-hit chain finisher put the student into a ragdoll after %.2fs"), T));
			Check(HitPush >= 15.f, TEXT("hit_pushes_capsule"), FString::Printf(TEXT("student slid %.0f cm in the 0.7s after the first hit"), HitPush));
			RagStart = RD->GetPelvisLocation();
			RagStartedAt = Now;
			bRagSeen = true;
			Next(EStep::RagdollWatch);
			break;
		}
		if (T > 14.0)
		{
			if (!bHitSeen) { Fail(TEXT("punch_connects_with_student"), FString::Printf(TEXT("no health loss after %.0fs of punching; state %s, dist %.0f cm"), T, UBullyCombatComponent::StateName(KC->GetState()), FVector::Dist2D(P->GetActorLocation(), K->GetActorLocation()))); }
			Fail(TEXT("finisher_throws_body"), FString::Printf(TEXT("student never ragdolled (hp %.0f, state %s); forcing a knockdown to test the rest"), KC->Health, UBullyCombatComponent::StateName(KC->GetState())));
			KC->ApplyWeaponHit(EBullyHitReaction::Knockdown, 5.f);
			RagStart = RD ? RD->GetPelvisLocation() : K->GetActorLocation();
			RagStartedAt = Now;
			Next(EStep::RagdollWatch);
		}
		break;
	}
	case EStep::RagdollWatch:
	{
		AAmbientWalker* K = W0.Get();
		UBullyRagdollComponent* RD = RagOf(K);
		if (!K || !RD) { Fail(TEXT("ragdoll"), TEXT("target vanished")); Next(EStep::Finish); break; }
		if (RD->IsActive())
		{
			const FVector Pel = RD->GetPelvisLocation();
			RagMaxDist = FMath::Max(RagMaxDist, FVector::Dist2D(Pel, RagStart));
			RagMaxZ = FMath::Max(RagMaxZ, Pel.Z - RagStart.Z);
			MaxFollowErr = FMath::Max(MaxFollowErr, float(FVector::Dist(Pel, K->GetActorLocation())));
			if (Pel.ContainsNaN()) { Fail(TEXT("ragdoll_finite"), TEXT("pelvis NaN")); }
		}
		if (RD->IsSettled() && Phase == 0)
		{
			Phase = 1;
			const FVector Pel = RD->GetPelvisLocation();
			const FVector Head = K->GetMesh()->GetSocketLocation(TEXT("Root-Head"));
			const float Fz = FloorZ(Pel);
			Note(FString::Printf(TEXT("W0 ragdoll settled after %.2fs: pelvis %s head %s floor z %.1f peak speed %.0f cm/s"), RD->GetActiveSeconds(), *Pel.ToCompactString(), *Head.ToCompactString(), Fz, RD->GetPeakSpeed()));
			Check(RagMaxDist >= 50.f || RD->GetPeakSpeed() >= 250.f, TEXT("ragdoll_launch"), FString::Printf(TEXT("pelvis travelled %.0f cm horizontally, rose %.0f cm, peak %.0f cm/s"), RagMaxDist, RagMaxZ, RD->GetPeakSpeed()));
			Check(Fz > -1e8f && Pel.Z - Fz > -5.f && Pel.Z - Fz < 80.f && Head.Z - Fz > -10.f, TEXT("ragdoll_on_floor"), FString::Printf(TEXT("pelvis %.0f cm / head %.0f cm above the floor (no fall-through)"), Pel.Z - Fz, Head.Z - Fz));
			Check(!K->GetMesh()->GetAttachParent(), TEXT("ragdoll_mesh_detached"), TEXT("mesh simulates detached from the capsule"));
			Check(MaxFollowErr < 12.f, TEXT("capsule_follows_pelvis"), FString::Printf(TEXT("max capsule-to-pelvis error %.1f cm"), MaxFollowErr));
			Check(RD->GetActiveSeconds() < RD->MaxSeconds - 0.1f, TEXT("ragdoll_settles"), FString::Printf(TEXT("settled on its own after %.2fs (cap %.0fs)"), RD->GetActiveSeconds(), RD->MaxSeconds));
			Next(EStep::Recovery);
			break;
		}
		if (T > 12.0) { Fail(TEXT("ragdoll_settles"), TEXT("ragdoll never settled in 12s")); Next(EStep::Recovery); }
		break;
	}
	case EStep::Recovery:
	{
		AAmbientWalker* K = W0.Get();
		UBullyRagdollComponent* RD = RagOf(K);
		UBullyCombatComponent* KC = CombatOf(K);
		if (!K || !RD || !KC) { Fail(TEXT("recovery"), TEXT("target vanished")); Next(EStep::KO); break; }
		if (!RD->IsActive() && KC->GetState() == EBullyCombatState::Idle)
		{
			UCapsuleComponent* Cap = K->GetCapsuleComponent();
			USkeletalMeshComponent* Mesh = K->GetMesh();
			const float Fz = FloorZ(K->GetActorLocation(), 60.f);
			const float Feet = K->GetActorLocation().Z - Cap->GetScaledCapsuleHalfHeight();
			Check(Mesh->GetAttachParent() == Cap, TEXT("recover_mesh_reattached"), TEXT("mesh is a child of the capsule again"));
			Check(Mesh->GetRelativeLocation().Equals(FVector(0.f, 0.f, -K->CapsuleHalfHeight), 1.f) && FMath::Abs(Mesh->GetRelativeRotation().Yaw - K->MeshYawOffset) < 1.f, TEXT("recover_mesh_transform"),
				FString::Printf(TEXT("relative %s yaw %.0f (want z %.0f yaw %.0f)"), *Mesh->GetRelativeLocation().ToCompactString(), Mesh->GetRelativeRotation().Yaw, -K->CapsuleHalfHeight, K->MeshYawOffset));
			Check(Cap->GetCollisionEnabled() == ECollisionEnabled::QueryAndPhysics && !Mesh->IsSimulatingPhysics() && Mesh->GetCollisionEnabled() == ECollisionEnabled::NoCollision, TEXT("recover_collision_restored"), TEXT("capsule collides again, mesh is anim-driven and collision-free"));
			Check(Fz > -1e8f && FMath::Abs(Feet - Fz) < 8.f, TEXT("recover_on_floor"), FString::Printf(TEXT("capsule feet %.1f cm vs floor %.1f"), Feet, Fz));
			Check(KC->Health > 0.f, TEXT("recover_alive"), FString::Printf(TEXT("student stands up with hp %.0f (state idle, ragdoll starts %d)"), KC->Health, RD->GetStartCount()));
			// done with W0: remove it (its brawl AI would chase the player in later steps) - also destroys a body cleanly
			if (AController* C = K->GetController()) { C->Destroy(); }
			K->Destroy();
			W0.Reset();
			// KO target
			Place(P, W1.Get(), 140.f);
			W1->bChatHold = true;
			CombatOf(W1.Get())->ApplyWeaponHit(EBullyHitReaction::KO, 9999.f);
			Next(EStep::KO);
			break;
		}
		if (T > 20.0) { Fail(TEXT("recovery"), FString::Printf(TEXT("did not get back up in 20s: state %s ragdoll_active %d"), UBullyCombatComponent::StateName(KC->GetState()), RD->IsActive())); Next(EStep::KO); }
		break;
	}
	case EStep::KO:
	{
		AAmbientWalker* K = W1.Get();
		UBullyRagdollComponent* RD = RagOf(K);
		UBullyCombatComponent* KC = CombatOf(K);
		if (!K || !RD || !KC) { Fail(TEXT("ko"), TEXT("target vanished")); Next(EStep::Throw); break; }
		if (Phase == 0 && T > 4.0)
		{
			Phase = 1;
			const float Err = float(FVector::Dist(RD->GetPelvisLocation(), K->GetActorLocation()));
			Check(KC->GetState() == EBullyCombatState::KO && RD->IsActive() && RD->IsSettled(), TEXT("ko_ragdoll_lies_down"), FString::Printf(TEXT("state %s ragdoll active=%d settled=%d after %.1fs"), UBullyCombatComponent::StateName(KC->GetState()), RD->IsActive(), RD->IsSettled(), RD->GetActiveSeconds()));
			Check(Err < 12.f, TEXT("ko_capsule_follows"), FString::Printf(TEXT("capsule %.1f cm from pelvis"), Err));
			PelvisBefore = RD->GetPelvisLocation();
			RD->AddLaunch(FVector(520.f, 0.f, 140.f));   // what a kick on the downed body does
		}
		else if (Phase == 1 && T > 6.0)
		{
			Phase = 2;
			const float Moved = float(FVector::Dist(RD->GetPelvisLocation(), PelvisBefore));
			Check(Moved >= 40.f, TEXT("kick_shoves_body"), FString::Printf(TEXT("a 520 cm/s shove moved the lying body %.0f cm"), Moved));
		}
		else if (Phase == 2 && T > 11.0)
		{
			Check(RD->IsActive() && KC->GetState() == EBullyCombatState::KO, TEXT("ko_stays_down"), FString::Printf(TEXT("still down %.1fs after the KO (state %s)"), T, UBullyCombatComponent::StateName(KC->GetState())));
			K->Destroy();   // destroy while ragdolling: must not crash
			W1.Reset();
			Place(P, W2.Get(), 140.f);
			W2->bChatHold = true;
			PelvisBefore = W2->GetActorLocation();
			const FVector Throw = FVector(1000.f, 0.f, 260.f);   // a car's shove (BullyVehicle passes ~0.9 x its speed)
			CombatOf(W2.Get())->ApplyWeaponHit(EBullyHitReaction::Knockdown, 12.f, Throw);
			RagMaxDist = 0.f;
			RagStart = PelvisBefore;
			Next(EStep::Throw);
		}
		break;
	}
	case EStep::Throw:
	{
		AAmbientWalker* K = W2.Get();
		UBullyRagdollComponent* RD = RagOf(K);
		UBullyCombatComponent* KC = CombatOf(K);
		if (!K || !RD || !KC) { Fail(TEXT("throw"), TEXT("target vanished")); Next(EStep::PlayerRag); break; }
		if (RD->IsActive()) { RagMaxDist = FMath::Max(RagMaxDist, FVector::Dist2D(RD->GetPelvisLocation(), RagStart)); }
		if (!RD->IsActive() && RD->GetStartCount() > 0 && KC->GetState() == EBullyCombatState::Idle)
		{
			Check(RagMaxDist >= 150.f, TEXT("vehicle_style_launch"), FString::Printf(TEXT("a 1000 cm/s throw carried the student %.0f cm"), RagMaxDist));
			Pass(TEXT("vehicle_style_recovers"), FString::Printf(TEXT("student stands up again %.1fs after being thrown"), T));
			// stagger: with bOnStagger the ragdoll must end in a get-up when the hit-stun is over (it used to lie limp for good)
			RD->bOnStagger = true;
			PelvisBefore = K->GetActorLocation();
			KC->ApplyWeaponHit(EBullyHitReaction::Stagger, 4.f, FVector(500.f, 0.f, 100.f));
			Next(EStep::Stagger);
			break;
		}
		if (T > 25.0) { Fail(TEXT("vehicle_style_recovers"), FString::Printf(TEXT("still %s after 25s"), UBullyCombatComponent::StateName(KC->GetState()))); Next(EStep::PlayerRag); }
		break;
	}
	case EStep::Stagger:
	{
		AAmbientWalker* K = W2.Get();
		UBullyRagdollComponent* RD = RagOf(K);
		UBullyCombatComponent* KC = CombatOf(K);
		bool bGo = false;
		if (!K || !RD || !KC) { Fail(TEXT("stagger"), TEXT("target vanished")); bGo = true; }
		else if (Phase == 0 && RD->IsActive()) { Phase = 1; Pass(TEXT("stagger_ragdolls"), FString::Printf(TEXT("a stagger threw the body (ragdoll start #%d, state %s)"), RD->GetStartCount(), UBullyCombatComponent::StateName(KC->GetState()))); }
		else if (Phase == 1 && !RD->IsActive())
		{
			Pass(TEXT("stagger_recovers"), FString::Printf(TEXT("ragdoll ended %.1fs after the stagger (state %s, ragdoll starts %d)"), T, UBullyCombatComponent::StateName(KC->GetState()), RD->GetStartCount()));
			bGo = true;
		}
		else if (T > 20.0) { Fail(TEXT("stagger_recovers"), FString::Printf(TEXT("ragdoll %s after 20s (phase %d, state %s)"), RD->IsActive() ? TEXT("still active") : TEXT("never started"), Phase, UBullyCombatComponent::StateName(KC->GetState()))); bGo = true; }
		if (bGo)
		{
			if (K) { if (AController* C = K->GetController()) { C->Destroy(); } K->Destroy(); }
			W2.Reset();
			// beaten while ragdolled (health floor): the ragdoll must be dropped, not left limp under the pair clip
			W3 = SpawnTestWalker(P->GetActorLocation() + FVector(0, -700, 0), TEXT("GN_Boy02"));
			if (!W3.IsValid()) { Fail(TEXT("beaten_ragdoll"), TEXT("could not spawn the target")); }
			else
			{
				// mission NPCs keep player-sized health; plain students get the (lower) enemy health
				const UBullyCombatComponent* SC = CombatOf(W3.Get());
				Check(FMath::IsNearlyEqual(SC->MaxHealth, SC->EnemyMaxHealth), TEXT("student_has_enemy_health"), FString::Printf(TEXT("student max hp %.0f (enemy %.0f)"), SC->MaxHealth, SC->EnemyMaxHealth));
				ABullyScriptedNPC* SN = W->SpawnActorDeferred<ABullyScriptedNPC>(ABullyScriptedNPC::StaticClass(), FTransform(FRotator::ZeroRotator, P->GetActorLocation() + FVector(0, 700, 100)), nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);
				if (SN)
				{
					SN->MeshPath = W3->MeshPath;
					SN->FinishSpawning(SN->GetActorTransform());
					const UBullyCombatComponent* NC = SN->GetCombat();
					Check(NC && FMath::IsNearlyEqual(NC->MaxHealth, NC->PlayerMaxHealth), TEXT("mission_npc_keeps_health"), FString::Printf(TEXT("scripted NPC max hp %.0f (player-sized %.0f)"), NC ? NC->MaxHealth : -1.f, NC ? NC->PlayerMaxHealth : -1.f));
					SN->Destroy();
				}
				else { Fail(TEXT("mission_npc_keeps_health"), TEXT("could not spawn a scripted NPC")); }
			}
			Next(EStep::BeatenRag);
			break;
		}
		break;
	}
	case EStep::BeatenRag:
	{
		AAmbientWalker* K = W3.Get();
		UBullyRagdollComponent* RD = RagOf(K);
		UBullyCombatComponent* KC = CombatOf(K);
		bool bGo = false;
		if (!K || !RD || !KC) { Fail(TEXT("beaten_ragdoll"), TEXT("target vanished")); bGo = true; }
		else if (Phase == 0 && T > 0.8)
		{
			Place(P, K, 140.f);
			KC->MinHealth = 5.f;
			KC->ApplyWeaponHit(EBullyHitReaction::Knockdown, 5.f, FVector(300.f, 0.f, 100.f));
			Phase = 1;
		}
		else if (Phase == 1 && RD->IsActive() && KC->GetState() == EBullyCombatState::Downed)
		{
			KC->ApplyWeaponHit(EBullyHitReaction::GroundHit, 500.f);   // a kick on the downed body takes it to the health floor
			Phase = 2;
		}
		else if (Phase == 2)
		{
			Check(KC->GetState() == EBullyCombatState::Beaten && !RD->IsActive() && K->GetMesh()->GetAttachParent() == K->GetCapsuleComponent(), TEXT("beaten_drops_ragdoll"),
				FString::Printf(TEXT("state %s ragdoll_active=%d mesh_attached=%d"), UBullyCombatComponent::StateName(KC->GetState()), RD->IsActive(), K->GetMesh()->GetAttachParent() == K->GetCapsuleComponent()));
			bGo = true;
		}
		else if (T > 10.0) { Fail(TEXT("beaten_drops_ragdoll"), FString::Printf(TEXT("phase %d state %s ragdoll_active=%d"), Phase, UBullyCombatComponent::StateName(KC->GetState()), RD->IsActive())); bGo = true; }
		if (bGo)
		{
			if (K) { if (AController* C = K->GetController()) { C->Destroy(); } K->Destroy(); }
			W3.Reset();
			// player ragdoll
			PC->bInvulnerable = true;
			PelvisBefore = P->GetActorLocation();
			PC->ApplyWeaponHit(EBullyHitReaction::Knockdown, 0.f, FVector(-600.f, 0.f, 220.f));
			MaxFollowErr = 0.f;
			Next(EStep::PlayerRag);
		}
		break;
	}
	case EStep::PlayerRag:
	{
		UBullyRagdollComponent* RD = P->GetRagdoll();
		if (RD->IsActive())
		{
			MaxFollowErr = FMath::Max(MaxFollowErr, float(FVector::Dist(RD->GetPelvisLocation(), P->GetActorLocation())));
			if (Phase == 0) { Phase = 1; Pass(TEXT("player_ragdolls"), FString::Printf(TEXT("Jimmy went ragdoll (bodies %d)"), RD->GetBodyCount())); }
		}
		if (Phase == 1 && !RD->IsActive() && PC->GetState() == EBullyCombatState::Idle)
		{
			APlayerController* Ctl = Cast<APlayerController>(P->GetController());
			Check(MaxFollowErr < 12.f, TEXT("player_capsule_follows"), FString::Printf(TEXT("max capsule-to-pelvis %.1f cm (camera rides the capsule)"), MaxFollowErr));
			Check(Ctl && !Ctl->IsMoveInputIgnored() && P->GetMesh()->GetAttachParent() == P->GetCapsuleComponent(), TEXT("player_recovers_control"), TEXT("input not ignored, mesh re-attached, combat idle"));
			Phase = 2;
			// a KO: Jimmy must not stay on the floor for good
			PC->ApplyWeaponHit(EBullyHitReaction::KO, 0.f, FVector(-300.f, 0.f, 150.f));
			Next(EStep::PlayerKO);
			break;
		}
		if (T > 25.0) { Fail(TEXT("player_recovers_control"), FString::Printf(TEXT("player still %s after 25s (ragdoll active %d)"), UBullyCombatComponent::StateName(PC->GetState()), RD->IsActive())); Next(EStep::Finish); }
		break;
	}
	case EStep::PlayerKO:
	{
		UBullyRagdollComponent* RD = P->GetRagdoll();
		APlayerController* Ctl = Cast<APlayerController>(P->GetController());
		if (Phase == 0)
		{
			if (T < 1.2) { break; }
			Check(PC->GetState() == EBullyCombatState::KO && RD->IsActive(), TEXT("player_ko_lies_down"), FString::Printf(TEXT("state %s ragdoll active=%d hp %.0f"), UBullyCombatComponent::StateName(PC->GetState()), RD->IsActive(), PC->Health));
			Phase = 1;
			break;
		}
		if (PC->GetState() != EBullyCombatState::KO && !RD->IsActive())
		{
			Check(PC->Health >= PC->MaxHealth * PC->PlayerKOHealthFraction - 0.5f, TEXT("player_ko_gets_up"), FString::Printf(TEXT("Jimmy is up again %.1fs after the KO, state %s, hp %.0f/%.0f"), T, UBullyCombatComponent::StateName(PC->GetState()), PC->Health, PC->MaxHealth));
			Check(Ctl && !Ctl->IsMoveInputIgnored() && P->GetMesh()->GetAttachParent() == P->GetCapsuleComponent(), TEXT("player_ko_recovers_control"), TEXT("input not ignored, mesh re-attached"));
			Next(EStep::DoorShared);
			break;
		}
		if (T > 25.0) { Fail(TEXT("player_ko_gets_up"), FString::Printf(TEXT("still %s after 25s (ragdoll active %d)"), UBullyCombatComponent::StateName(PC->GetState()), RD->IsActive())); Next(EStep::DoorShared); }
		break;
	}
	case EStep::DoorShared:
	{
		APlayerController* Ctl = Cast<APlayerController>(P->GetController());
		IConsoleVariable* Lumen = IConsoleManager::Get().FindConsoleVariable(TEXT("r.LumenScene.FastCameraMode"));
		auto XY = [](const FVector& A, const FVector& B) { return float(FVector::Dist2D(A, B)); };
		if (Phase == 0)
		{
			if (T < 2.0 || PC->GetState() != EBullyCombatState::Idle) { if (T > 12.0) { Fail(TEXT("door_shared"), TEXT("player never went idle")); Next(EStep::Finish); } break; }
			const FVector Here = P->GetActorLocation();
			SExitA = Here + FVector(2500.f, 0.f, 0.f);
			SExitB = Here + FVector(2500.f, 900.f, 0.f);
			SExitI = Here + FVector(2500.f, -900.f, 0.f);
			auto MakeDoor = [&](const TCHAR* Id, const TCHAR* Partner, const FVector& Exit, const FVector& At) -> ABullyDoorLink*
			{
				ABullyDoorLink* D = W->SpawnActorDeferred<ABullyDoorLink>(ABullyDoorLink::StaticClass(), FTransform(At));
				if (!D) { return nullptr; }
				D->LinkId = Id; D->PartnerId = Partner; D->ExitLocation = Exit; D->ExitYaw = 0.f; D->PrewarmRadius = 0.f;
				D->FinishSpawning(FTransform(At));
				D->Trigger->SetCollisionEnabled(ECollisionEnabled::NoCollision);   // used by script only
				D->Trigger->SetBoxExtent(FVector(150.f, 150.f, 300.f));
				return D;
			};
			// two street doors lead to ONE interior door, which only declares the first of them (the grocery store / bike shop in door_links.json)
			SOutA = MakeDoor(TEXT("smoke_out_a"), TEXT("smoke_in"), SExitA, Here + FVector(0.f, -700.f, 0.f));
			SOutB = MakeDoor(TEXT("smoke_out_b"), TEXT("smoke_in"), SExitB, Here + FVector(0.f, 700.f, 0.f));   // its box overlaps the decoy trigger below
			SIn = MakeDoor(TEXT("smoke_in"), TEXT("smoke_out_a"), SExitI, Here + FVector(0.f, 1500.f, 0.f));
			auto MakeTrigger = [&](const TCHAR* Id, const FVector& At) -> AMissionTrigger*
			{
				AMissionTrigger* M = W->SpawnActorDeferred<AMissionTrigger>(AMissionTrigger::StaticClass(), FTransform(At));
				if (!M) { return nullptr; }
				M->TriggerId = Id;
				M->FinishSpawning(FTransform(At));
				M->Box->SetBoxExtent(FVector(100.f, 100.f, 300.f));
				return M;
			};
			STouch = MakeTrigger(TEXT("smoke_touched"), Here);                           // the traveller stands in it
			SDecoy = MakeTrigger(TEXT("smoke_decoy"), Here + FVector(0.f, 700.f, 0.f));  // overlaps door B's box but not the traveller
			if (!SOutA.IsValid() || !SOutB.IsValid() || !SIn.IsValid() || !STouch.IsValid() || !SDecoy.IsValid()) { Fail(TEXT("door_shared"), TEXT("could not spawn the test doors")); Next(EStep::TeleRag); break; }
			Check(SOutB->Trigger->Bounds.GetBox().Intersect(SDecoy->Box->Bounds.GetBox()), TEXT("decoy_overlaps_door_box"), TEXT("the decoy mission trigger intersects the door's box (the old loose AABB test would have fired it)"));
			if (Lumen) { LumenPrev = Lumen->GetInt(); }
			Ctl->SetIgnoreMoveInput(true);   // a chat / menu holds the player's input: a door must not drop that lock
			P->LastEntryDoor.Reset();
			P->LastDoorUseTime = -1000.0;
			DoorUsesBefore = P->DoorUseCount;
			Check(SOutB->UseDoor(P), TEXT("shared_door_use_accepted"), TEXT("street door B accepted"));
			Phase = 1;
			break;
		}
		if (Phase == 1 && P->DoorUseCount > DoorUsesBefore && !P->bDoorBusy)
		{
			Check(XY(P->GetActorLocation(), SExitI) < 150.f, TEXT("shared_door_enters_interior"), FString::Printf(TEXT("street door B put the player at the interior door's exit (%.0f cm off)"), XY(P->GetActorLocation(), SExitI)));
			Check(STouch->IsPlayerInside() && !SDecoy->IsPlayerInside(), TEXT("mission_trigger_only_if_touched"), FString::Printf(TEXT("trigger the player stood in: %s; decoy that only overlaps the door's box: %s"), STouch->IsPlayerInside() ? TEXT("entered") : TEXT("NOT entered"), SDecoy->IsPlayerInside() ? TEXT("entered (wrong)") : TEXT("not entered")));
			Check(Ctl->IsMoveInputIgnored(), TEXT("door_keeps_foreign_input_lock"), TEXT("the instant cut left the lock held by someone else alone"));
			Ctl->SetIgnoreMoveInput(false);
			Check(!Ctl->IsMoveInputIgnored(), TEXT("input_lock_balanced"), TEXT("one release by its owner frees the player (the door did not release a lock it never took)"));
			Check(P->LastEntryDoor.Get() == SOutB.Get(), TEXT("entry_door_remembered"), TEXT("LastEntryDoor = street door B"));
			if (Lumen) { Check(Lumen->GetInt() == 1, TEXT("lumen_boost_on"), TEXT("r.LumenScene.FastCameraMode raised for the new room")); }
			Phase = 2;
			StepStart = Now;
			break;
		}
		if (Phase == 2 && T > SIn->LumenBoostSeconds + 0.4)
		{
			if (Lumen) { Check(Lumen->GetInt() == LumenPrev, TEXT("lumen_boost_restores"), FString::Printf(TEXT("r.LumenScene.FastCameraMode back to %d after the boost"), Lumen->GetInt())); }
			P->LastDoorUseTime = -1000.0;
			DoorUsesBefore = P->DoorUseCount;
			Check(SIn->UseDoor(P), TEXT("shared_door_leave_accepted"), TEXT("interior door accepted"));
			Phase = 3;
			break;
		}
		if (Phase == 3 && P->DoorUseCount > DoorUsesBefore && !P->bDoorBusy)
		{
			Check(XY(P->GetActorLocation(), SExitB) < 150.f, TEXT("shared_door_returns_to_entry"), FString::Printf(TEXT("leaving the shared interior door returned to street door B (%.0f cm off; door A, the declared partner, is %.0f cm off)"), XY(P->GetActorLocation(), SExitB), XY(P->GetActorLocation(), SExitA)));
			if (Lumen)
			{
				Check(Lumen->GetInt() == 1, TEXT("lumen_boost_on_again"), TEXT("second cut boosts again"));
				SIn->Destroy();   // EndPlay with the boost pending: the CVar must not stay raised
				Check(Lumen->GetInt() == LumenPrev, TEXT("lumen_boost_restored_on_end_play"), FString::Printf(TEXT("door destroyed mid-boost, CVar back to %d"), Lumen->GetInt()));
			}
			for (TWeakObjectPtr<ABullyDoorLink> D : { SOutA, SOutB, SIn }) { if (D.IsValid()) { D->Destroy(); } }
			for (TWeakObjectPtr<AMissionTrigger> M : { STouch, SDecoy }) { if (M.IsValid()) { M->Destroy(); } }
			Next(EStep::TeleRag);
			break;
		}
		if (T > 10.0) { Fail(TEXT("door_shared"), FString::Printf(TEXT("stuck in phase %d"), Phase)); Ctl->SetIgnoreMoveInput(false); Next(EStep::TeleRag); }
		break;
	}
	case EStep::TeleRag:
	{
		if (Phase == 0)
		{
			// doors: the main door pair of the saved map
			for (TActorIterator<ABullyDoorLink> It(W); It; ++It)
			{
				if (It->LinkId == TEXT("main_door_ext")) { DoorExt = *It; }
				if (It->LinkId == TEXT("main_door_int")) { DoorInt = *It; }
			}
			if (!DoorExt.IsValid() || !DoorInt.IsValid()) { Note(TEXT("door links not in the level: door checks skipped")); Next(EStep::Finish); break; }
			PC->bInvulnerable = true;
			PC->ApplyWeaponHit(EBullyHitReaction::Knockdown, 0.f, FVector(-200.f, 0.f, 120.f));
			Phase = 1;
			break;
		}
		UBullyRagdollComponent* RD = P->GetRagdoll();
		if (Phase == 1 && T > 0.5)
		{
			Check(RD->IsActive(), TEXT("tele_ragdoll_running"), TEXT("Jimmy is a ragdoll when the door is used"));
			P->LastDoorUseTime = -1000.0;
			DoorUsesBefore = P->DoorUseCount;
			Check(DoorExt->UseDoor(P), TEXT("tele_door_accepted"), TEXT("door used while ragdolled"));
			Phase = 2;
			break;
		}
		if (Phase == 2 && P->DoorUseCount > DoorUsesBefore && !P->bDoorBusy)
		{
			Check(!RD->IsActive(), TEXT("door_aborts_ragdoll"), TEXT("the door dropped the ragdoll before moving the capsule"));
			Phase = 3;
			StepStart = Now;
			break;
		}
		if (Phase == 3 && T > 1.2 && (PC->GetState() == EBullyCombatState::Idle || T > 8.0))   // the knocked-down state must be over before the next knockdown
		{
			const FVector Want = DoorInt->ExitLocation;
			Check(FVector::Dist2D(P->GetActorLocation(), Want) < 300.f && !RD->IsActive(), TEXT("door_teleport_not_undone"), FString::Printf(TEXT("1.2s after the cut Jimmy is %.0f cm from the lobby exit (not dragged back to the body)"), float(FVector::Dist2D(P->GetActorLocation(), Want))));
			// a teleport that does NOT call Abort() first (console, script): the ragdoll's own guard must give way
			PC->ApplyWeaponHit(EBullyHitReaction::Knockdown, 0.f, FVector(-200.f, 0.f, 120.f));
			Phase = 4;
			StepStart = Now;
			break;
		}
		if (Phase == 4 && T > 0.5)
		{
			Check(RD->IsActive(), TEXT("raw_tele_ragdoll_running"), TEXT("ragdolled again"));
			RagStart = P->GetActorLocation() + FVector(4000.f, 0.f, 0.f);
			P->SetActorLocation(RagStart, false, nullptr, ETeleportType::TeleportPhysics);   // no Abort() on purpose
			Phase = 5;
			StepStart = Now;
			break;
		}
		if (Phase == 5 && T > 0.6)
		{
			Check(!RD->IsActive() && FVector::Dist2D(P->GetActorLocation(), RagStart) < 150.f, TEXT("raw_teleport_drops_ragdoll"), FString::Printf(TEXT("ragdoll dropped, capsule stayed %.0f cm from where it was sent"), float(FVector::Dist2D(P->GetActorLocation(), RagStart))));
			// doors: stand right outside the main door and let the prewarm timer run
			const FVector Exit = DoorExt->ExitLocation;
			P->SetActorLocation(FVector(Exit.X, Exit.Y, FloorZ(Exit, 300.f) + P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 3.f), false, nullptr, ETeleportType::TeleportPhysics);
			P->GetCharacterMovement()->StopMovementImmediately();
			Next(EStep::Door);
			break;
		}
		if (T > 12.0) { Fail(TEXT("tele_ragdoll"), FString::Printf(TEXT("stuck in phase %d"), Phase)); Next(EStep::Finish); }
		break;
	}
	case EStep::Door:
	{
		APlayerController* Ctl = Cast<APlayerController>(P->GetController());
		APlayerCameraManager* Cam = Ctl ? Ctl->PlayerCameraManager : nullptr;
		if (Phase == 0)
		{
			if (T < 2.5) { break; }   // prewarm timer ticks every 0.5s while we stand within PrewarmRadius of the door
			Check(DoorExt->PrewarmedComponents > 0, TEXT("door_prewarm_streams_destination"), FString::Printf(TEXT("%d interior mesh component(s) pre-streamed around the lobby exit while standing at the door"), DoorExt->PrewarmedComponents));
			DoorUsesBefore = P->DoorUseCount;
			DoorCutsBefore = DoorExt->InstantCuts;
			DoorFrom = P->GetActorLocation();
			MaxFade = 0.f;
			DoorTicks = 0;
			P->LastDoorUseTime = -1000.0;
			Check(DoorExt->UseDoor(P), TEXT("door_use_accepted"), TEXT("UseDoor() accepted (default fade)"));
			Phase = 1;
			break;
		}
		++DoorTicks;
		if (Cam) { MaxFade = FMath::Max(MaxFade, Cam->FadeAmount); }
		if (P->DoorUseCount > DoorUsesBefore && !P->bDoorBusy)
		{
			const float Moved = float(FVector::Dist(P->GetActorLocation(), DoorFrom));
			Check(MaxFade <= 0.001f, TEXT("door_no_black_frame"), FString::Printf(TEXT("camera fade amount stayed %.3f for the whole transition (no black/dark frame)"), MaxFade));
			Check(DoorTicks <= 4, TEXT("door_instant"), FString::Printf(TEXT("teleported %d tick(s) after use (%.0f cm away)"), DoorTicks, Moved));
			Check(DoorExt->InstantCuts == DoorCutsBefore + 1 && Moved > 5000.f, TEXT("door_cut_arrived"), FString::Printf(TEXT("instant cuts %d (was %d), pawn now at %s"), DoorExt->InstantCuts, DoorCutsBefore, *P->GetActorLocation().ToCompactString()));
			Next(EStep::DoorFade);
			break;
		}
		if (T > 5.0) { Fail(TEXT("door_instant"), TEXT("door cut did not complete in 5s")); Next(EStep::DoorFade); }
		break;
	}
	case EStep::DoorFade:
	{
		APlayerController* Ctl = Cast<APlayerController>(P->GetController());
		APlayerCameraManager* Cam = Ctl ? Ctl->PlayerCameraManager : nullptr;
		if (Phase == 0)
		{
			if (T < 2.0) { break; }   // past the door cooldown
			DoorUsesBefore = P->DoorUseCount;
			DoorFrom = P->GetActorLocation();
			MaxFade = 0.f;
			P->LastDoorUseTime = -1000.0;
			Check(DoorInt->UseDoor(P, 0.25f), TEXT("door_fade_accepted"), TEXT("UseDoor(Pawn, 0.25) accepted (cutscene/mission style)"));
			Phase = 1;
			break;
		}
		if (Cam) { MaxFade = FMath::Max(MaxFade, Cam->FadeAmount); }
		if (P->DoorUseCount > DoorUsesBefore && !P->bDoorBusy && Cam && Cam->FadeAmount <= 0.001f)
		{
			Check(MaxFade >= 0.99f, TEXT("door_requested_fade_still_works"), FString::Printf(TEXT("explicit fade reached %.2f and cleared; pawn at %s"), MaxFade, *P->GetActorLocation().ToCompactString()));
			Next(EStep::Finish);
			break;
		}
		if (T > 6.0) { Fail(TEXT("door_requested_fade_still_works"), FString::Printf(TEXT("fade run did not finish (max fade %.2f, busy %d)"), MaxFade, P->bDoorBusy)); Next(EStep::Finish); }
		break;
	}
	case EStep::Finish:
		Finish();
		break;
	}
}

void UBullyPhysicsSmoke::CheckBystanders()
{
	bBystanderChecked = true;
	AAmbientWalker* J = BJoin.Get();
	AAmbientWalker* F = BFlee.Get();
	AAmbientWalker* Far = BFar.Get();
	ABullyCharacter* P = Player.Get();
	if (J)
	{
		Check(J->IsBrawling() && Cast<ABullyCombatAIController>(J->GetController()) != nullptr, TEXT("bystander_boy_joins"),
			FString::Printf(TEXT("1s after the first hit the boy 10 m away fights: brawling=%d ai=%d"), J->IsBrawling(), Cast<ABullyCombatAIController>(J->GetController()) != nullptr));
	}
	if (F && P)
	{
		const float D = float(FVector::Dist2D(F->GetActorLocation(), P->GetActorLocation()));
		Check(F->IsFleeing() && D >= FleeD0 + 100.f, TEXT("bystander_nerd_flees"), FString::Printf(TEXT("fleeing=%d, now %.0f cm from Jimmy (was %.0f)"), F->IsFleeing(), D, FleeD0));
	}
	if (Far) { Check(!Far->IsBrawling() && !Far->IsFleeing(), TEXT("bystander_out_of_range_ignores"), TEXT("a student 26 m away is not part of it")); }
	// done with them: a joiner would keep chasing Jimmy through the later steps
	for (AAmbientWalker* K : { J, F, Far })
	{
		if (!K) { continue; }
		if (AController* C = K->GetController()) { C->Destroy(); }
		K->Destroy();
	}
	BJoin.Reset(); BFlee.Reset(); BFar.Reset();
}

void UBullyPhysicsSmoke::Finish()
{
	bDone = true;
	Note(FString::Printf(TEXT("RESULT %s: %d passed, %d failed"), Failures == 0 ? TEXT("PASS") : TEXT("FAIL"), Passes, Failures));
	FString Json = FString::Printf(TEXT("{\n  \"result\": \"%s\",\n  \"passed\": %d,\n  \"failed\": %d,\n  \"lines\": [\n"), Failures == 0 ? TEXT("PASS") : TEXT("FAIL"), Passes, Failures);
	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		Json += FString::Printf(TEXT("    \"%s\"%s\n"), *Lines[i].ReplaceCharWithEscapedChar(), i + 1 < Lines.Num() ? TEXT(",") : TEXT(""));
	}
	Json += TEXT("  ]\n}\n");
	FFileHelper::SaveStringToFile(Json, *(OutDir / TEXT("physics_result.json")));
	if (GEngine) { GEngine->Exec(GetWorld(), TEXT("quit")); }
}

// ---------------------------------------------------------------------------------------------------------------
// wall probe

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Components/PrimitiveComponent.h"

bool UBullyWallProbe::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* W = Cast<UWorld>(Outer);
	return W && W->IsGameWorld() && FString(FCommandLine::Get()).Contains(TEXT("-BRWallProbe"));
}
TStatId UBullyWallProbe::GetStatId() const { RETURN_QUICK_DECLARE_CYCLE_STAT(UBullyWallProbe, STATGROUP_Tickables); }
void UBullyWallProbe::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	if (!FParse::Value(FCommandLine::Get(), TEXT("-BRWallProbe="), OutDir)) { OutDir = FPaths::ProjectSavedDir() / TEXT("BullyPhysics"); }
	IFileManager::Get().MakeDirectory(*OutDir, true);
	StartedAt = InWorld.GetTimeSeconds();
}
void UBullyWallProbe::Tick(float Dt)
{
	Super::Tick(Dt);
	if (!bDone && GetWorld() && GetWorld()->GetTimeSeconds() - StartedAt > 5.0) { bDone = true; Run(); }
}

void UBullyWallProbe::Run()
{
	UWorld* W = GetWorld();
	TArray<TPair<FString, FVector>> Pts;
	FString Txt;
	if (FFileHelper::LoadFileToString(Txt, *(FPaths::ProjectConfigDir() / TEXT("InteriorSpots.json"))))
	{
		TSharedPtr<FJsonObject> Root;
		if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Txt), Root) && Root.IsValid() && Root->HasTypedField<EJson::Object>(TEXT("spots")))
		{
			for (const auto& KV : Root->GetObjectField(TEXT("spots"))->Values)
			{
				const TSharedPtr<FJsonObject>* O = nullptr;
				const TArray<TSharedPtr<FJsonValue>>* L = nullptr;
				if (KV.Value->TryGetObject(O) && (*O)->TryGetArrayField(TEXT("location_cm"), L) && L->Num() == 3)
				{
					Pts.Emplace(KV.Key, FVector((*L)[0]->AsNumber(), (*L)[1]->AsNumber(), (*L)[2]->AsNumber()));
				}
			}
		}
	}
	for (TActorIterator<ABullyDoorLink> It(W); It; ++It) { Pts.Emplace(FString::Printf(TEXT("door:%s"), *It->LinkId.ToString()), It->ExitLocation); }

	FString Json = TEXT("{\n  \"spots\": [\n");
	int32 Probed = 0, TotalRays = 0, TotalOpen = 0, TotalCapsuleOnly = 0, TotalBack = 0;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(BullyWallProbe), /*bTraceComplex*/ true);
	for (const TPair<FString, FVector>& P : Pts)
	{
		FHitResult Floor;
		if (!W->LineTraceSingleByChannel(Floor, P.Value + FVector(0, 0, 300.f), P.Value - FVector(0, 0, 1500.f), ECC_Visibility, Q)) { continue; }
		const FVector Base = Floor.ImpactPoint;
		int32 Rays = 0, Open = 0, CapOnly = 0, Back = 0, CapBlocked = 0;
		float SumLineDist = 0.f;
		for (int32 Hgt = 0; Hgt < 2; ++Hgt)
		{
			const float H = Hgt == 0 ? 60.f : 150.f;
			for (int32 i = 0; i < 32; ++i)
			{
				const float Yaw = i * (360.f / 32.f);
				const FVector D = FRotator(0.f, Yaw, 0.f).Vector();
				const FVector A = Base + FVector(0, 0, H);
				FHitResult L;
				const bool bLine = W->LineTraceSingleByChannel(L, A, A + D * 2500.f, ECC_Visibility, Q);
				FHitResult C;
				const FVector CA = Base + FVector(0, 0, 90.f);
				const bool bCap = W->SweepSingleByChannel(C, CA, CA + D * 2500.f, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(30.f, 85.f), Q);
				++Rays;
				if (!bLine) { ++Open; }
				if (bLine && FVector::DotProduct(L.ImpactNormal, D) > 0.05f) { ++Back; }
				if (bCap) { ++CapBlocked; }
				if (!bLine && bCap && C.Distance < 2400.f) { ++CapOnly; }
				SumLineDist += bLine ? L.Distance : 2500.f;
			}
		}
		++Probed; TotalRays += Rays; TotalOpen += Open; TotalCapsuleOnly += CapOnly; TotalBack += Back;
		UE_LOG(LogBullyPhys, Display, TEXT("BR_WALL %-18s floor z=%.0f rays=%d no_hit_25m=%d (%.0f%%) capsule_blocked=%d capsule_blocks_where_trace_sees_nothing=%d backface_hits=%d mean_wall_dist=%.0f cm"),
			*P.Key, Base.Z, Rays, Open, 100.f * Open / Rays, CapBlocked, CapOnly, Back, SumLineDist / Rays);
		Json += FString::Printf(TEXT("    {\"spot\": \"%s\", \"rays\": %d, \"no_hit\": %d, \"capsule_blocked\": %d, \"capsule_only\": %d, \"backface_hits\": %d},\n"), *P.Key, Rays, Open, CapBlocked, CapOnly, Back);
	}
	Json += FString::Printf(TEXT("    {\"total_spots\": %d, \"rays\": %d, \"no_hit\": %d, \"capsule_only\": %d, \"backface_hits\": %d}\n  ]\n}\n"), Probed, TotalRays, TotalOpen, TotalCapsuleOnly, TotalBack);
	UE_LOG(LogBullyPhys, Display, TEXT("BR_WALL SUMMARY spots=%d rays=%d no_hit_25m=%d capsule_blocks_where_trace_sees_nothing=%d backface_hits=%d"), Probed, TotalRays, TotalOpen, TotalCapsuleOnly, TotalBack);
	FFileHelper::SaveStringToFile(Json, *(OutDir / TEXT("wall_probe.json")));
	if (GEngine) { GEngine->Exec(W, TEXT("quit")); }
}
