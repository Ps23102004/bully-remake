// OWNER: slice-npc (scripted NPCs)
// Headless driver for the scripted-NPC beats: -BRNPCSmoke[=stage,gauntlet,crowd] (default: all three).
// It plays the part of the mission framework and of the player/combat code, entirely through the public surfaces they will
// use: mission events (UMissionSubsystem::FireEvent), a started mission (stage hooks), and the REAL player pawn walking
// into the gauntlet with real collision. Every step logs BR_SNPC_SMOKE; the run ends with RESULT PASS|FAIL and exits.
#include "BullyScriptedNPC.h"

#include "BullyCharacter.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "Kismet/GameplayStatics.h"
#include "MissionSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogSNPCSmoke, Log, All);

void UScriptedNPCSubsystem::TickSmoke(float Dt)
{
	UWorld* W = GetWorld();
	const double Now = W->GetTimeSeconds();
	APawn* P = UGameplayStatics::GetPlayerPawn(W, 0);
	if (!P || Smoke.bAborted) { return; }
	UMissionSubsystem* M = UMissionSubsystem::Get(W);
	const double T = Now - Smoke.StepStart;
	auto Next = [&]() { ++Smoke.Step; Smoke.StepStart = Now; };
	auto Goto = [&](int32 Step) { Smoke.Step = Step; Smoke.StepStart = Now; };
	auto Fail = [&](const FString& Msg) { Smoke.Failures.Add(Msg); UE_LOG(LogSNPCSmoke, Error, TEXT("BR_SNPC_SMOKE FAIL: %s"), *Msg); };
	auto Pass = [&](const FString& Msg) { UE_LOG(LogSNPCSmoke, Display, TEXT("BR_SNPC_SMOKE PASS: %s"), *Msg); };
	auto Saw = [&](const TCHAR* Name) { for (const auto& E : Emitted) { if (E.Key == FName(Name)) { return true; } } return false; };
	auto Count = [&](const TCHAR* Name) { int32 N = 0; for (const auto& E : Emitted) { if (E.Key == FName(Name)) { ++N; } } return N; };
	auto Has = [&](const TCHAR* Phase) { return Smoke.Phases.Contains(Phase); };
	auto Teleport = [&](const FVector& Where, float Yaw)
	{
		FHitResult Hit;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(SNPCSmokeTp), true, P);
		FVector Dest = Where;
		if (W->LineTraceSingleByChannel(Hit, Where + FVector(0, 0, 800.f), Where - FVector(0, 0, 1200.f), ECC_Visibility, Q)) { Dest.Z = Hit.ImpactPoint.Z + 90.f; }
		P->SetActorLocation(Dest, false, nullptr, ETeleportType::TeleportPhysics);
		if (ACharacter* C = Cast<ACharacter>(P)) { C->GetCharacterMovement()->StopMovementImmediately(); }
		if (AController* Ctl = P->GetController()) { Ctl->SetControlRotation(FRotator(0.f, Yaw, 0.f)); }
		UE_LOG(LogSNPCSmoke, Display, TEXT("BR_SNPC_SMOKE player placed at %s facing yaw %.0f"), *Dest.ToString(), Yaw);
	};
	auto GroupActive = [&](const TCHAR* Id, int32& OutNpcs)
	{
		const FSNPCGroup* G = FindGroup(Id);
		OutNpcs = 0;
		if (G) { for (const auto& N : G->Spawned) { if (N.IsValid()) { ++OutNpcs; } } }
		return G && G->bActive;
	};

	if (Now >= Smoke.NextLog && Smoke.Step >= 10)
	{
		Smoke.NextLog = Now + 2.0;
		UE_LOG(LogSNPCSmoke, Display, TEXT("BR_SNPC_SMOKE t=%.1f step %d player (%.0f,%.0f,%.0f) emitted %d events"), Now, Smoke.Step, P->GetActorLocation().X, P->GetActorLocation().Y, P->GetActorLocation().Z, Emitted.Num());
	}

	switch (Smoke.Step)
	{
	case 0:   // let the world settle (ambient walkers spawn, collision streams in)
		if (T < 4.0) { break; }
		LoadScript(TEXT("_test_stage_hook"));
		Goto(Has(TEXT("stage")) ? 10 : (Has(TEXT("gauntlet")) ? 20 : 30));
		break;

	// ---- phase: mission stage hooks (UMissionSubsystem::OnStageBegan / OnMissionFinished) ----
	case 10:
		if (!M) { Fail(TEXT("no mission subsystem")); Goto(Has(TEXT("gauntlet")) ? 20 : 30); break; }
		M->StartMission(TEXT("smoke_day_one"));
		Next();
		break;
	case 11:
	{
		if (T < 1.5) { break; }
		int32 N = 0;
		if (GroupActive(TEXT("stage_probe"), N) && N == 1 && Saw(TEXT("probe_spawned"))) { Pass(TEXT("mission_start:smoke_day_one spawned the probe group (1 NPC)")); }
		else { Fail(FString::Printf(TEXT("stage probe group did not spawn on mission start (npcs %d)"), N)); }
		M->SkipToStage(TEXT("use_door"));
		Next();
		break;
	}
	case 12:
		if (Saw(TEXT("probe_stage_ok"))) { Pass(TEXT("stage_begin:smoke_day_one:use_door fired the probe beat and it finished (goto + gesture)")); Next(); }
		else if (T > 20.0) { Fail(TEXT("probe beat on stage_begin never completed")); Next(); }
		break;
	case 13:
		if (M) { M->StopMission(TEXT("smoke")); }
		Next();
		break;
	case 14:
	{
		if (T < 1.0) { break; }
		int32 N = 0;
		if (!GroupActive(TEXT("stage_probe"), N)) { Pass(TEXT("mission_end:smoke_day_one:* despawned the probe group")); }
		else { Fail(TEXT("probe group still active after the mission ended")); }
		Goto(Has(TEXT("gauntlet")) ? 20 : (Has(TEXT("crowd")) ? 30 : 90));
		break;
	}

	// ---- phase: gauntlet (1_01) ----
	case 20:
		Teleport(FVector(29836.f, 7223.f, 607.f), 180.f);   // 1_01_Player_Start, facing -X toward the school
		Next();
		break;
	case 21:
		if (T < 1.0) { break; }
		UE_LOG(LogSNPCSmoke, Display, TEXT("BR_SNPC_SMOKE firing mission event npc_gauntlet_spawn (as the 1_01 stage would)"));
		if (M) { M->FireEvent(TEXT("npc_gauntlet_spawn")); } else { Dispatch(TEXT("event:npc_gauntlet_spawn")); }
		Next();
		break;
	case 22:
	{
		if (T < 0.5) { break; }
		int32 N = 0;
		if (GroupActive(TEXT("gauntlet_1_01"), N) && N == 3) { Pass(TEXT("gauntlet spawned 3 bullies")); } else { Fail(FString::Printf(TEXT("gauntlet group active/npcs wrong (%d)"), N)); }
		UE_LOG(LogSNPCSmoke, Display, TEXT("BR_SNPC_SMOKE player runs west along y=7223 toward x=27000, through the bullies"));
		Next();
		break;
	}
	case 23:   // the player walks (real pawn, real collision) until the gauntlet reports complete and he is past the bullies
	{
		if (ABullyCharacter* C = Cast<ABullyCharacter>(P))
		{
			C->SetScriptedGait(EBullyGait::Run);
			const FVector Goal(27000.f, 7223.f, P->GetActorLocation().Z);
			FVector D = Goal - P->GetActorLocation();
			D.Z = 0.f;
			if (D.Size() > 80.f) { P->AddMovementInput(D.GetSafeNormal(), 1.f); }
			else { C->ClearScriptedGait(); Next(); }
		}
		if (Saw(TEXT("gauntlet_complete")) && P->GetActorLocation().X < 27800.f) { Pass(FString::Printf(TEXT("gauntlet_complete emitted and the player is past the bullies at x=%.0f"), P->GetActorLocation().X)); Next(); }
		else if (T > 60.0) { Fail(TEXT("gauntlet did not complete in 60 s")); Next(); }
		break;
	}
	case 24:
	{
		if (ABullyCharacter* C = Cast<ABullyCharacter>(P)) { C->ClearScriptedGait(); }
		if (T < 1.5) { break; }
		LogStatus();
		if (Saw(TEXT("gauntlet_engaged"))) { Pass(TEXT("gauntlet_engaged emitted")); } else { Fail(TEXT("no gauntlet_engaged")); }
		if (Count(TEXT("gauntlet_shove")) >= 1) { Pass(FString::Printf(TEXT("%d shove(s) emitted"), Count(TEXT("gauntlet_shove")))); } else { Fail(TEXT("no gauntlet_shove")); }
		if (Saw(TEXT("gauntlet_released"))) { Pass(TEXT("gauntlet_released emitted")); } else { Fail(TEXT("no gauntlet_released")); }
		UE_LOG(LogSNPCSmoke, Display, TEXT("BR_SNPC_SMOKE firing npc_gauntlet_ambient: the bullies disperse and become ambient walkers"));
		if (M) { M->FireEvent(TEXT("npc_gauntlet_ambient")); } else { Dispatch(TEXT("event:npc_gauntlet_ambient")); }
		Next();
		break;
	}
	case 25:
	{
		if (Saw(TEXT("gauntlet_ambient")) || T > 40.0)
		{
			int32 Amb = 0;
			if (const FSNPCGroup* G = FindGroup(TEXT("gauntlet_1_01"))) { for (const auto& N : G->Spawned) { if (N.IsValid() && N->IsAmbientNow()) { ++Amb; } } }
			if (Amb == 3) { Pass(TEXT("all 3 bullies are ambient walkers now (hand-back to AAmbientWalker loops)")); } else { Fail(FString::Printf(TEXT("%d of 3 bullies became ambient"), Amb)); }
			Next();
		}
		break;
	}
	case 26:   // watch the handed-back walkers loop for a few seconds, then clear the group
		if (T < 6.0) { break; }
		LogStatus();
		if (M) { M->FireEvent(TEXT("npc_gauntlet_clear")); } else { Dispatch(TEXT("event:npc_gauntlet_clear")); }
		Goto(Has(TEXT("crowd")) ? 30 : 90);
		break;

	// ---- phase: crowd (1_02) ----
	case 30:
		Teleport(FVector(27116.f, 10593.f, 640.f), 90.f);   // 1_02_PlayerNISLocs[0]: where Jimmy stands for the fight
		Next();
		break;
	case 31:
		if (T < 1.0) { break; }
		UE_LOG(LogSNPCSmoke, Display, TEXT("BR_SNPC_SMOKE firing mission event npc_crowd_gather (the fight-cutscene cue)"));
		if (M) { M->FireEvent(TEXT("npc_crowd_gather")); } else { Dispatch(TEXT("event:npc_crowd_gather")); }
		Next();
		break;
	case 32:
		if (Saw(TEXT("crowd_gathered"))) { Pass(FString::Printf(TEXT("crowd_gathered emitted %.1f s after the cue"), T)); Next(); }
		else if (T > 90.0) { LogStatus(); Fail(TEXT("crowd did not gather in 90 s")); Next(); }
		break;
	case 33:
		if (T < 6.0) { break; }
		if (Saw(TEXT("crowd_cheering"))) { Pass(TEXT("crowd_cheering emitted; crowd cheered for 6 s")); } else { Fail(TEXT("no crowd_cheering")); }
		LogStatus();
		UE_LOG(LogSNPCSmoke, Display, TEXT("BR_SNPC_SMOKE firing enemy_knocked_out (the combat builder's event)"));
		if (M) { M->FireEvent(TEXT("enemy_knocked_out")); } else { Dispatch(TEXT("event:enemy_knocked_out")); }
		Next();
		break;
	case 34:
		if (Saw(TEXT("crowd_dispersed"))) { Pass(FString::Printf(TEXT("crowd_dispersed emitted %.1f s after enemy_knocked_out"), T)); Next(); }
		else if (T > 90.0) { LogStatus(); Fail(TEXT("crowd did not disperse in 90 s")); Next(); }
		break;
	case 35:
	{
		if (T < 1.0) { break; }
		int32 N = 0;
		if (GroupActive(TEXT("crowd_1_02"), N)) { Fail(FString::Printf(TEXT("crowd group still active with %d NPCs"), N)); } else { Pass(TEXT("crowd despawned after dispersing")); }
		Goto(90);
		break;
	}

	default:
	{
		const bool bOk = Smoke.Failures.Num() == 0;
		UE_LOG(LogSNPCSmoke, Display, TEXT("BR_SNPC_SMOKE RESULT %s (%d failures) emitted: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), Smoke.Failures.Num(),
			*FString::JoinBy(Emitted, TEXT(", "), [](const TPair<FName, double>& E) { return FString::Printf(TEXT("%s@%.1f"), *E.Key.ToString(), E.Value); }));
		for (const FString& F : Smoke.Failures) { UE_LOG(LogSNPCSmoke, Display, TEXT("BR_SNPC_SMOKE   failure: %s"), *F); }
		Smoke.bAborted = true;
		FPlatformMisc::RequestExit(false);
		break;
	}
	}
}
