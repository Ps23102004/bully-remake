// OWNER: slice-npc (scripted NPCs)
// UScriptedNPCSubsystem: loads Config/NPCScripts/*.json, spawns the groups on their cues, turns beats into NPC commands,
// listens to / emits mission events. Data format: docs/re-notes/scripted-npcs.md.
#include "BullyScriptedNPC.h"

#include "Algo/Reverse.h"
#include "AmbientSubsystem.h"
#include "BullyCharacter.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "MissionSubsystem.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogSNPCSys, Log, All);

namespace
{
	using FObj = TSharedPtr<FJsonObject>;
	using FVal = TSharedPtr<FJsonValue>;

	FString ScriptDir() { return FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("NPCScripts")); }

	bool ReadVec3(const FVal& V, FVector& Out)
	{
		if (!V.IsValid() || V->Type != EJson::Array) { return false; }
		const TArray<FVal>& A = V->AsArray();
		if (A.Num() < 3 || A[0]->Type != EJson::Number || A[1]->Type != EJson::Number || A[2]->Type != EJson::Number) { return false; }
		Out = FVector(A[0]->AsNumber(), A[1]->AsNumber(), A[2]->AsNumber());
		return true;
	}

	FVector GameToUe(const FVector& M) { return FVector(100.0 * M.X, -100.0 * M.Y, 100.0 * M.Z); }

	APawn* PlayerOf(UWorld* W) { return W ? UGameplayStatics::GetPlayerPawn(W, 0) : nullptr; }

	EBullyGait ParseGait(const FObj& O, EBullyGait Def)
	{
		FString G;
		if (O->TryGetStringField(TEXT("gait"), G))
		{
			G = G.ToLower();
			if (G == TEXT("run")) { return EBullyGait::Run; }
			if (G == TEXT("sprint")) { return EBullyGait::Sprint; }
			if (G == TEXT("walk")) { return EBullyGait::Walk; }
		}
		return Def;
	}

	TArray<FString> StringList(const FObj& O, const TCHAR* Key)
	{
		TArray<FString> Out;
		FString S;
		const TArray<FVal>* A = nullptr;
		if (O->TryGetArrayField(Key, A)) { for (const FVal& V : *A) { Out.Add(V->AsString()); } }
		else if (O->TryGetStringField(Key, S)) { Out.Add(S); }
		return Out;
	}

	double Num(const FObj& O, const TCHAR* Key, double Def)
	{
		double N;
		return O->TryGetNumberField(Key, N) ? N : Def;
	}

	bool BoolOf(const FObj& O, const TCHAR* Key, bool Def)
	{
		bool B;
		return O->TryGetBoolField(Key, B) ? B : Def;
	}
}

// ---------------------------------------------------------------------------------------------------------------
// lifecycle

UScriptedNPCSubsystem* UScriptedNPCSubsystem::Get(UWorld* World)
{
	return World ? World->GetSubsystem<UScriptedNPCSubsystem>() : nullptr;
}

bool UScriptedNPCSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer)) { return false; }
	const UWorld* W = Cast<UWorld>(Outer);
	if (!W || !W->IsGameWorld() || W->GetNetMode() == NM_DedicatedServer || FParse::Param(FCommandLine::Get(), TEXT("BRNoNPC"))) { return false; }
	return W->GetName().Contains(TEXT("Bullworth")) || FParse::Param(FCommandLine::Get(), TEXT("BRNPCAnyMap"));
}

TStatId UScriptedNPCSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UScriptedNPCSubsystem, STATGROUP_Tickables);
}

void UScriptedNPCSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	if (UMissionSubsystem* M = UMissionSubsystem::Get(&InWorld))
	{
		EvHandle = M->OnMissionEvent.AddUObject(this, &UScriptedNPCSubsystem::OnMissionEvent);
		StageHandle = M->OnStageBegan.AddUObject(this, &UScriptedNPCSubsystem::OnStageBegan);
		DoneHandle = M->OnMissionFinished.AddUObject(this, &UScriptedNPCSubsystem::OnMissionFinished);
		UE_LOG(LogSNPCSys, Display, TEXT("BR_SNPC hooked to the mission subsystem (events, stage begin, mission end)"));
	}
	else
	{
		UE_LOG(LogSNPCSys, Warning, TEXT("BR_SNPC no mission subsystem in this world: cues only from SNPC.Cue"));
	}
	FString Only;
	if (FParse::Value(FCommandLine::Get(), TEXT("-BRNPCScript="), Only, false))
	{
		TArray<FString> Ids;
		Only.ParseIntoArray(Ids, TEXT(","));
		for (const FString& Id : Ids) { LoadScript(Id); }
	}
	else
	{
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *FPaths::Combine(ScriptDir(), TEXT("*.json")), true, false);
		Files.Sort();
		for (const FString& F : Files)
		{
			if (!F.StartsWith(TEXT("_"))) { LoadScript(FPaths::GetBaseFilename(F)); }   // _test_* scripts load only when named
		}
	}
	FString SmokeList;
	if (FParse::Param(FCommandLine::Get(), TEXT("BRNPCSmoke")) || FParse::Value(FCommandLine::Get(), TEXT("-BRNPCSmoke="), SmokeList, false))
	{
		Smoke.Phases = SmokeList.IsEmpty() ? TEXT("stage,gauntlet,crowd") : SmokeList;
		Smoke.bEnabled = Smoke.Phases != TEXT("none");   // "none": load only (console-driven runs: -ExecCmds="SNPC.Cue ...")
		Smoke.StepStart = InWorld.GetTimeSeconds();
		if (FParse::Value(FCommandLine::Get(), TEXT("-BRNPCShotDir="), Smoke.ShotDir) && !Smoke.ShotDir.IsEmpty())
		{
			int32 V = 0;
			if (FParse::Value(FCommandLine::Get(), TEXT("-BRShotResX="), V) && V > 0) { Smoke.ShotResX = V; }
			if (FParse::Value(FCommandLine::Get(), TEXT("-BRShotResY="), V) && V > 0) { Smoke.ShotResY = V; }
			IFileManager::Get().MakeDirectory(*Smoke.ShotDir, true);
			UE_LOG(LogSNPCSys, Display, TEXT("BR_SNPC_SMOKE crowd capture armed -> %s at %dx%d"), *Smoke.ShotDir, Smoke.ShotResX, Smoke.ShotResY);
		}
		UE_LOG(LogSNPCSys, Display, TEXT("BR_SNPC_SMOKE harness armed: phases %s"), *Smoke.Phases);
	}
}

void UScriptedNPCSubsystem::Deinitialize()
{
	if (UMissionSubsystem* M = GetWorld() ? UMissionSubsystem::Get(GetWorld()) : nullptr)
	{
		M->OnMissionEvent.Remove(EvHandle);
		M->OnStageBegan.Remove(StageHandle);
		M->OnMissionFinished.Remove(DoneHandle);
	}
	Super::Deinitialize();
}

// ---------------------------------------------------------------------------------------------------------------
// loading

void UScriptedNPCSubsystem::LoadPointsFile(const FString& RepoRelPath)
{
	const FString Full = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../.."), RepoRelPath));
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Full))
	{
		UE_LOG(LogSNPCSys, Warning, TEXT("BR_SNPC trigger_file %s not present: only inline points are available (python3 -m tools.importers trigger)"), *Full);
		return;
	}
	FVal Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		UE_LOG(LogSNPCSys, Warning, TEXT("BR_SNPC trigger_file %s is not valid JSON"), *Full);
		return;
	}
	int32 Added = 0;
	TFunction<void(const FVal&)> Walk = [&](const FVal& V)
	{
		if (!V.IsValid()) { return; }
		if (V->Type == EJson::Array) { for (const FVal& E : V->AsArray()) { Walk(E); } return; }
		if (V->Type != EJson::Object) { return; }
		const FObj O = V->AsObject();
		FString Name, Type;
		if (O->TryGetStringField(TEXT("name"), Name) && O->TryGetStringField(TEXT("type"), Type))
		{
			const FName Key(*Name);
			if (!Points.Contains(Key))
			{
				TArray<FSNPCPoint> List;
				const TArray<FVal>* Sub = nullptr;
				const TCHAR* ListKey = Type == TEXT("path") ? TEXT("waypoints") : TEXT("points");
				if (Type != TEXT("trigger-volume") && O->TryGetArrayField(ListKey, Sub))
				{
					for (const FVal& P : *Sub)
					{
						const FObj PO = P->AsObject();
						FSNPCPoint Pt;
						if (PO.IsValid() && ReadVec3(PO->TryGetField(TEXT("location_cm")), Pt.Loc))
						{
							double Y;
							if (PO->TryGetNumberField(TEXT("yaw_deg_ue"), Y)) { Pt.YawUE = float(Y); Pt.bHasYaw = true; }
							List.Add(Pt);
						}
					}
				}
				if (List.Num() == 0)
				{
					FSNPCPoint Pt;
					if (ReadVec3(O->TryGetField(TEXT("center_cm")), Pt.Loc) || ReadVec3(O->TryGetField(TEXT("location_cm")), Pt.Loc))
					{
						double Y;
						if (O->TryGetNumberField(TEXT("yaw_deg_ue"), Y)) { Pt.YawUE = float(Y); Pt.bHasYaw = true; }
						List.Add(Pt);
					}
				}
				if (List.Num()) { Points.Add(Key, List); ++Added; }
			}
			return;   // entities do not nest
		}
		for (const auto& KV : O->Values) { Walk(KV.Value); }
	};
	Walk(Root);
	UE_LOG(LogSNPCSys, Display, TEXT("BR_SNPC trigger_file %s: %d named points/paths/triggers"), *Full, Added);
}

bool UScriptedNPCSubsystem::AddGroup(const FObj& G, FString& Err)
{
	FSNPCGroup Grp;
	if (!G->TryGetStringField(TEXT("id"), Grp.Id)) { Err = TEXT("group without \"id\""); return false; }
	Grp.EventPrefix = Grp.Id;
	G->TryGetStringField(TEXT("event_prefix"), Grp.EventPrefix);
	Grp.SpawnOn = StringList(G, TEXT("spawn_on"));
	Grp.DespawnOn = StringList(G, TEXT("despawn_on"));
	const TArray<FVal>* Npcs = nullptr;
	if (G->TryGetArrayField(TEXT("npcs"), Npcs))
	{
		for (const FVal& V : *Npcs)
		{
			const FObj O = V->AsObject();
			FSNPCNpcDef D;
			if (!O.IsValid() || !O->TryGetStringField(TEXT("id"), D.Id)) { Err = FString::Printf(TEXT("group %s: npc without \"id\""), *Grp.Id); return false; }
			O->TryGetStringField(TEXT("ped"), D.Ped);
			O->TryGetStringField(TEXT("footsteps"), D.Footsteps);
			O->TryGetStringField(TEXT("tag"), D.Tag);
			D.Spawn = O->TryGetField(TEXT("spawn"));
			D.Face = O->TryGetField(TEXT("face"));
			D.WalkSpeed = float(Num(O, TEXT("walk_speed_cm_s"), 0.0));
			if (!D.Spawn.IsValid()) { Err = FString::Printf(TEXT("npc %s has no \"spawn\""), *D.Id); return false; }
			Grp.Npcs.Add(D);
		}
	}
	const TArray<FVal>* Beats = nullptr;
	if (G->TryGetArrayField(TEXT("beats"), Beats))
	{
		for (const FVal& V : *Beats)
		{
			const FObj O = V->AsObject();
			FSNPCBeat B;
			if (!O.IsValid() || !O->TryGetStringField(TEXT("id"), B.Id)) { Err = FString::Printf(TEXT("group %s: beat without \"id\""), *Grp.Id); return false; }
			B.On = StringList(O, TEXT("on"));
			for (const FString& E : StringList(O, TEXT("emit"))) { B.Emit.Add(FName(*E)); }
			B.Delay = float(Num(O, TEXT("delay"), 0.0));
			B.bInterrupt = BoolOf(O, TEXT("interrupt"), false);
			B.bRepeat = BoolOf(O, TEXT("repeat"), false);
			const TArray<FVal>* Steps = nullptr;
			if (O->TryGetArrayField(TEXT("steps"), Steps)) { for (const FVal& S : *Steps) { if (S->AsObject().IsValid()) { B.Steps.Add(S->AsObject()); } } }
			Grp.Beats.Add(B);
		}
	}
	Groups.Add(MoveTemp(Grp));
	return true;
}

int32 UScriptedNPCSubsystem::LoadScript(const FString& Id)
{
	const FString Path = FPaths::Combine(ScriptDir(), Id + TEXT(".json"));
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		UE_LOG(LogSNPCSys, Warning, TEXT("BR_SNPC script %s not found"), *Path);
		return 0;
	}
	FObj Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		UE_LOG(LogSNPCSys, Error, TEXT("BR_SNPC script %s is not valid JSON"), *Path);
		return 0;
	}
	if (LoadedScripts.Contains(Id))
	{
		UE_LOG(LogSNPCSys, Display, TEXT("BR_SNPC script %s already loaded"), *Id);
		return 0;
	}
	FString S;
	if (Root->TryGetStringField(TEXT("trigger_file"), S)) { LoadPointsFile(S); }
	if (const FObj* Pts = nullptr; Root->TryGetObjectField(TEXT("points"), Pts))
	{
		for (const auto& KV : (*Pts)->Values)
		{
			TArray<FSNPCPoint> List;
			FVector V;
			if (ReadVec3(KV.Value, V)) { FSNPCPoint P; P.Loc = V; List.Add(P); }
			else if (KV.Value->Type == EJson::Array)
			{
				for (const FVal& E : KV.Value->AsArray()) { if (ReadVec3(E, V)) { FSNPCPoint P; P.Loc = V; List.Add(P); } }
			}
			else if (KV.Value->Type == EJson::Object)
			{
				const FObj PO = KV.Value->AsObject();
				FSNPCPoint P;
				FVector M;
				if (ReadVec3(PO->TryGetField(TEXT("location_cm")), P.Loc)) { List.Add(P); }
				else if (ReadVec3(PO->TryGetField(TEXT("game_m")), M)) { P.Loc = GameToUe(M); List.Add(P); }
			}
			if (List.Num()) { Points.Add(FName(*FString(KV.Key)), List); }
		}
	}
	if (const FObj* Gs = nullptr; Root->TryGetObjectField(TEXT("gestures"), Gs))
	{
		for (const auto& KV : (*Gs)->Values)
		{
			TArray<FString> L;
			if (KV.Value->Type == EJson::Array) { for (const FVal& E : KV.Value->AsArray()) { L.Add(E->AsString()); } }
			else { L.Add(KV.Value->AsString()); }
			Gestures.Add(FString(KV.Key), L);
		}
	}
	int32 Added = 0;
	const TArray<FVal>* Gr = nullptr;
	if (Root->TryGetArrayField(TEXT("groups"), Gr))
	{
		for (const FVal& V : *Gr)
		{
			FString Err;
			if (V->AsObject().IsValid() && AddGroup(V->AsObject(), Err)) { ++Added; }
			else { UE_LOG(LogSNPCSys, Error, TEXT("BR_SNPC script %s: %s"), *Id, *Err); }
		}
	}
	LoadedScripts.Add(Id);
	UE_LOG(LogSNPCSys, Display, TEXT("BR_SNPC script '%s' loaded: %d groups (%d total), %d gesture aliases, %d named points"), *Id, Added, Groups.Num(), Gestures.Num(), Points.Num());
	return Added;
}

// ---------------------------------------------------------------------------------------------------------------
// data resolution

bool UScriptedNPCSubsystem::ResolveLoc(const FVal& V, const FResolveCtx& C, FVector& Out, float* OutYaw, bool* bOutHasYaw) const
{
	if (bOutHasYaw) { *bOutHasYaw = false; }
	if (!V.IsValid()) { return false; }
	UWorld* W = GetWorld();
	auto FromPoint = [&](const FString& Name, int32 Idx, bool bWrap) -> bool
	{
		const TArray<FSNPCPoint>* L = Points.Find(FName(*Name));
		if (!L || L->Num() == 0) { UE_LOG(LogSNPCSys, Warning, TEXT("BR_SNPC unknown point '%s'"), *Name); return false; }
		if (bWrap) { Idx = Idx % L->Num(); }
		if (!L->IsValidIndex(Idx)) { UE_LOG(LogSNPCSys, Warning, TEXT("BR_SNPC point '%s' has %d entries, index %d requested"), *Name, L->Num(), Idx); return false; }
		Out = (*L)[Idx].Loc;
		if (OutYaw) { *OutYaw = (*L)[Idx].YawUE; }
		if (bOutHasYaw) { *bOutHasYaw = (*L)[Idx].bHasYaw; }
		return true;
	};
	if (V->Type == EJson::Array) { return ReadVec3(V, Out); }
	if (V->Type == EJson::String)
	{
		const FString S = V->AsString();
		if (S == TEXT("player")) { APawn* P = PlayerOf(W); if (P) { Out = P->GetActorLocation(); return true; } return false; }
		if (S == TEXT("self") && C.Npc) { Out = C.Npc->GetActorLocation(); return true; }
		return FromPoint(S, 0, false);
	}
	if (V->Type != EJson::Object) { return false; }
	const FObj O = V->AsObject();
	FString Name;
	if (O->TryGetStringField(TEXT("point"), Name))
	{
		int32 Idx = 0;
		bool bWrap = false;
		FString IdxS;
		if (O->TryGetStringField(TEXT("index"), IdxS)) { bWrap = true; Idx = IdxS.Contains(TEXT("$i")) ? C.Ordinal : FCString::Atoi(*IdxS); }
		else { Idx = int32(Num(O, TEXT("index"), 0.0)); }
		if (O->HasField(TEXT("index_offset"))) { Idx += int32(Num(O, TEXT("index_offset"), 0.0)); }
		return FromPoint(Name, Idx, bWrap);
	}
	FVector M;
	if (ReadVec3(O->TryGetField(TEXT("game_m")), M)) { Out = GameToUe(M); return true; }
	if (ReadVec3(O->TryGetField(TEXT("location_cm")), Out)) { return true; }
	if (BoolOf(O, TEXT("player"), false)) { APawn* P = PlayerOf(W); if (P) { Out = P->GetActorLocation(); return true; } return false; }
	FString NpcName;
	if (O->TryGetStringField(TEXT("npc"), NpcName))
	{
		ABullyScriptedNPC* N = FindNpc(NpcName);
		if (!N) { return false; }
		Out = N->GetActorLocation();
		return true;
	}
	const FObj* Ring = nullptr;
	if (O->TryGetObjectField(TEXT("ring"), Ring))
	{
		FVector Center;
		if (!ResolveLoc((*Ring)->TryGetField(TEXT("center")), C, Center)) { return false; }
		const double R = Num(*Ring, TEXT("radius_cm"), 500.0);
		const double Start = Num(*Ring, TEXT("start_deg"), 0.0);
		const double Arc = Num(*Ring, TEXT("arc_deg"), 360.0);
		const int32 N = FMath::Max(1, C.Count);
		const double Step = (Arc >= 359.9 || N == 1) ? Arc / N : Arc / (N - 1);
		const double Deg = Start + Step * C.Ordinal;
		Out = Center + FVector(FMath::Cos(FMath::DegreesToRadians(Deg)), FMath::Sin(FMath::DegreesToRadians(Deg)), 0.0) * R;
		return true;
	}
	const TArray<FVal>* Between = nullptr;
	if (O->TryGetArrayField(TEXT("between"), Between) && Between->Num() >= 2)
	{
		FVector A, B;
		if (!ResolveLoc((*Between)[0], C, A) || !ResolveLoc((*Between)[1], C, B)) { return false; }
		Out = FMath::Lerp(A, B, float(Num(O, TEXT("t"), 0.5)));
		return true;
	}
	FVector Off;
	if (ReadVec3(O->TryGetField(TEXT("offset_cm")), Off))
	{
		FVector Base = C.Npc ? C.Npc->GetActorLocation() : FVector::ZeroVector;
		if (O->HasField(TEXT("of")) && !ResolveLoc(O->TryGetField(TEXT("of")), C, Base)) { return false; }
		Out = Base + Off;
		return true;
	}
	return false;
}

bool UScriptedNPCSubsystem::ResolveLocs(const FVal& V, const FResolveCtx& C, TArray<FVector>& Out) const
{
	if (!V.IsValid()) { return false; }
	if (V->Type == EJson::Object)
	{
		const FObj O = V->AsObject();
		FString Name;
		if (O->TryGetStringField(TEXT("path"), Name) || (BoolOf(O, TEXT("all"), false) && O->TryGetStringField(TEXT("point"), Name)))
		{
			const TArray<FSNPCPoint>* L = Points.Find(FName(*Name));
			if (!L) { UE_LOG(LogSNPCSys, Warning, TEXT("BR_SNPC unknown path '%s'"), *Name); return false; }
			for (const FSNPCPoint& P : *L) { Out.Add(P.Loc); }
			if (BoolOf(O, TEXT("reverse"), false)) { Algo::Reverse(Out); }
			return Out.Num() > 0;
		}
		FVector P;
		if (ResolveLoc(V, C, P)) { Out.Add(P); return true; }
		return false;
	}
	if (V->Type == EJson::Array)
	{
		FVector P;
		if (ReadVec3(V, P)) { Out.Add(P); return true; }
		for (const FVal& E : V->AsArray())
		{
			if (E->Type == EJson::Object && (E->AsObject()->HasField(TEXT("path")) || BoolOf(E->AsObject(), TEXT("all"), false))) { ResolveLocs(E, C, Out); }
			else if (ResolveLoc(E, C, P)) { Out.Add(P); }
		}
		return Out.Num() > 0;
	}
	FVector P;
	if (ResolveLoc(V, C, P)) { Out.Add(P); return true; }
	return false;
}

void UScriptedNPCSubsystem::ResolveGestureAlias(const FString& Alias, int32 Ordinal, TArray<FString>& Out) const
{
	if (const TArray<FString>* L = Gestures.Find(Alias))
	{
		const int32 N = L->Num();
		for (int32 i = 0; i < N; ++i) { Out.Add((*L)[(i + Ordinal) % N]); }   // rotate: neighbours prefer different clips
	}
	else
	{
		Out.Add(Alias);   // not an alias: a literal clip name
	}
}

void UScriptedNPCSubsystem::ResolveClips(const FObj& O, const TCHAR* GestureKey, const TCHAR* ClipKey, TArray<FString>& Out, int32 Ordinal) const
{
	FString G;
	if (O->TryGetStringField(GestureKey, G)) { ResolveGestureAlias(G, Ordinal, Out); }
	for (const FString& C : StringList(O, ClipKey)) { Out.Add(C); }
}

static FString DescribeRef(const FVal& V)
{
	if (!V.IsValid()) { return TEXT("?"); }
	if (V->Type == EJson::String) { return V->AsString(); }
	if (V->Type == EJson::Object)
	{
		const FObj O = V->AsObject();
		FString N;
		if (O->TryGetStringField(TEXT("point"), N))
		{
			FString IdxS;
			if (O->TryGetStringField(TEXT("index"), IdxS)) { return FString::Printf(TEXT("%s[%s]"), *N, *IdxS); }
			return FString::Printf(TEXT("%s[%d]"), *N, int32(Num(O, TEXT("index"), 0.0)));
		}
		if (O->TryGetStringField(TEXT("path"), N)) { return N; }
		if (O->TryGetStringField(TEXT("npc"), N)) { return TEXT("npc:") + N; }
		if (O->HasField(TEXT("ring"))) { return TEXT("ring"); }
	}
	return TEXT("loc");
}

bool UScriptedNPCSubsystem::BuildCommand(const FObj& Step, const FResolveCtx& C, FSNPCCommand& Out, FString& Err) const
{
	FString Cmd;
	if (!Step->TryGetStringField(TEXT("cmd"), Cmd)) { Err = TEXT("step without \"cmd\""); return false; }
	Cmd = Cmd.ToLower();
	auto SetFaceTarget = [&](const FVal& T) -> bool
	{
		if (!T.IsValid()) { return false; }
		if (T->Type == EJson::String && T->AsString() == TEXT("player")) { Out.TargetKind = FSNPCCommand::ETarget::Player; return true; }
		if (T->Type == EJson::Object && T->AsObject()->HasField(TEXT("npc")))
		{
			ABullyScriptedNPC* N = FindNpc(T->AsObject()->GetStringField(TEXT("npc")));
			if (!N) { return false; }
			Out.TargetKind = FSNPCCommand::ETarget::Actor;
			Out.TargetActor = N;
			return true;
		}
		FVector P;
		if (!ResolveLoc(T, C, P)) { return false; }
		Out.TargetKind = FSNPCCommand::ETarget::Loc;
		Out.TargetLoc = P;
		return true;
	};
	if (Cmd == TEXT("goto"))
	{
		Out.Type = ESNPCCmd::GoTo;
		FVector P;
		if (!ResolveLoc(Step->TryGetField(TEXT("to")), C, P)) { Err = TEXT("goto: \"to\" does not resolve"); return false; }
		Out.Points = { P };
		Out.Gait = ParseGait(Step, EBullyGait::Walk);
		Out.Radius = float(Num(Step, TEXT("radius_cm"), 70.0));
		Out.Label = FString::Printf(TEXT("GoToPoint(%s)"), *DescribeRef(Step->TryGetField(TEXT("to"))));
	}
	else if (Cmd == TEXT("follow_path"))
	{
		Out.Type = ESNPCCmd::FollowPath;
		if (!ResolveLocs(Step->TryGetField(TEXT("path")), C, Out.Points)) { Err = TEXT("follow_path: \"path\" does not resolve"); return false; }
		Out.Gait = ParseGait(Step, EBullyGait::Walk);
		Out.Radius = float(Num(Step, TEXT("radius_cm"), 70.0));
		Out.Label = FString::Printf(TEXT("FollowPath(%s, %d points)"), *DescribeRef(Step->TryGetField(TEXT("path"))), Out.Points.Num());
	}
	else if (Cmd == TEXT("face"))
	{
		Out.Type = ESNPCCmd::Face;
		if (!SetFaceTarget(Step->TryGetField(TEXT("target")))) { Err = TEXT("face: \"target\" does not resolve"); return false; }
		Out.bTrack = BoolOf(Step, TEXT("track"), Out.TargetKind != FSNPCCommand::ETarget::Loc);
		Out.Label = FString::Printf(TEXT("%s(%s)"), Out.TargetKind == FSNPCCommand::ETarget::Loc ? TEXT("FaceLocation") : TEXT("FaceActor"), *DescribeRef(Step->TryGetField(TEXT("target"))));
	}
	else if (Cmd == TEXT("gesture"))
	{
		Out.Type = ESNPCCmd::Gesture;
		ResolveClips(Step, TEXT("gesture"), TEXT("clip"), Out.Clips, C.Ordinal);
		if (Out.Clips.Num() == 0) { Err = TEXT("gesture: needs \"gesture\" (alias) or \"clip\""); return false; }
		Out.bLoop = BoolOf(Step, TEXT("loop"), false);
		Out.Seconds = float(Num(Step, TEXT("seconds"), 0.0));
		Out.Rate = float(Num(Step, TEXT("rate"), 1.0));
		FString Alias;
		Out.Label = FString::Printf(TEXT("PlayGesture(%s%s)"), Step->TryGetStringField(TEXT("gesture"), Alias) ? *Alias : *Out.Clips[0], Out.bLoop ? TEXT(",loop") : TEXT(""));
	}
	else if (Cmd == TEXT("wait"))
	{
		Out.Type = ESNPCCmd::Wait;
		Out.Seconds = float(Num(Step, TEXT("seconds"), 1.0));
		Out.Label = FString::Printf(TEXT("Wait(%.1fs)"), Out.Seconds);
	}
	else if (Cmd == TEXT("block"))
	{
		Out.Type = ESNPCCmd::Block;
		FSNPCBlockSpec& B = Out.Block;
		if (Step->HasField(TEXT("at")))
		{
			if (!ResolveLoc(Step->TryGetField(TEXT("at")), C, B.At)) { Err = TEXT("block: \"at\" does not resolve"); return false; }
			B.bHasAt = true;
		}
		B.EngageRadius = float(Num(Step, TEXT("engage_radius_cm"), 450.0));
		B.AsideCm = float(Num(Step, TEXT("aside_cm"), 180.0));
		B.BodyRadiusCm = float(Num(Step, TEXT("body_radius_cm"), 0.0));
		if (Step->HasField(TEXT("guard")))
		{
			if (!ResolveLoc(Step->TryGetField(TEXT("guard")), C, B.Guard)) { Err = TEXT("block: \"guard\" does not resolve"); return false; }
			B.bGuard = true;
			B.MaxLateralCm = float(Num(Step, TEXT("max_lateral_cm"), 380.0));
		}
		B.AsideSide = int32(Num(Step, TEXT("aside_side"), 0.0));
		B.Until = StringList(Step, TEXT("until"));
		ResolveClips(Step, TEXT("engage_gesture"), TEXT("engage_clip"), B.EngageClips, C.Ordinal);
		ResolveClips(Step, TEXT("hold_gesture"), TEXT("hold_clip"), B.HoldClips, C.Ordinal);
		if (const FObj* Sh = nullptr; Step->TryGetObjectField(TEXT("shove"), Sh))
		{
			B.bShove = true;
			ResolveClips(*Sh, TEXT("gesture"), TEXT("clip"), B.ShoveClips, C.Ordinal);
			B.ShoveDelay = float(Num(*Sh, TEXT("delay"), B.ShoveDelay));
			B.ShoveInterval = float(Num(*Sh, TEXT("interval"), B.ShoveInterval));
			B.ShoveRadius = float(Num(*Sh, TEXT("radius_cm"), B.ShoveRadius));
			B.ShoveSpeed = float(Num(*Sh, TEXT("speed_cm_s"), B.ShoveSpeed));
			B.ShoveContact = float(Num(*Sh, TEXT("contact"), B.ShoveContact));
		}
		Out.Label = FString::Printf(TEXT("BlockPath(%s)"), Step->HasField(TEXT("at")) ? *DescribeRef(Step->TryGetField(TEXT("at"))) : TEXT("here"));
	}
	else if (Cmd == TEXT("disperse"))
	{
		Out.Type = ESNPCCmd::Disperse;
		Out.Gait = ParseGait(Step, EBullyGait::Walk);
		Out.DisperseDistance = float(Num(Step, TEXT("distance_cm"), 1800.0));
		Out.Radius = float(Num(Step, TEXT("radius_cm"), 120.0));
		Out.Seconds = float(Num(Step, TEXT("timeout"), 18.0));
		if (Step->HasField(TEXT("to"))) { ResolveLocs(Step->TryGetField(TEXT("to")), C, Out.Points); }
		if (Step->HasField(TEXT("from"))) { if (!SetFaceTarget(Step->TryGetField(TEXT("from")))) { Err = TEXT("disperse: \"from\" does not resolve"); return false; } }
		else { Out.TargetKind = FSNPCCommand::ETarget::Player; }
		FString Then = TEXT("despawn");
		Step->TryGetStringField(TEXT("then"), Then);
		Out.Then = Then == TEXT("ambient") ? FSNPCCommand::EThen::Ambient : Then == TEXT("stay") ? FSNPCCommand::EThen::Stay : FSNPCCommand::EThen::Despawn;
		Out.Label = FString::Printf(TEXT("Disperse(%s)"), *Then);
	}
	else if (Cmd == TEXT("despawn"))
	{
		Out.Type = ESNPCCmd::Despawn;
		Out.Label = TEXT("Despawn");
	}
	else if (Cmd == TEXT("ambient"))
	{
		Out.Type = ESNPCCmd::MakeAmbient;
		ResolveLocs(Step->TryGetField(TEXT("path")), C, Out.Points);
		Out.Label = FString::Printf(TEXT("MakeAmbient(%d-point loop)"), Out.Points.Num());
	}
	else { Err = FString::Printf(TEXT("unknown cmd '%s'"), *Cmd); return false; }
	return true;
}

// ---------------------------------------------------------------------------------------------------------------
// groups, spawning

const FSNPCGroup* UScriptedNPCSubsystem::FindGroup(const FString& GroupId) const
{
	for (const FSNPCGroup& G : Groups) { if (G.Id == GroupId) { return &G; } }
	return nullptr;
}

ABullyScriptedNPC* UScriptedNPCSubsystem::FindNpc(const FString& NpcId) const
{
	for (const TWeakObjectPtr<ABullyScriptedNPC>& N : AllNpcs) { if (N.IsValid() && N->NpcId == NpcId) { return N.Get(); } }
	return nullptr;
}

ABullyScriptedNPC* UScriptedNPCSubsystem::SpawnNpc(FSNPCGroup& Group, int32 GroupIdx, const FSNPCNpcDef& Def, int32 Ordinal)
{
	UWorld* W = GetWorld();
	FResolveCtx C;
	C.Ordinal = Ordinal;
	C.Count = Group.Npcs.Num();
	FVector At;
	float Yaw = 0.f;
	bool bHasYaw = false;
	if (!ResolveLoc(Def.Spawn, C, At, &Yaw, &bHasYaw))
	{
		UE_LOG(LogSNPCSys, Error, TEXT("BR_SNPC %s: spawn location does not resolve; skipped"), *Def.Id);
		return nullptr;
	}
	FVector FaceAt;
	if (Def.Face.IsValid() && ResolveLoc(Def.Face, C, FaceAt))
	{
		const FVector D = FaceAt - At;
		Yaw = FMath::RadiansToDegrees(FMath::Atan2(D.Y, D.X));
	}
	const FString MeshPath = FString::Printf(TEXT("/Game/Imported/Characters/%s/SK_%s.SK_%s"), *Def.Ped, *Def.Ped, *Def.Ped);
	ABullyScriptedNPC* N = W->SpawnActorDeferred<ABullyScriptedNPC>(ABullyScriptedNPC::StaticClass(),
		FTransform(FRotator(0.f, Yaw, 0.f), At + FVector(0, 0, 90.f)), nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!N) { return nullptr; }
	N->MeshPath = MeshPath;
	N->NpcId = Def.Id;
	N->GroupId = Group.Id;
	N->Ordinal = Ordinal;
	N->StudentId = Def.Id;
	N->LoopId = Group.Id;
	if (Def.WalkSpeed > 0.f) { N->WalkSpeed = Def.WalkSpeed; }
	N->FinishSpawning(N->GetActorTransform());
	N->OnCommandDone.AddUObject(this, &UScriptedNPCSubsystem::OnNpcCommandDone);
	N->OnNpcEvent.AddUObject(this, &UScriptedNPCSubsystem::OnNpcEvent);
	if (UAmbientSubsystem* A = W->GetSubsystem<UAmbientSubsystem>()) { A->AttachFootsteps(N, Def.Footsteps.IsEmpty() ? TEXT("shoe") : Def.Footsteps, Def.Id); }
	Group.Spawned.Add(N);
	AllNpcs.Add(N);
	return N;
}

bool UScriptedNPCSubsystem::SpawnGroup(const FString& GroupId, const FString& Why)
{
	UWorld* W = GetWorld();
	for (int32 g = 0; g < Groups.Num(); ++g)
	{
		FSNPCGroup& G = Groups[g];
		if (G.Id != GroupId) { continue; }
		if (G.bActive) { DespawnGroup(G.Id, TEXT("respawn")); }
		for (FSNPCBeat& B : G.Beats) { B.bFired = B.bDone = false; B.Pending = 0; B.ScheduledAt = -1.0; }
		G.Once.Reset();
		G.Spawned.Reset();
		G.bActive = true;
		G.SpawnTime = W->GetTimeSeconds();
		int32 N = 0;
		for (int32 i = 0; i < G.Npcs.Num(); ++i) { if (SpawnNpc(G, g, G.Npcs[i], i)) { ++N; } }
		UE_LOG(LogSNPCSys, Display, TEXT("BR_SNPC group %s: spawned %d of %d NPCs (cue: %s) t=%.2f"), *G.Id, N, G.Npcs.Num(), *Why, G.SpawnTime);
		FireEvent(FName(*(G.EventPrefix + TEXT("_spawned"))));
		DispatchGroup(g, TEXT("spawn"));
		return true;
	}
	UE_LOG(LogSNPCSys, Warning, TEXT("BR_SNPC group '%s' not found"), *GroupId);
	return false;
}

void UScriptedNPCSubsystem::DespawnGroup(const FString& GroupId, const FString& Why)
{
	for (FSNPCGroup& G : Groups)
	{
		if (G.Id != GroupId || !G.bActive) { continue; }
		int32 N = 0;
		for (const TWeakObjectPtr<ABullyScriptedNPC>& P : G.Spawned) { if (ABullyScriptedNPC* NPC = P.Get()) { NPC->Destroy(); ++N; } }
		G.Spawned.Reset();
		G.bActive = false;
		UE_LOG(LogSNPCSys, Display, TEXT("BR_SNPC group %s: despawned %d NPCs (%s)"), *G.Id, N, *Why);
		FireEvent(FName(*(G.EventPrefix + TEXT("_despawned"))));
	}
}

void UScriptedNPCSubsystem::DespawnAll()
{
	for (const FSNPCGroup& G : Groups) { if (G.bActive) { DespawnGroup(G.Id, TEXT("clear")); } }
}

// ---------------------------------------------------------------------------------------------------------------
// events in / out

bool UScriptedNPCSubsystem::CueMatches(const FString& Pattern, const FString& Cue) const
{
	if (Pattern.EndsWith(TEXT("*"))) { return Cue.StartsWith(Pattern.LeftChop(1)); }
	return Pattern == Cue;
}

bool UScriptedNPCSubsystem::GroupMatchesCue(const TArray<FString>& List, const FString& Cue) const
{
	for (const FString& P : List) { if (CueMatches(P, Cue)) { return true; } }
	return false;
}

void UScriptedNPCSubsystem::OnMissionEvent(FName Event)
{
	Dispatch(TEXT("event:") + Event.ToString());
}

void UScriptedNPCSubsystem::OnStageBegan(const FMissionDef& M, int32 StageIndex)
{
	const FString StageId = M.Stages.IsValidIndex(StageIndex) ? M.Stages[StageIndex].Id.ToString() : FString();
	UE_LOG(LogSNPCSys, Display, TEXT("BR_SNPC mission stage began: %s stage %d (%s)"), *M.Id, StageIndex + 1, *StageId);
	if (StageIndex == 0) { Dispatch(FString::Printf(TEXT("mission_start:%s"), *M.Id)); }
	Dispatch(FString::Printf(TEXT("stage_begin:%s:%s"), *M.Id, *StageId));
	Dispatch(FString::Printf(TEXT("stage_begin:%s:#%d"), *M.Id, StageIndex + 1));
}

void UScriptedNPCSubsystem::OnMissionFinished(const FMissionDef& M, bool bSuccess)
{
	Dispatch(FString::Printf(TEXT("mission_end:%s:%s"), *M.Id, bSuccess ? TEXT("success") : TEXT("fail")));
}

void UScriptedNPCSubsystem::FireEvent(FName Event)
{
	UWorld* W = GetWorld();
	Emitted.Add(TPair<FName, double>(Event, W ? W->GetTimeSeconds() : 0.0));
	UE_LOG(LogSNPCSys, Display, TEXT("BR_SNPC EMIT mission event '%s' (t=%.2f)"), *Event.ToString(), W ? W->GetTimeSeconds() : 0.0);
	if (UMissionSubsystem* M = UMissionSubsystem::Get(W)) { M->FireEvent(Event); }   // loops back through OnMissionEvent -> Dispatch
	else { Dispatch(TEXT("event:") + Event.ToString()); }
}

void UScriptedNPCSubsystem::Dispatch(const FString& Cue)
{
	if (DispatchDepth > 8) { UE_LOG(LogSNPCSys, Warning, TEXT("BR_SNPC cue '%s' dropped: dispatch depth"), *Cue); return; }
	++DispatchDepth;
	// tell running blockers about plain events first ("event:NAME" release conditions)
	if (Cue.StartsWith(TEXT("event:")))
	{
		for (const TWeakObjectPtr<ABullyScriptedNPC>& N : AllNpcs) { if (N.IsValid()) { N->NotifyEvent(FName(*Cue.RightChop(6))); } }
	}
	for (int32 g = 0; g < Groups.Num(); ++g)
	{
		FSNPCGroup& G = Groups[g];
		if (GroupMatchesCue(G.SpawnOn, Cue))
		{
			UE_LOG(LogSNPCSys, Display, TEXT("BR_SNPC cue '%s' -> spawn group %s"), *Cue, *G.Id);
			SpawnGroup(G.Id, Cue);
			continue;
		}
		if (G.bActive && GroupMatchesCue(G.DespawnOn, Cue))
		{
			DespawnGroup(G.Id, Cue);
			continue;
		}
		if (G.bActive) { DispatchGroup(g, Cue); }
	}
	--DispatchDepth;
}

void UScriptedNPCSubsystem::DispatchGroup(int32 GroupIdx, const FString& Cue)
{
	FSNPCGroup& G = Groups[GroupIdx];
	for (int32 b = 0; b < G.Beats.Num(); ++b)
	{
		FSNPCBeat& B = G.Beats[b];
		if (B.bFired && !(B.bRepeat && B.bDone)) { continue; }
		if (GroupMatchesCue(B.On, Cue)) { FireBeat(GroupIdx, b, Cue); }
	}
}

void UScriptedNPCSubsystem::FireBeat(int32 GroupIdx, int32 BeatIdx, const FString& Cue)
{
	UWorld* W = GetWorld();
	FSNPCBeat& B = Groups[GroupIdx].Beats[BeatIdx];
	B.bFired = true;
	B.bDone = false;
	B.FiredAt = W->GetTimeSeconds();
	UE_LOG(LogSNPCSys, Display, TEXT("BR_SNPC beat %s/%s fired by '%s' (t=%.2f%s)"), *Groups[GroupIdx].Id, *B.Id, *Cue, B.FiredAt, B.Delay > 0.f ? *FString::Printf(TEXT(", +%.1fs delay"), B.Delay) : TEXT(""));
	if (B.Delay > 0.f) { B.ScheduledAt = B.FiredAt + B.Delay; return; }
	RunBeat(GroupIdx, BeatIdx);
}

TArray<ABullyScriptedNPC*> UScriptedNPCSubsystem::ResolveTargets(int32 GroupIdx, const FString& Who) const
{
	TArray<ABullyScriptedNPC*> Out;
	const FSNPCGroup& G = Groups[GroupIdx];
	TArray<FString> Ids;
	Who.ParseIntoArray(Ids, TEXT(","));
	for (const TWeakObjectPtr<ABullyScriptedNPC>& P : G.Spawned)
	{
		ABullyScriptedNPC* N = P.Get();
		if (!N) { continue; }
		for (FString Id : Ids)
		{
			Id.TrimStartAndEndInline();
			if (Id == TEXT("*") || Id == N->NpcId) { Out.Add(N); break; }
		}
	}
	return Out;
}

void UScriptedNPCSubsystem::RunBeat(int32 GroupIdx, int32 BeatIdx)
{
	FSNPCBeat& B = Groups[GroupIdx].Beats[BeatIdx];
	B.Pending = 0;
	B.bEnqueueing = true;
	B.ScheduledAt = -1.0;
	B.Interrupted.Reset();
	for (const FObj& Step : B.Steps) { EnqueueStep(GroupIdx, BeatIdx, Step); }
	B.bEnqueueing = false;
	BeatMaybeDone(GroupIdx, BeatIdx);
}

void UScriptedNPCSubsystem::EnqueueStep(int32 GroupIdx, int32 BeatIdx, const FObj& Step)
{
	FSNPCBeat& B = Groups[GroupIdx].Beats[BeatIdx];
	FString Who = TEXT("*");
	Step->TryGetStringField(TEXT("npc"), Who);
	const TArray<ABullyScriptedNPC*> Targets = ResolveTargets(GroupIdx, Who);
	if (Targets.Num() == 0)
	{
		UE_LOG(LogSNPCSys, Warning, TEXT("BR_SNPC beat %s/%s: step targets '%s' but no such live NPC"), *Groups[GroupIdx].Id, *B.Id, *Who);
		return;
	}
	const bool bInterrupt = B.bInterrupt || BoolOf(Step, TEXT("interrupt"), false);
	const float Delay = float(Num(Step, TEXT("delay"), 0.0));
	const float Stagger = float(Num(Step, TEXT("stagger"), 0.0));
	const float Jitter = float(Num(Step, TEXT("jitter"), 0.0));
	for (int32 k = 0; k < Targets.Num(); ++k)
	{
		ABullyScriptedNPC* N = Targets[k];
		FResolveCtx C;
		C.Ordinal = N->Ordinal;
		C.Count = Groups[GroupIdx].Npcs.Num();
		C.Npc = N;
		FSNPCCommand Cmd;
		FString Err;
		if (!BuildCommand(Step, C, Cmd, Err))
		{
			UE_LOG(LogSNPCSys, Error, TEXT("BR_SNPC beat %s/%s -> %s: %s"), *Groups[GroupIdx].Id, *B.Id, *N->NpcId, *Err);
			continue;
		}
		if (bInterrupt && !B.Interrupted.Contains(N)) { B.Interrupted.Add(N); N->ClearCommands(true); }   // once per NPC per beat
		const float Pre = Delay + Stagger * k + (Jitter > 0.f ? FMath::FRandRange(0.f, Jitter) : 0.f);
		if (Pre > 0.01f) { N->Wait(Pre, true); }
		Cmd.Token = GroupIdx * 1000 + BeatIdx;
		++B.Pending;
		N->Enqueue(MoveTemp(Cmd));
	}
}

void UScriptedNPCSubsystem::OnNpcCommandDone(ABullyScriptedNPC* Npc, const FSNPCCommand& Cmd, const FString& Result)
{
	if (Cmd.Token < 0) { return; }
	const int32 g = Cmd.Token / 1000, b = Cmd.Token % 1000;
	if (!Groups.IsValidIndex(g) || !Groups[g].Beats.IsValidIndex(b)) { return; }
	FSNPCBeat& B = Groups[g].Beats[b];
	B.Pending = FMath::Max(0, B.Pending - 1);
	BeatMaybeDone(g, b);
}

void UScriptedNPCSubsystem::BeatMaybeDone(int32 GroupIdx, int32 BeatIdx)
{
	FSNPCBeat& B = Groups[GroupIdx].Beats[BeatIdx];
	if (B.bEnqueueing || B.Pending > 0 || !B.bFired || B.bDone || B.ScheduledAt >= 0.0) { return; }
	B.bDone = true;
	UE_LOG(LogSNPCSys, Display, TEXT("BR_SNPC beat %s/%s -> done (%.1fs)"), *Groups[GroupIdx].Id, *B.Id, GetWorld()->GetTimeSeconds() - B.FiredAt);
	const FString Id = B.Id;
	const TArray<FName> Emit = B.Emit;
	if (B.bRepeat) { B.bFired = false; }
	for (const FName& E : Emit) { FireEvent(E); }
	DispatchGroup(GroupIdx, TEXT("beat_done:") + Id);
}

void UScriptedNPCSubsystem::OnNpcEvent(ABullyScriptedNPC* Npc, FName Event)
{
	for (FSNPCGroup& G : Groups)
	{
		if (G.Id != Npc->GroupId) { continue; }
		const FName Name(*(G.EventPrefix + TEXT("_") + Event.ToString()));
		if (Event == TEXT("shove") || !G.Once.Contains(Name))
		{
			G.Once.Add(Name);
			FireEvent(Name);
		}
	}
}

void UScriptedNPCSubsystem::TickCues(double Now)
{
	APawn* P = PlayerOf(GetWorld());
	for (int32 g = 0; g < Groups.Num(); ++g)
	{
		FSNPCGroup& G = Groups[g];
		if (!G.bActive) { continue; }
		for (int32 b = 0; b < G.Beats.Num(); ++b)
		{
			FSNPCBeat& B = G.Beats[b];
			if (B.ScheduledAt >= 0.0 && Now >= B.ScheduledAt) { RunBeat(g, b); continue; }
			if (B.bFired && !(B.bRepeat && B.bDone)) { continue; }
			for (const FString& On : B.On)
			{
				FString Key, Val;
				if (!On.Split(TEXT(":"), &Key, &Val)) { continue; }
				if (Key == TEXT("timer") && Now - G.SpawnTime >= FCString::Atod(*Val))
				{
					FireBeat(g, b, On);
					break;
				}
				if (Key == TEXT("near") && P)
				{
					FString Name = Val, Rad;
					float Radius = 600.f;
					if (Val.Split(TEXT(":"), &Name, &Rad)) { Radius = FCString::Atof(*Rad); }
					const TArray<FSNPCPoint>* L = Points.Find(FName(*Name));
					if (!L || L->Num() == 0) { continue; }
					if (FVector::Dist2D(P->GetActorLocation(), (*L)[0].Loc) <= Radius && FMath::Abs(P->GetActorLocation().Z - (*L)[0].Loc.Z) < 600.f)
					{
						FireBeat(g, b, On);
						break;
					}
				}
			}
		}
	}
}

void UScriptedNPCSubsystem::Tick(float Dt)
{
	Super::Tick(Dt);
	UWorld* W = GetWorld();
	if (!W) { return; }
	const double Now = W->GetTimeSeconds();
	TickCues(Now);
	// a group whose NPCs have all left on their own (Disperse then=despawn) is over: free it for the next cue
	for (FSNPCGroup& G : Groups)
	{
		if (!G.bActive || G.Spawned.Num() == 0 || Now - G.SpawnTime < 1.0) { continue; }
		bool bAny = false;
		for (const TWeakObjectPtr<ABullyScriptedNPC>& N : G.Spawned) { if (N.IsValid()) { bAny = true; break; } }
		if (!bAny)
		{
			G.bActive = false;
			G.Spawned.Reset();
			UE_LOG(LogSNPCSys, Display, TEXT("BR_SNPC group %s: every NPC has left -> inactive (t=%.2f)"), *G.Id, Now);
			FireEvent(FName(*(G.EventPrefix + TEXT("_despawned"))));
		}
	}
	if (Smoke.bEnabled) { TickSmoke(Dt); }
}

void UScriptedNPCSubsystem::LogStatus() const
{
	UE_LOG(LogSNPCSys, Display, TEXT("BR_SNPC status: %d scripts, %d groups, %d named points, %d gesture aliases, %d emitted events"), LoadedScripts.Num(), Groups.Num(), Points.Num(), Gestures.Num(), Emitted.Num());
	for (const FSNPCGroup& G : Groups)
	{
		UE_LOG(LogSNPCSys, Display, TEXT("BR_SNPC  group %s: %s, spawn_on [%s], %d NPCs, %d beats"), *G.Id, G.bActive ? TEXT("ACTIVE") : TEXT("idle"), *FString::Join(G.SpawnOn, TEXT(", ")), G.Npcs.Num(), G.Beats.Num());
		for (const FSNPCBeat& B : G.Beats)
		{
			UE_LOG(LogSNPCSys, Display, TEXT("BR_SNPC    beat %s: %s on [%s] pending %d"), *B.Id, B.bDone ? TEXT("done") : B.bFired ? TEXT("running") : TEXT("waiting"), *FString::Join(B.On, TEXT(", ")), B.Pending);
		}
		for (const TWeakObjectPtr<ABullyScriptedNPC>& N : G.Spawned) { if (N.IsValid()) { UE_LOG(LogSNPCSys, Display, TEXT("BR_SNPC    npc %s"), *N->DescribeState()); } }
	}
}

// ---------------------------------------------------------------------------------------------------------------
// quick console commands

void UScriptedNPCSubsystem::DoQuick(const TArray<FString>& A)
{
	if (A.Num() < 2) { UE_LOG(LogSNPCSys, Display, TEXT("BR_SNPC usage: SNPC.Do <npc> goto <x> <y> [z] | gesture <alias|CLIP> [loop] | face player | disperse [cm] | despawn")); return; }
	ABullyScriptedNPC* N = FindNpc(A[0]);
	if (!N) { UE_LOG(LogSNPCSys, Warning, TEXT("BR_SNPC no live NPC '%s'"), *A[0]); return; }
	const FString C = A[1].ToLower();
	if (C == TEXT("goto") && A.Num() >= 4)
	{
		N->GoToPoint(FVector(FCString::Atof(*A[2]), FCString::Atof(*A[3]), A.Num() > 4 ? FCString::Atof(*A[4]) : N->GetActorLocation().Z), EBullyGait::Walk, 70.f);
	}
	else if (C == TEXT("gesture") && A.Num() >= 3)
	{
		TArray<FString> Clips;
		ResolveGestureAlias(A[2], N->Ordinal, Clips);
		N->PlayGesture(Clips, A.Num() > 3 && A[3] == TEXT("loop"), 0.f);
	}
	else if (C == TEXT("face")) { N->FacePlayer(true); }
	else if (C == TEXT("disperse"))
	{
		FSNPCCommand D;
		D.TargetKind = FSNPCCommand::ETarget::Player;
		D.DisperseDistance = A.Num() > 2 ? FCString::Atof(*A[2]) : 1800.f;
		D.Radius = 120.f;
		D.Seconds = 18.f;
		N->Disperse(D);
	}
	else if (C == TEXT("despawn")) { N->Despawn(); }
	else { UE_LOG(LogSNPCSys, Warning, TEXT("BR_SNPC unknown quick command '%s'"), *C); }
}

namespace
{
	UScriptedNPCSubsystem* SysFor(UWorld* W)
	{
		UScriptedNPCSubsystem* S = UScriptedNPCSubsystem::Get(W);
		if (!S) { UE_LOG(LogSNPCSys, Warning, TEXT("BR_SNPC: no scripted-NPC subsystem in this world (Bullworth map only; -BRNPCAnyMap forces it)")); }
		return S;
	}

	FAutoConsoleCommandWithWorldAndArgs GLoad(TEXT("SNPC.Load"), TEXT("SNPC.Load <script id>: load Config/NPCScripts/<id>.json"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& A, UWorld* W) { if (UScriptedNPCSubsystem* S = SysFor(W)) { for (const FString& Id : A) { S->LoadScript(Id); } } }));
	FAutoConsoleCommandWithWorldAndArgs GCue(TEXT("SNPC.Cue"), TEXT("SNPC.Cue <event>: dispatch a mission-style event to the NPC scripts (also SNPC.Cue stage_begin:<mission>:<stage>)"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& A, UWorld* W)
		{
			if (UScriptedNPCSubsystem* S = SysFor(W)) { if (A.Num()) { S->Dispatch(A[0].Contains(TEXT(":")) && !A[0].StartsWith(TEXT("trigger_")) ? A[0] : TEXT("event:") + A[0]); } }
		}));
	FAutoConsoleCommandWithWorldAndArgs GSpawn(TEXT("SNPC.Spawn"), TEXT("SNPC.Spawn <group>: force-spawn a group"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& A, UWorld* W) { if (UScriptedNPCSubsystem* S = SysFor(W)) { if (A.Num()) { S->SpawnGroup(A[0], TEXT("console")); } } }));
	FAutoConsoleCommandWithWorldAndArgs GDespawn(TEXT("SNPC.Despawn"), TEXT("SNPC.Despawn [group]: remove a group (or all)"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& A, UWorld* W) { if (UScriptedNPCSubsystem* S = SysFor(W)) { if (A.Num()) { S->DespawnGroup(A[0], TEXT("console")); } else { S->DespawnAll(); } } }));
	FAutoConsoleCommandWithWorldAndArgs GStatus(TEXT("SNPC.Status"), TEXT("Log scripted-NPC groups, beats and NPC states"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* W) { if (UScriptedNPCSubsystem* S = SysFor(W)) { S->LogStatus(); } }));
	FAutoConsoleCommandWithWorldAndArgs GDo(TEXT("SNPC.Do"), TEXT("SNPC.Do <npc> goto x y [z] | gesture <alias|CLIP> [loop] | face player | disperse [cm] | despawn"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& A, UWorld* W) { if (UScriptedNPCSubsystem* S = SysFor(W)) { S->DoQuick(A); } }));
}
