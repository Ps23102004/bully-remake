// OWNER: mission-fw
#include "MissionSubsystem.h"

#include "BullyCharacter.h"
#include "BullyCombatComponent.h"
#include "BullyCombatPawn.h"
#include "BullyCombatTest.h"
#include "BullyHUD.h"
#include "BullyProgressSubsystem.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Components/BoxComponent.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "MissionTrigger.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogBullyMission, Log, All);

namespace
{
	using FObj = TSharedPtr<FJsonObject>;

	FString MissionDir() { return FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("Missions")); }

	bool ReadVec3(const FJsonObject& O, const TCHAR* Field, FVector& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
		if (O.TryGetArrayField(Field, A) && A->Num() >= 3)
		{
			Out = FVector((*A)[0]->AsNumber(), (*A)[1]->AsNumber(), (*A)[2]->AsNumber());
			return true;
		}
		return false;
	}

	FVector GameToUe(const FVector& M) { return FVector(M.X * 100.0, -M.Y * 100.0, M.Z * 100.0); }

	/** A location from "point" (named), "location_cm"/"center_cm" or "game_m". `Prefix` allows e.g. "blip". */
	bool ReadLocation(const FJsonObject& O, const FMissionDef& D, FVector& Out)
	{
		FString PointName;
		if (O.TryGetStringField(TEXT("point"), PointName))
		{
			if (const FVector* P = D.Points.Find(FName(*PointName))) { Out = *P; return true; }
			for (const FMissionTriggerDef& T : D.Triggers) { if (T.Id == FName(*PointName)) { Out = T.Center; return true; } }
			return false;
		}
		if (ReadVec3(O, TEXT("location_cm"), Out) || ReadVec3(O, TEXT("center_cm"), Out)) { return true; }
		FVector M;
		if (ReadVec3(O, TEXT("game_m"), M)) { Out = GameToUe(M); return true; }
		return false;
	}

	bool ParseCondition(const FObj& O, const FMissionDef& D, FMissionCondition& C, FString& Err)
	{
		FString Type;
		if (!O.IsValid() || !O->TryGetStringField(TEXT("type"), Type)) { Err = TEXT("condition missing \"type\""); return false; }
		auto Num = [&](const TCHAR* F, float Def) { double V; return O->TryGetNumberField(F, V) ? float(V) : Def; };
		FString S;
		if (Type == TEXT("reach_location") || Type == TEXT("reach"))
		{
			C.Type = FMissionCondition::EType::Reach;
			if (!ReadLocation(*O, D, C.Location)) { Err = TEXT("reach_location: unknown point / no location"); return false; }
			C.Radius = Num(TEXT("radius_cm"), 300.f);
			C.ZTolerance = Num(TEXT("z_tolerance_cm"), 400.f);
		}
		else if (Type == TEXT("trigger_enter") || Type == TEXT("trigger_exit"))
		{
			C.Type = Type == TEXT("trigger_enter") ? FMissionCondition::EType::TriggerEnter : FMissionCondition::EType::TriggerExit;
			if (!O->TryGetStringField(TEXT("trigger"), S)) { Err = Type + TEXT(": missing \"trigger\""); return false; }
			C.Name = FName(*S);
		}
		else if (Type == TEXT("interact"))
		{
			C.Type = FMissionCondition::EType::Interact;
			if (O->TryGetStringField(TEXT("trigger"), S)) { C.Name = FName(*S); }
			else if (O->TryGetStringField(TEXT("actor_tag"), S)) { C.Name = FName(*S); C.bActorTag = true; }
			else { Err = TEXT("interact: needs \"trigger\" or \"actor_tag\""); return false; }
			C.Radius = Num(TEXT("radius_cm"), 250.f);
			O->TryGetStringField(TEXT("prompt"), C.Prompt);
		}
		else if (Type == TEXT("event"))
		{
			C.Type = FMissionCondition::EType::Event;
			if (!O->TryGetStringField(TEXT("name"), S)) { Err = TEXT("event: missing \"name\""); return false; }
			C.Name = FName(*S);
			O->TryGetBoolField(TEXT("latched"), C.bLatched);
		}
		else if (Type == TEXT("timer")) { C.Type = FMissionCondition::EType::Timer; C.Seconds = Num(TEXT("seconds"), 1.f); }
		else if (Type == TEXT("flag"))
		{
			C.Type = FMissionCondition::EType::Flag;
			if (!O->TryGetStringField(TEXT("name"), S)) { Err = TEXT("flag: missing \"name\""); return false; }
			C.Name = FName(*S);
		}
		else if (Type == TEXT("all") || Type == TEXT("any"))
		{
			C.Type = Type == TEXT("all") ? FMissionCondition::EType::All : FMissionCondition::EType::Any;
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (!O->TryGetArrayField(TEXT("conditions"), Arr) || Arr->Num() == 0) { Err = Type + TEXT(": needs \"conditions\""); return false; }
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				FMissionCondition Child;
				if (!ParseCondition(V->AsObject(), D, Child, Err)) { return false; }
				C.Children.Add(MoveTemp(Child));
			}
		}
		else { Err = FString::Printf(TEXT("unknown condition type \"%s\""), *Type); return false; }
		return true;
	}

	bool ParseActions(const FObj& Parent, const TCHAR* Field, const FMissionDef& D, TArray<FMissionAction>& Out, FString& Err)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Parent->TryGetArrayField(Field, Arr)) { return true; }
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			FObj O = V->AsObject();
			FString Type, S;
			if (!O.IsValid() || !O->TryGetStringField(TEXT("type"), Type)) { Err = TEXT("action missing \"type\""); return false; }
			FMissionAction A;
			double Sec = 0.0;
			if (O->TryGetNumberField(TEXT("seconds"), Sec)) { A.Seconds = float(Sec); }
			O->TryGetStringField(TEXT("text"), A.Text);
			O->TryGetStringField(TEXT("sub"), A.Sub);
			double Yaw = 0.0; if (O->TryGetNumberField(TEXT("yaw"), Yaw)) { A.Yaw = float(Yaw); }
			if (O->TryGetStringField(TEXT("name"), S)) { A.Name = FName(*S); }
			if (Type == TEXT("hint")) { A.Type = FMissionAction::EType::Hint; }
			else if (Type == TEXT("banner")) { A.Type = FMissionAction::EType::Banner; }
			else if (Type == TEXT("event") || Type == TEXT("fire_event")) { A.Type = FMissionAction::EType::Event; }
			else if (Type == TEXT("set_flag")) { A.Type = FMissionAction::EType::SetFlag; }
			else if (Type == TEXT("clear_flag")) { A.Type = FMissionAction::EType::ClearFlag; }
			else if (Type == TEXT("log")) { A.Type = FMissionAction::EType::Log; }
			else if (Type == TEXT("teleport"))
			{
				A.Type = FMissionAction::EType::Teleport;
				if (!ReadLocation(*O, D, A.Location)) { Err = TEXT("teleport: unknown point / no location"); return false; }
			}
			else if (Type == TEXT("start_mission"))
			{
				A.Type = FMissionAction::EType::StartMission;
				if (!O->TryGetStringField(TEXT("mission"), A.Text) || A.Text.IsEmpty()) { Err = TEXT("start_mission: missing \"mission\" id"); return false; }
				if (A.Seconds >= 5.f) { A.Seconds = 0.5f; }   // default: brief beat after the complete banner, unless overridden
			}
			else if (Type == TEXT("spawn_enemy"))
			{
				A.Type = FMissionAction::EType::SpawnEnemy;
				double D2 = 0.0;
				if (O->TryGetNumberField(TEXT("distance_cm"), D2)) { A.Dist = float(D2); }
				if (O->TryGetNumberField(TEXT("health_floor"), D2)) { A.HealthFloor = float(D2); }
				bool b = false;
				if (O->TryGetBoolField(TEXT("player_invulnerable"), b)) { A.PlayerInvuln = b ? 1 : 0; }
				if (O->TryGetBoolField(TEXT("gate_attack"), b)) { A.GateAttack = b ? 1 : 0; }
				if (O->TryGetBoolField(TEXT("gate_block"), b)) { A.GateBlock = b ? 1 : 0; }
			}
			else if (Type == TEXT("fade"))
			{
				A.Type = FMissionAction::EType::Fade;
				double F = 0.0;
				if (O->TryGetNumberField(TEXT("out"), F))  { A.FadeOut  = FMath::Max(0.f, float(F)); }
				if (O->TryGetNumberField(TEXT("hold"), F)) { A.FadeHold = FMath::Max(0.f, float(F)); }
				if (O->TryGetNumberField(TEXT("in"), F))   { A.FadeIn   = FMath::Max(0.f, float(F)); }
				// Optional teleport (resolved like the teleport action) happens at full black, so there is no visible jump.
				A.bFadeTeleport = ReadLocation(*O, D, A.Location);
			}
			else if (Type == TEXT("reward"))
			{
				A.Type = FMissionAction::EType::Reward;
				double N = 0.0;
				if (O->TryGetNumberField(TEXT("money"), N))   { A.Money = int32(N); }
				if (O->TryGetNumberField(TEXT("respect"), N)) { A.Respect = int32(N); }
			}
			else if (Type == TEXT("letterbox"))
			{
				A.Type = FMissionAction::EType::Letterbox;
				bool b = true; O->TryGetBoolField(TEXT("on"), b); A.bOn = b;   // default on
			}
			else if (Type == TEXT("say"))
			{
				A.Type = FMissionAction::EType::Say;
				O->TryGetStringField(TEXT("speaker"), A.Sub);   // Sub = speaker label; Text (line) already read above
				if (A.Text.IsEmpty()) { O->TryGetStringField(TEXT("line"), A.Text); }   // "line" accepted as an alias for "text"
				if (!O->HasField(TEXT("seconds"))) { A.Seconds = 4.f; }
			}
			else if (Type == TEXT("camera"))
			{
				A.Type = FMissionAction::EType::Camera;
				double B = 0.0; if (O->TryGetNumberField(TEXT("blend"), B)) { A.Blend = FMath::Max(0.f, float(B)); }
				bool bRel = false; O->TryGetBoolField(TEXT("release"), bRel); A.bRelease = bRel;
				if (!A.bRelease)
				{
					if (!ReadLocation(*O, D, A.Location)) { Err = TEXT("camera: needs a point / location_cm"); return false; }
					// look target: "look":"player"|"<point>" or a "look_cm":[x,y,z] spot (a point name wins)
					FString Lk;
					if (O->TryGetStringField(TEXT("look"), Lk))
					{
						if (Lk.Equals(TEXT("player"), ESearchCase::IgnoreCase)) { A.bLookPlayer = true; }
						else if (const FVector* P = D.Points.Find(FName(*Lk))) { A.Look = *P; A.bHasLook = true; }
						else { Err = FString::Printf(TEXT("camera: unknown look point \"%s\""), *Lk); return false; }
					}
					else if (ReadVec3(*O, TEXT("look_cm"), A.Look)) { A.bHasLook = true; }
					double F = 0.0; if (O->TryGetNumberField(TEXT("fov"), F)) { A.Fov = float(F); }
				}
			}
			else { Err = FString::Printf(TEXT("unknown action type \"%s\""), *Type); return false; }
			Out.Add(MoveTemp(A));
		}
		return true;
	}

	/** Optional extra named points from another tool's JSON (imported/triggers/day_one.json). Tolerant of layout: a root
	 *  "points" / "triggers" object (name -> entry) or array (entry has "name"/"id"); an entry's position is
	 *  "location_cm" | "ue_cm" | "cm" (UE cm) or "game_m" | "pos" | "position" (game metres); optional "half_extent_cm". */
	int32 LoadTriggerFile(const FString& RepoRelPath, FMissionDef& D)
	{
		const FString Full = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../.."), RepoRelPath));
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Full))
		{
			UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION trigger_file %s not present (optional); using inline points only"), *Full);
			return 0;
		}
		FObj Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
		{
			UE_LOG(LogBullyMission, Warning, TEXT("BR_MISSION trigger_file %s is not valid JSON; ignored"), *Full);
			return 0;
		}
		int32 Added = 0;
		auto AddEntry = [&](const FString& Name, const FObj& E)
		{
			if (!E.IsValid() || Name.IsEmpty() || D.Points.Contains(FName(*Name))) { return; }   // inline wins
			FVector V;
			bool bOk = ReadVec3(*E, TEXT("location_cm"), V) || ReadVec3(*E, TEXT("ue_cm"), V) || ReadVec3(*E, TEXT("cm"), V);
			FVector M;
			if (!bOk && (ReadVec3(*E, TEXT("game_m"), M) || ReadVec3(*E, TEXT("pos"), M) || ReadVec3(*E, TEXT("position"), M))) { V = GameToUe(M); bOk = true; }
			if (!bOk) { return; }
			D.Points.Add(FName(*Name), V);
			FVector Ext;
			if (ReadVec3(*E, TEXT("half_extent_cm"), Ext))
			{
				FMissionTriggerDef T; T.Id = FName(*Name); T.Center = V; T.HalfExtent = Ext;
				D.Triggers.Add(T);
			}
			++Added;
		};
		for (const TCHAR* Key : { TEXT("points"), TEXT("triggers") })
		{
			const FObj* AsObj = nullptr; const TArray<TSharedPtr<FJsonValue>>* AsArr = nullptr;
			if (Root->TryGetObjectField(Key, AsObj))
			{
				for (const auto& Pair : (*AsObj)->Values) { AddEntry(FString(*Pair.Key), Pair.Value->AsObject()); }
			}
			else if (Root->TryGetArrayField(Key, AsArr))
			{
				for (const TSharedPtr<FJsonValue>& V : *AsArr)
				{
					FObj E = V->AsObject(); FString N;
					if (E.IsValid() && (E->TryGetStringField(TEXT("name"), N) || E->TryGetStringField(TEXT("id"), N))) { AddEntry(N, E); }
				}
			}
		}
		UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION trigger_file %s: %d extra points"), *Full, Added);
		return Added;
	}

	FString CondText(const FMissionCondition& C)
	{
		switch (C.Type)
		{
		case FMissionCondition::EType::Reach: return FString::Printf(TEXT("reach %s r=%.0f"), *C.Location.ToCompactString(), C.Radius);
		case FMissionCondition::EType::TriggerEnter: return FString::Printf(TEXT("enter trigger %s"), *C.Name.ToString());
		case FMissionCondition::EType::TriggerExit: return FString::Printf(TEXT("exit trigger %s"), *C.Name.ToString());
		case FMissionCondition::EType::Interact: return FString::Printf(TEXT("interact %s%s"), C.bActorTag ? TEXT("tag ") : TEXT("trigger "), *C.Name.ToString());
		case FMissionCondition::EType::Event: return FString::Printf(TEXT("event %s"), *C.Name.ToString());
		case FMissionCondition::EType::Timer: return FString::Printf(TEXT("timer %.1fs"), C.Seconds);
		case FMissionCondition::EType::Flag: return FString::Printf(TEXT("flag %s"), *C.Name.ToString());
		case FMissionCondition::EType::All: return TEXT("all-of");
		case FMissionCondition::EType::Any: return TEXT("any-of");
		default: return TEXT("none");
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------
// subsystem lifecycle

UMissionSubsystem* UMissionSubsystem::Get(UWorld* World)
{
	return World ? World->GetSubsystem<UMissionSubsystem>() : nullptr;
}

bool UMissionSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer)) { return false; }
	const UWorld* W = Cast<UWorld>(Outer);
	return W && W->IsGameWorld() && W->GetNetMode() != NM_DedicatedServer && !FParse::Param(FCommandLine::Get(), TEXT("BRNoMission"));
}

TStatId UMissionSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMissionSubsystem, STATGROUP_Tickables);
}

void UMissionSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	FString Id;
	if (FParse::Value(FCommandLine::Get(), TEXT("-BRMission="), Id))
	{
		PendingAutoStart = Id;
		AutoStartAt = InWorld.GetTimeSeconds() + 1.0;
	}
	else if (!BootMission.IsEmpty())
	{
		// Data-driven default (DefaultGame.ini): a packaged build with no args boots into this mission.
		PendingAutoStart = BootMission;
		AutoStartAt = InWorld.GetTimeSeconds() + 1.0;
		UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION boot mission '%s' (config BootMission)"), *BootMission);
	}
	FString SmokeId;
	if (FParse::Param(FCommandLine::Get(), TEXT("BRMissionSmoke")) || FParse::Value(FCommandLine::Get(), TEXT("-BRMissionSmoke="), SmokeId))
	{
		Smoke.bEnabled = true;
		Smoke.MissionId = SmokeId.IsEmpty() ? TEXT("smoke_day_one") : SmokeId;
		Smoke.bWantFailTest = !FParse::Param(FCommandLine::Get(), TEXT("BRMissionSmokeNoFail"));
		UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION smoke harness armed: mission '%s' (fail/retry test %s)"), *Smoke.MissionId, Smoke.bWantFailTest ? TEXT("on") : TEXT("off"));
	}
}

void UMissionSubsystem::Deinitialize()
{
	if (ACameraActor* Cam = CutsceneCam.Get()) { Cam->Destroy(); CutsceneCam = nullptr; }
	DestroyOwnedTriggers();
	if (Hud.IsValid()) { Hud->Hide(); Hud.Reset(); }
	Super::Deinitialize();
}

APawn* UMissionSubsystem::GetPlayerPawn() const
{
	return GetWorld() ? UGameplayStatics::GetPlayerPawn(GetWorld(), 0) : nullptr;
}

void UMissionSubsystem::TeleportPlayer(const FVector& Loc, float Yaw, bool bSetYaw)
{
	APawn* Pawn = GetPlayerPawn();
	if (!Pawn) { return; }
	Pawn->SetActorLocation(Loc, false, nullptr, ETeleportType::TeleportPhysics);
	if (ACharacter* C = Cast<ACharacter>(Pawn))
	{
		if (C->GetCharacterMovement()) { C->GetCharacterMovement()->StopMovementImmediately(); }
	}
	if (bSetYaw)
	{
		if (AController* Ctl = Pawn->GetController()) { Ctl->SetControlRotation(FRotator(0.f, Yaw, 0.f)); }
	}
}

// ---------------------------------------------------------------------------------------------------------------
// loading

TArray<FString> UMissionSubsystem::ListMissionIds()
{
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(MissionDir(), TEXT("*.json")), true, false);
	for (FString& F : Files) { F = FPaths::GetBaseFilename(F); }
	return Files;
}

bool UMissionSubsystem::LoadMission(const FString& Id, FMissionDef& D, FString& Err) const
{
	const FString Path = FPaths::Combine(MissionDir(), Id + TEXT(".json"));
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path)) { Err = FString::Printf(TEXT("cannot read %s"), *Path); return false; }
	FObj Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid()) { Err = FString::Printf(TEXT("%s is not valid JSON"), *Path); return false; }

	D.Id = Id;
	Root->TryGetStringField(TEXT("id"), D.Id);
	D.Title = D.Id;
	Root->TryGetStringField(TEXT("title"), D.Title);
	FString S;
	if (Root->TryGetStringField(TEXT("retry"), S)) { D.Retry = S == TEXT("mission") ? FMissionDef::ERetry::Mission : S == TEXT("none") ? FMissionDef::ERetry::None : FMissionDef::ERetry::Stage; }
	double N;
	if (Root->TryGetNumberField(TEXT("fail_delay"), N)) { D.FailDelay = float(N); }

	if (const FObj* Start = nullptr; Root->TryGetObjectField(TEXT("start"), Start))
	{
		if (ReadLocation(**Start, D, D.StartLocation)) { D.bHasStart = true; }
		if ((*Start)->TryGetNumberField(TEXT("yaw"), N)) { D.StartYaw = float(N); }
	}

	// points: inline first, then the optional trigger file (inline wins)
	if (const FObj* Pts = nullptr; Root->TryGetObjectField(TEXT("points"), Pts))
	{
		for (const auto& Pair : (*Pts)->Values)
		{
			FVector V;
			if (const TArray<TSharedPtr<FJsonValue>>* A = nullptr; Pair.Value->TryGetArray(A) && A->Num() >= 3) { D.Points.Add(FName(*Pair.Key), FVector((*A)[0]->AsNumber(), (*A)[1]->AsNumber(), (*A)[2]->AsNumber())); }
			else if (FObj PO = Pair.Value->AsObject(); PO.IsValid() && ReadLocation(*PO, D, V)) { D.Points.Add(FName(*Pair.Key), V); }
			else { Err = FString::Printf(TEXT("point \"%s\" has no location"), *Pair.Key); return false; }
		}
	}
	if (Root->TryGetStringField(TEXT("trigger_file"), S)) { LoadTriggerFile(S, D); }

	if (const TArray<TSharedPtr<FJsonValue>>* Trs = nullptr; Root->TryGetArrayField(TEXT("triggers"), Trs))
	{
		for (const TSharedPtr<FJsonValue>& V : *Trs)
		{
			FObj O = V->AsObject();
			FMissionTriggerDef T;
			if (!O.IsValid() || !O->TryGetStringField(TEXT("id"), S)) { Err = TEXT("trigger missing \"id\""); return false; }
			T.Id = FName(*S);
			if (!ReadLocation(*O, D, T.Center)) { Err = FString::Printf(TEXT("trigger \"%s\" has no center_cm"), *S); return false; }
			ReadVec3(*O, TEXT("half_extent_cm"), T.HalfExtent);
			if (O->TryGetNumberField(TEXT("yaw"), N)) { T.Yaw = float(N); }
			O->TryGetBoolField(TEXT("interact"), T.bInteract);
			O->TryGetStringField(TEXT("prompt"), T.Prompt);
			D.Triggers.RemoveAll([&](const FMissionTriggerDef& E) { return E.Id == T.Id; });   // inline overrides file-derived
			D.Triggers.Add(T);
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Stages = nullptr;
	if (!Root->TryGetArrayField(TEXT("stages"), Stages) || Stages->Num() == 0) { Err = TEXT("mission has no \"stages\""); return false; }
	int32 Index = 0;
	for (const TSharedPtr<FJsonValue>& V : *Stages)
	{
		++Index;
		FObj O = V->AsObject();
		FMissionStage St;
		if (!O.IsValid()) { Err = FString::Printf(TEXT("stage %d is not an object"), Index); return false; }
		St.Id = O->TryGetStringField(TEXT("id"), S) ? FName(*S) : FName(*FString::Printf(TEXT("stage%d"), Index));
		O->TryGetStringField(TEXT("objective"), St.Objective);
		// blip: branch on the JSON value's real type. (Do NOT probe with TryGetBoolField first: a string like
		// "office_trigger" parses as bool false via FString::ToBool, which would silently drop the waypoint.)
		//   array [x,y,z] -> explicit cm   |   false -> no blip, true -> auto   |   "point/trigger id" -> explicit   |   absent -> auto
		if (TSharedPtr<FJsonValue> BlipVal = O->TryGetField(TEXT("blip")))
		{
			if (BlipVal->Type == EJson::Array)
			{
				const TArray<TSharedPtr<FJsonValue>>& BA = BlipVal->AsArray();
				if (BA.Num() < 3) { Err = FString::Printf(TEXT("stage %d: blip array needs 3 numbers"), Index); return false; }
				St.BlipMode = FMissionStage::EBlip::Explicit;
				St.BlipLocation = FVector(BA[0]->AsNumber(), BA[1]->AsNumber(), BA[2]->AsNumber());
			}
			else if (BlipVal->Type == EJson::Boolean)
			{
				St.BlipMode = BlipVal->AsBool() ? FMissionStage::EBlip::Auto : FMissionStage::EBlip::None;
			}
			else if (BlipVal->Type == EJson::String)
			{
				const FString BlipName = BlipVal->AsString();
				FVector BlipV; FJsonObject Tmp; Tmp.SetStringField(TEXT("point"), BlipName);
				if (!ReadLocation(Tmp, D, BlipV)) { Err = FString::Printf(TEXT("stage %d: blip \"%s\" is not a point/trigger"), Index, *BlipName); return false; }
				St.BlipMode = FMissionStage::EBlip::Explicit; St.BlipLocation = BlipV;
			}
			else { Err = FString::Printf(TEXT("stage %d: blip must be an array, bool, or point/trigger name"), Index); return false; }
		}
		const FObj* CO = nullptr;
		if (!O->TryGetObjectField(TEXT("complete"), CO)) { Err = FString::Printf(TEXT("stage %d (%s) has no \"complete\" condition"), Index, *St.Id.ToString()); return false; }
		if (!ParseCondition(*CO, D, St.Complete, Err)) { Err = FString::Printf(TEXT("stage %d complete: %s"), Index, *Err); return false; }
		if (O->TryGetObjectField(TEXT("fail"), CO) && !ParseCondition(*CO, D, St.Fail, Err)) { Err = FString::Printf(TEXT("stage %d fail: %s"), Index, *Err); return false; }
		if (O->TryGetObjectField(TEXT("checkpoint"), CO))
		{
			St.bHasCheckpoint = ReadLocation(**CO, D, St.CheckpointLocation);
			if ((*CO)->TryGetNumberField(TEXT("yaw"), N)) { St.CheckpointYaw = float(N); }
		}
		if (!ParseActions(O, TEXT("on_start"), D, St.OnStart, Err) || !ParseActions(O, TEXT("on_complete"), D, St.OnComplete, Err))
		{
			Err = FString::Printf(TEXT("stage %d: %s"), Index, *Err); return false;
		}
		D.Stages.Add(MoveTemp(St));
	}
	if (!ParseActions(Root, TEXT("on_complete"), D, D.OnComplete, Err)) { return false; }
	return true;
}

// ---------------------------------------------------------------------------------------------------------------
// control

bool UMissionSubsystem::StartMission(const FString& Id, int32 StartStage)
{
	UWorld* World = GetWorld();
	if (!World) { return false; }
	if (State != EState::Idle) { StopMission(TEXT("restarted")); }

	TSharedPtr<FMissionDef> D = MakeShared<FMissionDef>();
	FString Err;
	if (!LoadMission(Id, *D, Err))
	{
		UE_LOG(LogBullyMission, Error, TEXT("BR_MISSION start '%s' FAILED to load: %s"), *Id, *Err);
		return false;
	}
	Mission = D;
	Attempt = 1;
	Flags.Reset(); MissionEvents.Reset(); StageEvents.Reset();
	StartStage = FMath::Clamp(StartStage, 0, D->Stages.Num() - 1);

	if (D->bHasStart) { TeleportPlayer(D->StartLocation, D->StartYaw, true); }
	if (APawn* P = GetPlayerPawn())
	{
		MissionStartPawnLoc = P->GetActorLocation();
		MissionStartYaw = P->GetActorRotation().Yaw;
	}
	UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION start id=%s title='%s' stages=%d triggers=%d points=%d retry=%s from stage %d"),
		*D->Id, *D->Title, D->Stages.Num(), D->Triggers.Num(), D->Points.Num(),
		D->Retry == FMissionDef::ERetry::Stage ? TEXT("stage") : D->Retry == FMissionDef::ERetry::Mission ? TEXT("mission") : TEXT("none"), StartStage + 1);

	SpawnTriggers();
	EnsureHud();
	BeginStage(StartStage, false);
	return true;
}

void UMissionSubsystem::StopMission(const FString& Reason)
{
	if (State == EState::Idle) { return; }
	const bool bWasDone = State == EState::Complete;
	UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION stop id=%s (%s)"), Mission.IsValid() ? *Mission->Id : TEXT("?"), *Reason);
	ReleaseCutsceneCamera(0.f);   // never leave gameplay looking through a cutscene camera
	DestroyOwnedTriggers();
	if (Hud.IsValid()) { Hud->ClearObjective(); Hud->Hide(); }
	State = EState::Idle;
	if (Mission.IsValid() && !bWasDone) { OnMissionFinished.Broadcast(*Mission, false); }
}

bool UMissionSubsystem::SkipToStage(const FString& Arg)
{
	if (!Mission.IsValid() || State == EState::Idle) { UE_LOG(LogBullyMission, Warning, TEXT("BR_MISSION skip: no active mission")); return false; }
	int32 Target = INDEX_NONE;
	if (Arg.Equals(TEXT("next"), ESearchCase::IgnoreCase) || Arg.IsEmpty()) { Target = StageIndex + 1; }
	else if (Arg.IsNumeric()) { Target = FCString::Atoi(*Arg) - 1; }
	else { Target = Mission->Stages.IndexOfByPredicate([&](const FMissionStage& S) { return S.Id == FName(*Arg); }); }
	if (Target < 0 || Target > Mission->Stages.Num()) { UE_LOG(LogBullyMission, Warning, TEXT("BR_MISSION skip: bad stage '%s'"), *Arg); return false; }
	UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION skip -> stage %d"), Target + 1);
	if (Target == Mission->Stages.Num()) { CompleteMission(); return true; }
	State = EState::Running;
	BeginStage(Target, false);
	return true;
}

void UMissionSubsystem::FailMission(const FString& Reason)
{
	if (State != EState::Running || !Mission.IsValid()) { return; }
	const FMissionStage& St = Mission->Stages[StageIndex];
	State = EState::Failed;
	FailTime = GetWorld()->GetTimeSeconds();
	UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION stage %d: %s -> FAIL (%s) attempt %d"), StageIndex + 1, *St.Objective, *Reason, Attempt);
	if (Hud.IsValid())
	{
		Hud->SetBlip(false); Hud->SetPrompt(TEXT(""));
		Hud->ShowBanner(TEXT("MISSION FAILED"), Reason, Mission->FailDelay + 0.5f, true);
	}
}

void UMissionSubsystem::RetryNow()
{
	if (!Mission.IsValid()) { return; }
	if (Mission->Retry == FMissionDef::ERetry::None) { StopMission(TEXT("failed, retry disabled")); return; }
	++Attempt;
	if (Mission->Retry == FMissionDef::ERetry::Stage)
	{
		const FMissionStage& St = Mission->Stages[StageIndex];
		const FVector At = St.bHasCheckpoint ? St.CheckpointLocation : StageStartPawnLoc;
		UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION retry attempt %d: reload stage %d (%s) at %s"), Attempt, StageIndex + 1, St.bHasCheckpoint ? TEXT("checkpoint") : TEXT("stage-start pos"), *At.ToCompactString());
		TeleportPlayer(At, St.CheckpointYaw, St.bHasCheckpoint);
		State = EState::Running;
		BeginStage(StageIndex, true);
	}
	else
	{
		UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION retry attempt %d: restart mission at %s"), Attempt, *MissionStartPawnLoc.ToCompactString());
		Flags.Reset(); MissionEvents.Reset();
		TeleportPlayer(MissionStartPawnLoc, MissionStartYaw, true);
		State = EState::Running;
		BeginStage(0, true);
	}
}

void UMissionSubsystem::SetFlag(FName Flag, bool bOn)
{
	if (bOn) { Flags.Add(Flag); } else { Flags.Remove(Flag); }
}

void UMissionSubsystem::FireEvent(FName Event)
{
	StageEvents.Add(Event);
	MissionEvents.Add(Event);
	UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION event '%s'"), *Event.ToString());
	OnMissionEvent.Broadcast(Event);
}

// ---------------------------------------------------------------------------------------------------------------
// stage machine

void UMissionSubsystem::BeginStage(int32 Index, bool bRetry)
{
	if (!Mission.IsValid()) { return; }
	if (Index >= Mission->Stages.Num()) { CompleteMission(); return; }
	StageIndex = Index;
	State = EState::Running;
	const FMissionStage& St = Mission->Stages[Index];
	StageStartTime = GetWorld()->GetTimeSeconds();
	StageEvents.Reset();
	if (APawn* P = GetPlayerPawn()) { StageStartPawnLoc = P->GetActorLocation(); }

	// actors for actor_tag interact conditions
	TagActors.Reset();
	TArray<const FMissionCondition*> Interacts;
	CollectInteracts(St.Complete, Interacts);
	if (St.Fail.IsValid()) { CollectInteracts(St.Fail, Interacts); }
	for (const FMissionCondition* C : Interacts)
	{
		if (C->bActorTag)
		{
			TArray<AActor*> Found;
			UGameplayStatics::GetAllActorsWithTag(GetWorld(), C->Name, Found);
			for (AActor* A : Found) { TagActors.Add(A); }
		}
		else if (!Triggers.Contains(C->Name))
		{
			UE_LOG(LogBullyMission, Warning, TEXT("BR_MISSION stage %d interacts with unknown trigger '%s'"), Index + 1, *C->Name.ToString());
		}
	}

	if (Hud.IsValid())
	{
		Hud->SetObjective(Mission->Title, Index + 1, Mission->Stages.Num(), St.Objective);
		Hud->SetPrompt(TEXT(""));
	}
	UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION stage %d/%d: %s -> begin%s [complete: %s%s]"), Index + 1, Mission->Stages.Num(), *St.Objective,
		bRetry ? TEXT(" (retry)") : TEXT(""), *CondText(St.Complete), St.Fail.IsValid() ? *FString::Printf(TEXT("; fail: %s"), *CondText(St.Fail)) : TEXT(""));
	RunActions(St.OnStart);
	OnStageBegan.Broadcast(*Mission, Index);
}

void UMissionSubsystem::CompleteStage()
{
	if (!Mission.IsValid() || State != EState::Running) { return; }
	const FMissionStage& St = Mission->Stages[StageIndex];
	UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION stage %d: %s -> complete"), StageIndex + 1, *St.Objective);
	RunActions(St.OnComplete);
	OnStageCompleted.Broadcast(*Mission, StageIndex);
	if (StageIndex + 1 >= Mission->Stages.Num()) { CompleteMission(); }
	else { BeginStage(StageIndex + 1, false); }
}

void UMissionSubsystem::CompleteMission()
{
	if (!Mission.IsValid()) { return; }
	State = EState::Complete;
	UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION complete id=%s stages=%d attempts=%d"), *Mission->Id, Mission->Stages.Num(), Attempt);
	// A mission that chains straight into another (its on_complete starts a mission) is a beat in a longer sequence
	// (e.g. a cutscene handing off to gameplay), so skip the "MISSION COMPLETE" end-card and let the hand-off read clean.
	bool bChains = false;
	for (const FMissionAction& A : Mission->OnComplete) { if (A.Type == FMissionAction::EType::StartMission) { bChains = true; break; } }
	if (Hud.IsValid())
	{
		Hud->SetBlip(false); Hud->SetPrompt(TEXT(""));
		if (!bChains) { Hud->ShowBanner(TEXT("MISSION COMPLETE"), Mission->Title, 4.f, false); }
	}
	RunActions(Mission->OnComplete);
	// A terminal mission returns to gameplay, so never leave a cutscene camera live (a chaining one hands off via
	// StopMission, which releases it there). If on_complete deliberately set a camera, that's respected on the chain.
	if (!bChains) { ReleaseCutsceneCamera(0.6f); }
	DestroyOwnedTriggers();
	OnMissionFinished.Broadcast(*Mission, true);
}

void UMissionSubsystem::RunActions(const TArray<FMissionAction>& Actions)
{
	for (const FMissionAction& A : Actions)
	{
		switch (A.Type)
		{
		case FMissionAction::EType::Hint: if (Hud.IsValid()) { Hud->ShowHint(A.Text, A.Seconds); } break;
		case FMissionAction::EType::Banner: if (Hud.IsValid()) { Hud->ShowBanner(A.Text, A.Sub, A.Seconds, false); } break;
		case FMissionAction::EType::Event: FireEvent(A.Name); break;
		case FMissionAction::EType::SetFlag: SetFlag(A.Name, true); break;
		case FMissionAction::EType::ClearFlag: SetFlag(A.Name, false); break;
		case FMissionAction::EType::Teleport: TeleportPlayer(A.Location, A.Yaw, true); break;
		case FMissionAction::EType::Log: UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION log: %s"), *A.Text); break;
		case FMissionAction::EType::StartMission:
			// Defer to Tick (via the auto-start slot) so we never re-enter the stage machine from inside an action.
			if (UWorld* W = GetWorld())
			{
				PendingAutoStart = A.Text;
				AutoStartAt = W->GetTimeSeconds() + FMath::Max(0.f, A.Seconds);
				UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION start_mission '%s' in %.1fs"), *A.Text, A.Seconds);
			}
			break;
		case FMissionAction::EType::SpawnEnemy:
		{
			ABullyCharacter* Player = Cast<ABullyCharacter>(GetPlayerPawn());
			if (!Player) { UE_LOG(LogBullyMission, Warning, TEXT("BR_MISSION spawn_enemy: no player pawn")); break; }
			ABullyCombatPawn* E = UBullyCombatTest::SpawnEnemyInFront(GetWorld(), Player, A.Dist, A.HealthFloor);
			if (E)
			{
				if (A.GateAttack >= 0) { E->bAttackAllowed = A.GateAttack != 0; }
				if (A.GateBlock >= 0) { E->bBlockAllowed = A.GateBlock != 0; }
			}
			if (A.PlayerInvuln >= 0 && Player->GetCombat()) { Player->GetCombat()->bInvulnerable = A.PlayerInvuln != 0; }
			UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION spawn_enemy dist=%.0f floor=%.0f invuln=%d atk=%d blk=%d -> %s"),
				A.Dist, A.HealthFloor, A.PlayerInvuln, A.GateAttack, A.GateBlock, E ? *E->GetName() : TEXT("FAILED"));
			break;
		}
		case FMissionAction::EType::Fade:
			// Start the fade now; TickFade drives it (fade-out -> optional teleport at full black -> fade-in),
			// so the stage machine keeps running underneath (e.g. the end-card banner shows while black).
			if (UWorld* W = GetWorld())
			{
				FadeOutDur = A.FadeOut; FadeHoldDur = A.FadeHold; FadeInDur = A.FadeIn;
				bFadePendingTeleport = A.bFadeTeleport;
				FadeTeleportLoc = A.Location; FadeTeleportYaw = A.Yaw;
				FadePhase = EFadePhase::Out;
				FadePhaseStart = W->GetTimeSeconds();
				SetCameraFade(0.f);
				UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION fade out=%.2f hold=%.2f in=%.2f teleport=%d"),
					FadeOutDur, FadeHoldDur, FadeInDur, bFadePendingTeleport ? 1 : 0);
			}
			break;
		case FMissionAction::EType::Reward:
		{
			UBullyProgressSubsystem* Prog = UBullyProgressSubsystem::Get(GetWorld());
			if (Prog)
			{
				Prog->AddMoney(A.Money);
				Prog->AddRespect(A.Respect);
				if (Hud.IsValid()) { Hud->SetWallet(Prog->GetMoney(), Prog->GetRespect(), true); }
			}
			// Toast it: explicit text wins, else a generated line from whatever was granted.
			FString Line = A.Text;
			if (Line.IsEmpty())
			{
				TArray<FString> Parts;
				if (A.Money != 0)   { Parts.Add(FString::Printf(TEXT("$%d"), A.Money)); }
				if (A.Respect != 0) { Parts.Add(FString::Printf(TEXT("%d respect"), A.Respect)); }
				Line = Parts.Num() ? FString::Printf(TEXT("Reward: %s"), *FString::Join(Parts, TEXT("  +  "))) : FString();
			}
			if (!Line.IsEmpty() && Hud.IsValid()) { Hud->ShowHint(Line, 4.f); }
			UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION reward money=%d respect=%d"), A.Money, A.Respect);
			break;
		}
		case FMissionAction::EType::Letterbox:
			if (Hud.IsValid()) { Hud->SetLetterbox(A.bOn); }
			UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION letterbox %s"), A.bOn ? TEXT("on") : TEXT("off"));
			break;
		case FMissionAction::EType::Say:
			if (Hud.IsValid()) { Hud->Say(A.Sub, A.Text, A.Seconds); }
			UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION say %s: %s"), A.Sub.IsEmpty() ? TEXT("-") : *A.Sub, *A.Text);
			break;
		case FMissionAction::EType::Camera:
			ApplyCamera(A);
			break;
		}
	}
}

namespace { float FadeSmooth01(float X) { X = FMath::Clamp(X, 0.f, 1.f); return X * X * (3.f - 2.f * X); } }

void UMissionSubsystem::SetCameraFade(float Alpha)
{
	APawn* Pawn = GetPlayerPawn();
	if (!Pawn) { return; }
	if (APlayerController* PC = Cast<APlayerController>(Pawn->GetController()))
	{
		if (PC->PlayerCameraManager)
		{
			if (Alpha <= 0.f) { PC->PlayerCameraManager->StopCameraFade(); }
			else { PC->PlayerCameraManager->SetManualCameraFade(FMath::Min(Alpha, 1.f), FLinearColor::Black, /*bFadeAudio*/ false); }
		}
	}
}

void UMissionSubsystem::ApplyCamera(const FMissionAction& A)
{
	UWorld* World = GetWorld();
	APawn* Pawn = GetPlayerPawn();
	APlayerController* PC = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
	if (!World || !PC) { UE_LOG(LogBullyMission, Warning, TEXT("BR_MISSION camera: no player controller")); return; }

	if (A.bRelease) { ReleaseCutsceneCamera(A.Blend); return; }

	// aim: at the player, an explicit spot, or (default) keep the camera's current facing by looking slightly ahead
	FVector LookAt = A.Look;
	if (A.bLookPlayer && Pawn) { LookAt = Pawn->GetActorLocation() + FVector(0, 0, 60.f); }
	else if (!A.bHasLook) { LookAt = A.Location + FVector(100.f, 0, 0); }
	const FRotator Rot = (LookAt - A.Location).Rotation();

	ACameraActor* Cam = CutsceneCam.Get();
	if (!Cam)
	{
		FActorSpawnParameters SP; SP.ObjectFlags |= RF_Transient;
		Cam = World->SpawnActor<ACameraActor>(A.Location, Rot, SP);
		CutsceneCam = Cam;
	}
	if (!Cam) { UE_LOG(LogBullyMission, Warning, TEXT("BR_MISSION camera: spawn failed")); return; }
	Cam->SetActorLocationAndRotation(A.Location, Rot);
	if (A.Fov > 1.f && Cam->GetCameraComponent()) { Cam->GetCameraComponent()->SetFieldOfView(A.Fov); }
	PC->SetViewTargetWithBlend(Cam, A.Blend, VTBlend_Cubic);
	UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION camera at=%s look=%s fov=%.0f blend=%.2f"),
		*A.Location.ToCompactString(), *LookAt.ToCompactString(), A.Fov, A.Blend);
}

void UMissionSubsystem::ReleaseCutsceneCamera(float Blend)
{
	APawn* Pawn = GetPlayerPawn();
	if (APlayerController* PC = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr)
	{
		PC->SetViewTargetWithBlend(Pawn, FMath::Max(0.f, Blend), VTBlend_Cubic);
		UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION camera release -> player (blend=%.2f)"), Blend);
	}
	if (ACameraActor* Cam = CutsceneCam.Get()) { Cam->Destroy(); }
	CutsceneCam = nullptr;
}

void UMissionSubsystem::TickFade(float /*Dt*/)
{
	if (FadePhase == EFadePhase::None) { return; }
	UWorld* World = GetWorld();
	if (!World) { return; }
	const float Elapsed = float(World->GetTimeSeconds() - FadePhaseStart);
	switch (FadePhase)
	{
	case EFadePhase::Out:
	{
		const float Dur = FMath::Max(FadeOutDur, 0.02f);
		if (Elapsed < FadeOutDur) { SetCameraFade(FadeSmooth01(Elapsed / Dur)); break; }
		SetCameraFade(1.f);
		if (bFadePendingTeleport) { TeleportPlayer(FadeTeleportLoc, FadeTeleportYaw, true); bFadePendingTeleport = false; }
		FadePhase = EFadePhase::Hold;
		FadePhaseStart = World->GetTimeSeconds();
		break;
	}
	case EFadePhase::Hold:
		SetCameraFade(1.f);
		if (Elapsed >= FadeHoldDur) { FadePhase = EFadePhase::In; FadePhaseStart = World->GetTimeSeconds(); }
		break;
	case EFadePhase::In:
	{
		const float Dur = FMath::Max(FadeInDur, 0.02f);
		if (Elapsed < FadeInDur) { SetCameraFade(1.f - FadeSmooth01(Elapsed / Dur)); break; }
		SetCameraFade(0.f);
		FadePhase = EFadePhase::None;
		break;
	}
	default: break;
	}
}

// ---------------------------------------------------------------------------------------------------------------
// conditions

bool UMissionSubsystem::Evaluate(const FMissionCondition& C) const
{
	switch (C.Type)
	{
	case FMissionCondition::EType::Reach:
	{
		const APawn* P = GetPlayerPawn();
		if (!P) { return false; }
		const FVector L = P->GetActorLocation();
		return FVector::Dist2D(L, C.Location) <= C.Radius && FMath::Abs(L.Z - C.Location.Z) <= C.ZTolerance;
	}
	case FMissionCondition::EType::TriggerEnter:
	{
		const TWeakObjectPtr<AMissionTrigger>* T = Triggers.Find(C.Name);
		return T && T->IsValid() && (*T)->IsPlayerInside();
	}
	case FMissionCondition::EType::TriggerExit: return StageEvents.Contains(FName(*FString::Printf(TEXT("trigger_exit:%s"), *C.Name.ToString())));
	case FMissionCondition::EType::Interact: return StageEvents.Contains(FName(*FString::Printf(TEXT("interact:%s"), *C.Name.ToString())));
	case FMissionCondition::EType::Event: return StageEvents.Contains(C.Name) || (C.bLatched && MissionEvents.Contains(C.Name));
	case FMissionCondition::EType::Timer: return GetWorld()->GetTimeSeconds() - StageStartTime >= C.Seconds;
	case FMissionCondition::EType::Flag: return Flags.Contains(C.Name);
	case FMissionCondition::EType::All:
		for (const FMissionCondition& K : C.Children) { if (!Evaluate(K)) { return false; } }
		return true;
	case FMissionCondition::EType::Any:
		for (const FMissionCondition& K : C.Children) { if (Evaluate(K)) { return true; } }
		return false;
	default: return false;
	}
}

void UMissionSubsystem::CollectInteracts(const FMissionCondition& C, TArray<const FMissionCondition*>& Out) const
{
	if (C.Type == FMissionCondition::EType::Interact) { Out.Add(&C); }
	for (const FMissionCondition& K : C.Children) { CollectInteracts(K, Out); }
}

bool UMissionSubsystem::InteractInRange(const FMissionCondition& C, FVector* OutAt) const
{
	if (C.bActorTag)
	{
		const APawn* P = GetPlayerPawn();
		if (!P) { return false; }
		for (const TWeakObjectPtr<AActor>& A : TagActors)
		{
			if (A.IsValid() && FVector::Dist2D(A->GetActorLocation(), P->GetActorLocation()) <= C.Radius)
			{
				if (OutAt) { *OutAt = A->GetActorLocation(); }
				return true;
			}
		}
		return false;
	}
	const TWeakObjectPtr<AMissionTrigger>* T = Triggers.Find(C.Name);
	if (T && T->IsValid() && (*T)->IsPlayerInside())
	{
		if (OutAt) { *OutAt = (*T)->GetActorLocation(); }
		return true;
	}
	return false;
}

bool UMissionSubsystem::DoInteract()
{
	if (State != EState::Running || !Mission.IsValid()) { return false; }
	const FMissionStage& St = Mission->Stages[StageIndex];
	TArray<const FMissionCondition*> Interacts;
	CollectInteracts(St.Complete, Interacts);
	if (St.Fail.IsValid()) { CollectInteracts(St.Fail, Interacts); }
	bool bFired = false;
	for (const FMissionCondition* C : Interacts)
	{
		if (InteractInRange(*C))
		{
			const FName Ev(*FString::Printf(TEXT("interact:%s"), *C->Name.ToString()));
			StageEvents.Add(Ev);
			MissionEvents.Add(Ev);
			UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION interact '%s' (stage %d)"), *C->Name.ToString(), StageIndex + 1);
			OnMissionEvent.Broadcast(Ev);
			bFired = true;
		}
	}
	return bFired;
}

/** The world position a positional condition points at (also the HUD waypoint and the smoke harness target). */
bool UMissionSubsystem::ConditionTarget(const FMissionCondition& C, FVector& Out) const
{
	switch (C.Type)
	{
	case FMissionCondition::EType::Reach: Out = C.Location; return true;
	case FMissionCondition::EType::TriggerEnter:
	case FMissionCondition::EType::TriggerExit:
	case FMissionCondition::EType::Interact:
	{
		if (C.Type == FMissionCondition::EType::Interact && C.bActorTag)
		{
			const APawn* P = GetPlayerPawn();
			float Best = TNumericLimits<float>::Max(); bool bFound = false;
			for (const TWeakObjectPtr<AActor>& A : TagActors)
			{
				if (!A.IsValid()) { continue; }
				const float D = P ? FVector::DistSquared(A->GetActorLocation(), P->GetActorLocation()) : 0.f;
				if (D < Best) { Best = D; Out = A->GetActorLocation(); bFound = true; }
			}
			return bFound;
		}
		const TWeakObjectPtr<AMissionTrigger>* T = Triggers.Find(C.Name);
		if (T && T->IsValid()) { Out = (*T)->GetActorLocation(); return true; }
		return false;
	}
	case FMissionCondition::EType::All:
		for (const FMissionCondition& K : C.Children) { if (!Evaluate(K) && ConditionTarget(K, Out)) { return true; } }
		return false;
	case FMissionCondition::EType::Any:
	{
		const APawn* P = GetPlayerPawn();
		float Best = TNumericLimits<float>::Max(); bool bFound = false;
		for (const FMissionCondition& K : C.Children)
		{
			FVector V;
			if (ConditionTarget(K, V))
			{
				const float D = P ? FVector::DistSquared(V, P->GetActorLocation()) : 0.f;
				if (D < Best) { Best = D; Out = V; bFound = true; }
			}
		}
		return bFound;
	}
	default: return false;
	}
}

bool UMissionSubsystem::StageTarget(FVector& Out) const
{
	if (!Mission.IsValid() || StageIndex >= Mission->Stages.Num()) { return false; }
	const FMissionStage& St = Mission->Stages[StageIndex];
	if (St.BlipMode == FMissionStage::EBlip::Explicit) { Out = St.BlipLocation; return true; }
	if (St.BlipMode == FMissionStage::EBlip::None) { return false; }
	return ConditionTarget(St.Complete, Out);
}

bool UMissionSubsystem::GotoStageTarget()
{
	FVector V;
	if (!StageTarget(V)) { UE_LOG(LogBullyMission, Warning, TEXT("BR_MISSION goto: current stage has no positional target")); return false; }
	TeleportPlayer(V + FVector(0, 0, 100), 0.f, false);
	UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION goto stage %d target %s"), StageIndex + 1, *V.ToCompactString());
	return true;
}

// ---------------------------------------------------------------------------------------------------------------
// triggers

void UMissionSubsystem::RegisterTrigger(AMissionTrigger* T)
{
	if (T) { Triggers.Add(T->TriggerId, T); }
}

void UMissionSubsystem::UnregisterTrigger(AMissionTrigger* T)
{
	if (!T) { return; }
	const TWeakObjectPtr<AMissionTrigger>* E = Triggers.Find(T->TriggerId);
	if (E && E->Get() == T) { Triggers.Remove(T->TriggerId); }
}

void UMissionSubsystem::NotifyTrigger(AMissionTrigger* T, bool bEntered)
{
	if (!T) { return; }
	const FName Ev(*FString::Printf(TEXT("trigger_%s:%s"), bEntered ? TEXT("enter") : TEXT("exit"), *T->TriggerId.ToString()));
	StageEvents.Add(Ev);
	MissionEvents.Add(Ev);
	UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION trigger '%s' %s"), *T->TriggerId.ToString(), bEntered ? TEXT("entered") : TEXT("exited"));
	OnMissionEvent.Broadcast(Ev);
}

void UMissionSubsystem::SpawnTriggers()
{
	UWorld* World = GetWorld();
	for (const FMissionTriggerDef& D : Mission->Triggers)
	{
		if (const TWeakObjectPtr<AMissionTrigger>* Existing = Triggers.Find(D.Id); Existing && Existing->IsValid()) { continue; }   // hand-placed wins
		FActorSpawnParameters Params;
		AMissionTrigger* T = World->SpawnActorDeferred<AMissionTrigger>(AMissionTrigger::StaticClass(), FTransform(FRotator(0.f, D.Yaw, 0.f), D.Center));
		if (!T) { continue; }
		T->TriggerId = D.Id;
		T->bInteract = D.bInteract;
		T->Prompt = D.Prompt;
		T->Box->SetBoxExtent(D.HalfExtent);
		T->FinishSpawning(FTransform(FRotator(0.f, D.Yaw, 0.f), D.Center));
		OwnedTriggers.Add(T);
	}
	UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION spawned %d trigger volumes (%d registered)"), OwnedTriggers.Num(), Triggers.Num());
}

void UMissionSubsystem::DestroyOwnedTriggers()
{
	for (const TWeakObjectPtr<AMissionTrigger>& T : OwnedTriggers)
	{
		if (T.IsValid()) { T->Destroy(); }
	}
	OwnedTriggers.Reset();
}

// ---------------------------------------------------------------------------------------------------------------
// HUD + tick

void UMissionSubsystem::EnsureHud()
{
	if (!Hud.IsValid()) { Hud = MakeShared<FBullyHUD>(); }
	Hud->Show(GetWorld());
}

void UMissionSubsystem::RefreshHudPerFrame(float Dt)
{
	if (!Hud.IsValid() || !Mission.IsValid()) { return; }
	// keep the wallet readout live (no flash — reward actions do the flash themselves)
	if (UBullyProgressSubsystem* Prog = UBullyProgressSubsystem::Get(GetWorld()))
	{
		Hud->SetWallet(Prog->GetMoney(), Prog->GetRespect(), false);
	}
	if (State == EState::Running)
	{
		FVector V;
		if (StageTarget(V)) { Hud->SetBlip(true, V); } else { Hud->SetBlip(false); }

		// button prompt: first interact condition of this stage whose target is in range
		const FMissionStage& St = Mission->Stages[StageIndex];
		TArray<const FMissionCondition*> Interacts;
		CollectInteracts(St.Complete, Interacts);
		FString Prompt;
		for (const FMissionCondition* C : Interacts)
		{
			if (!InteractInRange(*C) || StageEvents.Contains(FName(*FString::Printf(TEXT("interact:%s"), *C->Name.ToString())))) { continue; }
			Prompt = C->Prompt;
			if (Prompt.IsEmpty())
			{
				const TWeakObjectPtr<AMissionTrigger>* T = Triggers.Find(C->Name);
				if (T && T->IsValid()) { Prompt = (*T)->Prompt; }
			}
			if (Prompt.IsEmpty()) { Prompt = TEXT("Interact"); }
			break;
		}
		Hud->SetPrompt(Prompt);
	}
}

void UMissionSubsystem::Tick(float Dt)
{
	UWorld* World = GetWorld();
	if (!World) { return; }

	if (!PendingAutoStart.IsEmpty() && World->GetTimeSeconds() >= AutoStartAt && GetPlayerPawn())
	{
		const FString Id = PendingAutoStart;
		PendingAutoStart.Reset();
		StartMission(Id);
	}
	if (Smoke.bEnabled) { TickSmoke(Dt); }
	TickFade(Dt);
	if (Hud.IsValid()) { Hud->Tick(Dt); }

	if (State == EState::Running && Mission.IsValid())
	{
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			if (PC->WasInputKeyJustPressed(EKeys::E) || PC->WasInputKeyJustPressed(EKeys::Gamepad_FaceButton_Top)) { DoInteract(); }
		}
		RefreshHudPerFrame(Dt);
		const FMissionStage& St = Mission->Stages[StageIndex];
		if (St.Fail.IsValid() && Evaluate(St.Fail)) { FailMission(TEXT("objective failed")); }
		else if (Evaluate(St.Complete)) { CompleteStage(); }
	}
	else if (State == EState::Failed && Mission.IsValid())
	{
		if (World->GetTimeSeconds() - FailTime >= Mission->FailDelay) { RetryNow(); }
	}
	else if (State == EState::Complete && Hud.IsValid() && Hud->IsShown() && Hud->GetState().BannerLeft <= 0.f)
	{
		Hud->ClearObjective();
		Hud->Hide();
	}
}

void UMissionSubsystem::LogStatus() const
{
	if (!Mission.IsValid() || State == EState::Idle) { UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION status: idle")); return; }
	const FMissionStage& St = Mission->Stages[FMath::Clamp(StageIndex, 0, Mission->Stages.Num() - 1)];
	FVector T = FVector::ZeroVector; const bool bT = StageTarget(T);
	UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION status: %s state=%d stage %d/%d '%s' attempt=%d target=%s flags=%d"),
		*Mission->Id, int32(State), StageIndex + 1, Mission->Stages.Num(), *St.Objective, Attempt, bT ? *T.ToCompactString() : TEXT("none"), Flags.Num());
}

// ---------------------------------------------------------------------------------------------------------------
// smoke harness

void UMissionSubsystem::TickSmoke(float Dt)
{
	UWorld* World = GetWorld();
	if (Smoke.bDone) { return; }
	Smoke.Elapsed += Dt;
	auto Quit = [&](bool bPass, const FString& Why)
	{
		Smoke.bDone = true;
		UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION_SMOKE %s: %s (stages seen %d/%d, attempts %d, t=%.1fs)"), bPass ? TEXT("PASS") : TEXT("FAIL"), *Why,
			Smoke.StagesSeen.Num(), Mission.IsValid() ? Mission->Stages.Num() : 0, Attempt, Smoke.Elapsed);
		GEngine->Exec(World, TEXT("quit"));
	};

	if (!Smoke.bStarted)
	{
		if (Smoke.Elapsed > 3.0 && GetPlayerPawn())
		{
			Smoke.bStarted = true;
			if (!StartMission(Smoke.MissionId)) { Quit(false, TEXT("mission failed to load")); return; }
			// Only expect a fail/retry exercise if this mission actually defines a smoke_fail stage (day_one does;
			// a plain errand mission need not). Keeps the harness usable for any mission.
			Smoke.bWantFailTest = false;
			if (Mission.IsValid())
			{
				for (const FMissionStage& St : Mission->Stages)
				{
					if (St.Fail.Type == FMissionCondition::EType::Event && St.Fail.Name == FName("smoke_fail")) { Smoke.bWantFailTest = true; break; }
				}
			}
		}
		return;
	}
	if (Smoke.Elapsed > 150.0) { Quit(false, TEXT("timeout")); return; }
	if (State == EState::Complete)
	{
		const bool bAll = Mission.IsValid() && Smoke.StagesSeen.Num() == Mission->Stages.Num();
		Quit(bAll && (!Smoke.bWantFailTest || Smoke.bFailTested) && (!Smoke.bWantFailTest || Attempt >= 2), bAll ? TEXT("mission completed") : TEXT("completed without visiting every stage"));
		return;
	}
	if (State != EState::Running || !Mission.IsValid()) { return; }

	const FMissionStage& St = Mission->Stages[StageIndex];
	if (StageIndex != Smoke.LastStage)
	{
		Smoke.LastStage = StageIndex;
		Smoke.StagesSeen.AddUnique(StageIndex);
		Smoke.LastDrive = Smoke.Elapsed;   // first drive 1.5 s after the stage begins
		Smoke.InsideSince = -1.0;
	}

	// exercise the fail -> retry path once: a stage whose fail condition is the event "smoke_fail"
	if (Smoke.bWantFailTest && !Smoke.bFailTested && St.Fail.Type == FMissionCondition::EType::Event && St.Fail.Name == FName("smoke_fail") && Smoke.Elapsed - Smoke.LastDrive > 0.6)
	{
		Smoke.bFailTested = true;
		UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION_SMOKE firing smoke_fail on stage %d to exercise fail/retry"), StageIndex + 1);
		FireEvent(St.Fail.Name);
		return;
	}

	if (Smoke.Elapsed - Smoke.LastDrive < 1.5) { return; }
	TArray<const FMissionCondition*> Interacts;
	CollectInteracts(St.Complete, Interacts);
	if (Interacts.Num() > 0 && InteractInRange(*Interacts[0]))
	{
		if (Smoke.InsideSince < 0.0) { Smoke.InsideSince = Smoke.Elapsed; }
		if (Smoke.Elapsed - Smoke.InsideSince > 0.4) { UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION_SMOKE pressing interact (stage %d, prompt shown: '%s')"), StageIndex + 1, Hud.IsValid() ? *Hud->GetState().PromptText : TEXT("-")); DoInteract(); }
		return;
	}
	if (St.Complete.Type == FMissionCondition::EType::Event) { Smoke.LastDrive = Smoke.Elapsed; FireEvent(St.Complete.Name); return; }
	FVector Target;
	if (ConditionTarget(St.Complete, Target))
	{
		Smoke.LastDrive = Smoke.Elapsed;
		UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION_SMOKE teleport player to stage %d target %s"), StageIndex + 1, *Target.ToCompactString());
		TeleportPlayer(Target + FVector(0, 0, 100), 0.f, false);
	}
}

// ---------------------------------------------------------------------------------------------------------------
// console

namespace
{
	UMissionSubsystem* SysFor(UWorld* W)
	{
		UMissionSubsystem* S = UMissionSubsystem::Get(W);
		if (!S) { UE_LOG(LogBullyMission, Warning, TEXT("BR_MISSION: no mission subsystem in this world")); }
		return S;
	}

	FAutoConsoleCommandWithWorldAndArgs GStart(TEXT("Mission.Start"), TEXT("Mission.Start <id> [stage#]: load Config/Missions/<id>.json and begin"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& A, UWorld* W)
		{
			if (UMissionSubsystem* S = SysFor(W)) { if (A.Num() >= 1) { S->StartMission(A[0], A.Num() >= 2 ? FCString::Atoi(*A[1]) - 1 : 0); } else { UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION usage: Mission.Start <id> [stage#]")); } }
		}));
	FAutoConsoleCommandWithWorldAndArgs GStop(TEXT("Mission.Stop"), TEXT("Stop the active mission"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* W) { if (UMissionSubsystem* S = SysFor(W)) { S->StopMission(TEXT("console")); } }));
	FAutoConsoleCommandWithWorldAndArgs GSkip(TEXT("Mission.Skip"), TEXT("Mission.Skip [next|stage#|stageId]: jump to a stage"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& A, UWorld* W) { if (UMissionSubsystem* S = SysFor(W)) { S->SkipToStage(A.Num() ? A[0] : TEXT("next")); } }));
	FAutoConsoleCommandWithWorldAndArgs GFire(TEXT("Mission.Fire"), TEXT("Mission.Fire <event>: fire a mission event"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& A, UWorld* W) { if (UMissionSubsystem* S = SysFor(W)) { if (A.Num()) { S->FireEvent(FName(*A[0])); } } }));
	FAutoConsoleCommandWithWorldAndArgs GFail(TEXT("Mission.Fail"), TEXT("Mission.Fail [reason]: fail the current stage (triggers retry)"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& A, UWorld* W) { if (UMissionSubsystem* S = SysFor(W)) { S->FailMission(A.Num() ? FString::Join(A, TEXT(" ")) : TEXT("console")); } }));
	FAutoConsoleCommandWithWorldAndArgs GInteract(TEXT("Mission.Interact"), TEXT("Press the interact button (as if E)"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* W) { if (UMissionSubsystem* S = SysFor(W)) { S->DoInteract(); } }));
	FAutoConsoleCommandWithWorldAndArgs GGoto(TEXT("Mission.Goto"), TEXT("Teleport the player onto the current stage's target"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* W) { if (UMissionSubsystem* S = SysFor(W)) { S->GotoStageTarget(); } }));
	FAutoConsoleCommandWithWorldAndArgs GStatus(TEXT("Mission.Status"), TEXT("Log the mission state"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* W) { if (UMissionSubsystem* S = SysFor(W)) { S->LogStatus(); } }));
	FAutoConsoleCommandWithWorldAndArgs GList(TEXT("Mission.List"), TEXT("List Config/Missions/*.json"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld*)
		{
			for (const FString& Id : UMissionSubsystem::ListMissionIds()) { UE_LOG(LogBullyMission, Display, TEXT("BR_MISSION available: %s"), *Id); }
		}));
}
