// OWNER: slice-vehicles
#include "BullySandboxSubsystem.h"

#include "BullyCharacter.h"
#include "BullyRagdollComponent.h"
#include "BullyCombatPawn.h"
#include "BullyCombatTest.h"
#include "BullyVehicle.h"
#include "BullyWeaponComponent.h"
#include "Components/CapsuleComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MissionSubsystem.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogBullySandbox, Log, All);

namespace
{
	bool IsWater(const FHitResult& H)
	{
		const AActor* A = H.GetActor();
		if (!A) { return false; }
		for (const FName& T : A->Tags) { if (T.ToString().Contains(TEXT("water"), ESearchCase::IgnoreCase)) { return true; } }
		return A->GetName().Contains(TEXT("water"), ESearchCase::IgnoreCase);
	}

	bool GroundAt(const UWorld* W, const FVector2D& XY, float FromZ, FHitResult& Out)
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(BullySandboxGround), true);
		return W->LineTraceSingleByObjectType(Out, FVector(XY, FromZ), FVector(XY, -2000.f), FCollisionObjectQueryParams(ECC_WorldStatic), Q);
	}
}

bool UBullySandboxSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer)) { return false; }
	const UWorld* W = Cast<UWorld>(Outer);
	return W && W->IsGameWorld() && W->GetNetMode() != NM_DedicatedServer;
}

TStatId UBullySandboxSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UBullySandboxSubsystem, STATGROUP_Tickables);
}

bool UBullySandboxSubsystem::LoadConfig()
{
	FString Text;
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(Text, *(FPaths::ProjectConfigDir() / TEXT("Sandbox.json")))
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		return false;
	}
	bool bEnabled = false;
	Root->TryGetBoolField(TEXT("enabled"), bEnabled);
	Root->TryGetStringField(TEXT("start"), StartKey);
	double N = 0.0;
	if (Root->TryGetNumberField(TEXT("search_radius_cm"), N)) { SearchRadius = float(N); }
	if (Root->TryGetNumberField(TEXT("min_ground_z_cm"), N)) { MinGroundZ = float(N); }
	TArray<FString> Ids;
	if (Root->TryGetStringArrayField(TEXT("vehicles"), Ids)) { for (const FString& S : Ids) { VehicleIds.Add(FName(*S)); } }
	return bEnabled;
}

void UBullySandboxSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	const bool bEnabled = LoadConfig();

	TArray<FString> Tokens, Switches;
	FCommandLine::Parse(FCommandLine::Get(), Tokens, Switches);
	bool bForced = false, bOff = false, bHarness = false;
	for (const FString& S : Switches)
	{
		if (S.StartsWith(TEXT("BRSandbox"), ESearchCase::IgnoreCase))
		{
			bForced = true;
			if (S.Equals(TEXT("BRSandboxSmoke"), ESearchCase::IgnoreCase)) { bSmoke = true; }
			FString Key;
			if (S.Split(TEXT("="), nullptr, &Key) && !Key.IsEmpty()) { StartKey = Key; }
		}
		else if (S.Equals(TEXT("BRNoSandbox"), ESearchCase::IgnoreCase)) { bOff = true; }
		else if (S.StartsWith(TEXT("BR"), ESearchCase::IgnoreCase)) { bHarness = true; }   // a test harness run
	}
	const UMissionSubsystem* M = UMissionSubsystem::Get(&InWorld);
	const bool bMissionBoot = M && !M->BootMission.IsEmpty();
	bActive = !bOff && (bForced || (bEnabled && !bHarness && !bMissionBoot));
	StartedAt = InWorld.GetTimeSeconds();
	UE_LOG(LogBullySandbox, Display, TEXT("BR_SANDBOX %s (start '%s', %d ride(s)%s)"), bActive ? TEXT("armed") : TEXT("off"),
		*StartKey, VehicleIds.Num(), bSmoke ? TEXT(", smoke") : TEXT(""));
}

bool UBullySandboxSubsystem::SpotLocation(const FString& Key, FVector& Out) const
{
	FString Text;
	TSharedPtr<FJsonObject> Root;
	const TSharedPtr<FJsonObject>* Spots = nullptr;
	const TSharedPtr<FJsonObject>* Spot = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* L = nullptr;
	if (!FFileHelper::LoadFileToString(Text, *(FPaths::ProjectConfigDir() / TEXT("InteriorSpots.json")))
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid()
		|| !Root->TryGetObjectField(TEXT("spots"), Spots) || !(*Spots)->TryGetObjectField(Key.ToLower(), Spot)
		|| !(*Spot)->TryGetArrayField(TEXT("location_cm"), L) || L->Num() != 3)
	{
		return false;
	}
	Out = FVector((*L)[0]->AsNumber(), (*L)[1]->AsNumber(), (*L)[2]->AsNumber());
	return true;
}

bool UBullySandboxSubsystem::FindOpenArea(const FVector& InAnchor, const FVector2D& Half, FVector& OutCenter, float& OutYaw) const
{
	const UWorld* W = GetWorld();
	const float FromZ = InAnchor.Z + 4000.f;
	for (float Ring = 0.f; Ring <= SearchRadius; Ring += 350.f)
	{
		const int32 Count = Ring < 1.f ? 1 : FMath::Max(8, int32(2.f * PI * Ring / 500.f));
		for (int32 k = 0; k < Count; ++k)
		{
			const float A = 2.f * PI * k / Count;
			const FVector2D P(InAnchor.X + Ring * FMath::Cos(A), InAnchor.Y + Ring * FMath::Sin(A));
			FHitResult C;
			if (!GroundAt(W, P, FromZ, C) || C.ImpactNormal.Z < 0.93f || C.ImpactPoint.Z < MinGroundZ || IsWater(C)) { continue; }
			// a roof/awning has real ground a storey or more under it (road layers stacked on terrain are only cm apart)
			FHitResult Below;
			if (GroundAt(W, P, C.ImpactPoint.Z - 40.f, Below) && !IsWater(Below) && C.ImpactPoint.Z - Below.ImpactPoint.Z > 250.f) { continue; }
			for (const float Yaw : { 0.f, 90.f })
			{
				const FRotator R(0.f, Yaw, 0.f);
				const FVector X = R.Vector(), Y = FRotationMatrix(R).GetUnitAxis(EAxis::Y);
				bool bFlat = true;
				for (int32 c = 0; c < 4 && bFlat; ++c)
				{
					const FVector Corner = C.ImpactPoint + X * Half.X * (c & 1 ? 1.f : -1.f) + Y * Half.Y * (c & 2 ? 1.f : -1.f);
					FHitResult H;
					bFlat = GroundAt(W, FVector2D(Corner), C.ImpactPoint.Z + 300.f, H) && !IsWater(H)
						&& FMath::Abs(H.ImpactPoint.Z - C.ImpactPoint.Z) < 70.f && H.ImpactNormal.Z > 0.85f;
				}
				if (!bFlat) { continue; }
				const FVector BoxCenter = C.ImpactPoint + FVector(0.f, 0.f, 45.f + 160.f);
				FCollisionObjectQueryParams Obj;
				Obj.AddObjectTypesToQuery(ECC_WorldStatic);
				Obj.AddObjectTypesToQuery(ECC_WorldDynamic);
				if (W->OverlapAnyTestByObjectType(BoxCenter, R.Quaternion(), Obj, FCollisionShape::MakeBox(FVector(Half.X, Half.Y, 160.f)))) { continue; }
				OutCenter = C.ImpactPoint;
				OutYaw = Yaw;
				UE_LOG(LogBullySandbox, Display, TEXT("BR_SANDBOX open area %.0fx%.0f cm at %s yaw %.0f (%.0f cm from the spot)"),
					Half.X * 2.f, Half.Y * 2.f, *OutCenter.ToCompactString(), Yaw, Ring);
				return true;
			}
		}
	}
	return false;
}

void UBullySandboxSubsystem::Setup(ABullyCharacter* Player)
{
	UWorld* W = GetWorld();
	if (!SpotLocation(StartKey, Anchor))
	{
		UE_LOG(LogBullySandbox, Warning, TEXT("BR_SANDBOX no spot '%s' in InteriorSpots.json: staying put"), *StartKey);
		Anchor = Player->GetActorLocation();
	}
	FVector Center = Anchor;
	float Yaw = 0.f;
	auto Search = [&](const FVector& Around)
	{
		return FindOpenArea(Around, FVector2D(850.f, 520.f), Center, Yaw)
			|| FindOpenArea(Around, FVector2D(600.f, 420.f), Center, Yaw)
			|| FindOpenArea(Around, FVector2D(420.f, 300.f), Center, Yaw);
	};
	bool bFound = Search(Anchor);
	if (!bFound)
	{
		// Nothing there (e.g. this machine only imported the academy): never drop the player into empty air.
		// Stay where they spawned and park the rides next to them instead.
		UE_LOG(LogBullySandbox, Warning, TEXT("BR_SANDBOX no open ground near '%s' (area not imported here? run tools.importers world --preset all + run_import.sh --scenes all): rides go next to the player"), *StartKey);
		Anchor = Player->GetActorLocation();
		const float SavedMin = MinGroundZ;
		MinGroundZ = -100000.f;   // the academy sits lower than the town streets
		bFound = Search(Anchor);
		MinGroundZ = SavedMin;
		if (!bFound) { Center = Anchor - FVector(0.f, 0.f, 90.f); Yaw = Player->GetActorRotation().Yaw; }
	}

	const FRotator R(0.f, Yaw, 0.f);
	const FVector Fwd = R.Vector(), Right = FRotationMatrix(R).GetUnitAxis(EAxis::Y);

	// the rides, side by side across the patch, all facing the same way
	float Total = 0.f;
	for (const FName& Id : VehicleIds) { if (const FBullyVehicleDef* D = FBullyVehicleDef::Find(Id)) { Total += D->HalfExtent.Y * 2.f + 180.f; } }
	float Cursor = -Total * 0.5f;
	for (const FName& Id : VehicleIds)
	{
		const FBullyVehicleDef* D = FBullyVehicleDef::Find(Id);
		if (!D) { continue; }
		Cursor += D->HalfExtent.Y + 90.f;
		if (ABullyVehicle* V = ABullyVehicle::SpawnVehicle(W, Id, Center + Fwd * 250.f + Right * Cursor + FVector(0, 0, 50), Yaw))
		{
			Lineup.Add(V);
			LineupHome.Add(V->GetActorTransform());
		}
		Cursor += D->HalfExtent.Y + 90.f;
	}

	// the player, a few steps back, looking at them
	const float HalfH = Player->GetCapsuleComponent() ? Player->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : 88.f;
	const FVector PStand = Center - Fwd * 420.f + FVector(0.f, 0.f, HalfH + 5.f);
	if (UBullyRagdollComponent* RD = Player->GetRagdoll()) { RD->Abort(); }   // a ragdolled Jimmy would be dragged back to his body
	Player->SetActorLocationAndRotation(PStand, R, false, nullptr, ETeleportType::TeleportPhysics);
	if (AController* C = Player->GetController()) { C->SetControlRotation(FRotator(-10.f, Yaw, 0.f)); }
	Player->SnapCamera();

	UE_LOG(LogBullySandbox, Display, TEXT("BR_SANDBOX ready in '%s': player at %s, %d ride(s), %d weapon(s) unlocked"),
		*StartKey, *PStand.ToCompactString(), Lineup.Num(), Player->GetWeapons() ? Player->GetWeapons()->GetDefs().Num() : 0);
	if (GEngine && !bSmoke)
	{
		GEngine->AddOnScreenDebugMessage(INDEX_NONE, 14.f, FColor(250, 200, 60),
			TEXT("TOWN SANDBOX  -  walk up to a ride and press E  |  mouse wheel picks a weapon, LMB fires  |  G teleports anywhere"));
	}
}

void UBullySandboxSubsystem::Tick(float DeltaTime)
{
	if (!bActive) { return; }
	UWorld* W = GetWorld();
	if (!bDone)
	{
		APlayerController* PC = W->GetFirstPlayerController();
		ABullyCharacter* P = PC ? Cast<ABullyCharacter>(PC->GetPawn()) : nullptr;
		if (P && W->GetTimeSeconds() - StartedAt > 1.0)
		{
			bDone = true;
			Setup(P);
			Smoke.StepStart = W->GetTimeSeconds();
		}
		return;
	}
	if (bSmoke) { TickSmoke(DeltaTime); }
}

// ---------------------------------------------------------------------------------------------------------------
// headless smoke: drive every ride

void UBullySandboxSubsystem::TickSmoke(float Dt)
{
	UWorld* W = GetWorld();
	const double Now = W->GetTimeSeconds();
	const double T = Now - Smoke.StepStart;
	APlayerController* PC = W->GetFirstPlayerController();
	auto Next = [&](int32 S) { Smoke.Step = S; Smoke.StepStart = Now; };
	auto Fail = [&](const FString& Why) { Smoke.Failures.Add(Why); UE_LOG(LogBullySandbox, Error, TEXT("BR_SANDBOX_SMOKE FAIL: %s"), *Why); };
	auto Pass = [&](const FString& What) { ++Smoke.Passes; UE_LOG(LogBullySandbox, Display, TEXT("BR_SANDBOX_SMOKE PASS: %s"), *What); };
	ABullyVehicle* V = Lineup.IsValidIndex(Smoke.Index) ? Lineup[Smoke.Index].Get() : nullptr;
	static TWeakObjectPtr<ABullyCharacter> Player;

	switch (Smoke.Step)
	{
	case 0:
	{
		if (T < 1.0) { break; }
		Player = PC ? Cast<ABullyCharacter>(PC->GetPawn()) : nullptr;
		if (!Player.IsValid()) { Fail(TEXT("no player")); Next(9); break; }
		const float D = FVector::Dist2D(Player->GetActorLocation(), Anchor);
		if (D < SearchRadius + 1500.f) { Pass(FString::Printf(TEXT("player starts in '%s' (%.0f m from the spot)"), *StartKey, D / 100.f)); }
		else { Fail(FString::Printf(TEXT("player is %.0f m from '%s'"), D / 100.f, *StartKey)); }
		if (Lineup.Num() == VehicleIds.Num()) { Pass(FString::Printf(TEXT("%d rides lined up"), Lineup.Num())); }
		else { Fail(FString::Printf(TEXT("only %d of %d rides spawned"), Lineup.Num(), VehicleIds.Num())); }
		Next(1);
		break;
	}

	case 1:   // this ride alone on the patch: the others step aside; stand beside it, get on, full throttle
	{
		if (!V || !Player.IsValid()) { Next(9); break; }
		for (int32 i = 0; i < Lineup.Num(); ++i)
		{
			if (ABullyVehicle* O = Lineup[i].Get())
			{
				O->SetActorHiddenInGame(i != Smoke.Index);
				O->SetActorEnableCollision(i == Smoke.Index);
			}
		}
		V->SetActorTransform(LineupHome[Smoke.Index], false, nullptr, ETeleportType::TeleportPhysics);
		V->ResetMotion();
		Player->SetActorLocation(V->GetActorLocation() - V->GetActorRightVector() * (V->GetDef().HalfExtent.Y + 120.f) + FVector(0, 0, 60),
			false, nullptr, ETeleportType::TeleportPhysics);
		if (!V->Mount(Player.Get()) || !PC || PC->GetPawn() != V) { Fail(FString::Printf(TEXT("%s: could not get on"), *V->GetDef().Name)); Next(6); break; }
		Smoke.StartLoc = V->GetActorLocation();
		Smoke.PeakSpeed = 0.f;
		V->SetScriptedInput(true, 1.f, 0.f);
		Next(2);
		break;
	}

	case 2:   // accelerate (until a third of top speed, max 1.3 s, so the fast ones don't leave the patch)
		Smoke.PeakSpeed = FMath::Max(Smoke.PeakSpeed, V ? V->GetSpeed() : 0.f);
		if (T < 1.3 && Smoke.PeakSpeed < 0.35f * V->GetDef().MaxSpeed) { break; }
		{
			const float Moved = FVector::Dist2D(V->GetActorLocation(), Smoke.StartLoc);
			if (Smoke.PeakSpeed > 0.3f * V->GetDef().MaxSpeed && Moved > 100.f)
			{
				Pass(FString::Printf(TEXT("%s accelerates: %.0f km/h, %.1f m in %.1f s"), *V->GetDef().Name, Smoke.PeakSpeed * 0.036f, Moved / 100.f, T));
			}
			else { Fail(FString::Printf(TEXT("%s barely moved: %.0f km/h, %.1f m"), *V->GetDef().Name, Smoke.PeakSpeed * 0.036f, Moved / 100.f)); }
			Smoke.PeakSpeed = FMath::Abs(V->GetSpeed());   // speed going into the brake test
			V->SetScriptedInput(true, 0.f, 0.f, true);
			Next(3);
		}
		break;

	case 3:   // handbrake to a stop from real speed
		if (FMath::Abs(V->GetSpeed()) > 40.f && T < 3.0) { break; }
		if (FMath::Abs(V->GetSpeed()) <= 40.f && Smoke.PeakSpeed > 200.f)
		{
			Pass(FString::Printf(TEXT("%s brakes from %.0f km/h to a stop in %.1f s"), *V->GetDef().Name, Smoke.PeakSpeed * 0.036f, T));
		}
		else { Fail(FString::Printf(TEXT("%s braking: from %.0f km/h, now %.0f km/h"), *V->GetDef().Name, Smoke.PeakSpeed * 0.036f, FMath::Abs(V->GetSpeed()) * 0.036f)); }
		// fresh start for the turn
		V->SetActorTransform(LineupHome[Smoke.Index], false, nullptr, ETeleportType::TeleportPhysics);
		V->ResetMotion();
		Smoke.StartYaw = V->GetActorRotation().Yaw;
		V->SetScriptedInput(true, 0.6f, 1.f);
		Next(4);
		break;

	case 4:   // steer from a standing start
		if (T < 1.5) { break; }
		{
			const float Turned = FMath::Abs(FRotator::NormalizeAxis(V->GetActorRotation().Yaw - Smoke.StartYaw));
			if (Turned > 20.f) { Pass(FString::Printf(TEXT("%s steers: %.0f deg in 1.5 s"), *V->GetDef().Name, Turned)); }
			else { Fail(FString::Printf(TEXT("%s did not turn (%.0f deg)"), *V->GetDef().Name, Turned)); }
			V->SetScriptedInput(true, 0.f, 0.f, true);
		}
		if (Smoke.Index == Lineup.Num() - 1)
		{
			// last ride: plough into a pedestrian
			V->SetActorTransform(LineupHome[Smoke.Index], false, nullptr, ETeleportType::TeleportPhysics);
			V->ResetMotion();
			if (ABullyCombatPawn* E = UBullyCombatTest::SpawnEnemyInFront(W, Player.Get(), 650.f, 0.f))
			{
				E->bAttackAllowed = false;
				E->bBlockAllowed = false;
			}
			Smoke.RunOversBefore = ABullyVehicle::TotalRunOvers;
			V->SetScriptedInput(true, 1.f, 0.f);
			Next(5);
		}
		else { Next(6); }
		break;

	case 5:   // run-over
		if (ABullyVehicle::TotalRunOvers == Smoke.RunOversBefore && T < 4.0) { break; }
		if (ABullyVehicle::TotalRunOvers > Smoke.RunOversBefore) { Pass(FString::Printf(TEXT("%s knocks a pedestrian over"), *V->GetDef().Name)); }
		else { Fail(FString::Printf(TEXT("%s never hit the pedestrian"), *V->GetDef().Name)); }
		V->SetScriptedInput(true, 0.f, 0.f, true);
		Next(6);
		break;

	case 6:   // get off
		if (T < 0.5) { break; }
		if (V)
		{
			V->SetScriptedInput(false);
			V->Dismount();
			const bool bOk = Player.IsValid() && PC && PC->GetPawn() == Player.Get() && !Player->GetAttachParentActor() && Player->GetActorEnableCollision();
			if (bOk) { Pass(FString::Printf(TEXT("%s: got off, back on foot"), *V->GetDef().Name)); }
			else { Fail(FString::Printf(TEXT("%s: dismount left the player in a bad state"), *V->GetDef().Name)); }
		}
		++Smoke.Index;
		Next(Smoke.Index < Lineup.Num() ? 1 : 9);
		break;

	case 9:
		for (int32 i = 0; i < Lineup.Num(); ++i)   // everyone back on show
		{
			if (ABullyVehicle* O = Lineup[i].Get())
			{
				O->SetActorHiddenInGame(false);
				O->SetActorEnableCollision(true);
				O->SetActorTransform(LineupHome[i], false, nullptr, ETeleportType::TeleportPhysics);
				O->ResetMotion();
			}
		}
		UE_LOG(LogBullySandbox, Display, TEXT("BR_SANDBOX_SMOKE RESULT %s: %d check(s) passed, %d failure(s); mounts %d, dismounts %d, run-overs %d"),
			Smoke.Failures.Num() == 0 ? TEXT("PASS") : TEXT("FAIL"), Smoke.Passes, Smoke.Failures.Num(),
			ABullyVehicle::TotalMounts, ABullyVehicle::TotalDismounts, ABullyVehicle::TotalRunOvers);
		Smoke.Step = 99;
		GEngine->Exec(W, TEXT("quit"));
		break;

	default:
		break;
	}
}
