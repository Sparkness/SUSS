﻿#include "SussBrainComponent.h"

#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "AIController.h"
#include "SussAction.h"
#include "SussBrainConfigAsset.h"
#include "SussCommon.h"
#include "SussGameSubsystem.h"
#include "SussPoolSubsystem.h"
#include "SussSettings.h"
#include "SussUtility.h"
#include "SussWorldSubsystem.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Perception/AIPerceptionComponent.h"
#include "Queries/SussPerceptionQueries.h"


// Sets default values for this component's properties
USussBrainComponent::USussBrainComponent(): bQueuedForUpdate(false),
                                            bWasPreventedFromUpdating(false),
                                            BrainConfigAsset(nullptr),
                                            DistanceCategory(ESussDistanceCategory::OutOfRange),
                                            CurrentUpdateInterval(0),
                                            CurrentActionResult(),
                                            PerceptionComp(nullptr)
{
	PrimaryComponentTick.bCanEverTick = false;

	// Disable ticking by default
	PrimaryComponentTick.SetTickFunctionEnable(false);
}

void USussBrainComponent::SetBrainConfig(const FSussBrainConfig& NewConfig)
{
	// Note that we don't do anything with the current action until we need to change our minds
	BrainConfig = NewConfig;
	BrainConfigChanged();
}

void USussBrainComponent::SetBrainConfigFromAsset(USussBrainConfigAsset* Asset)
{
	BrainConfig = Asset->BrainConfig;
	BrainConfigChanged();
}

void USussBrainComponent::BrainConfigChanged()
{
	if (GetOwner()->HasAuthority())
	{
		InitActions();
	}
}



// Called when the game starts
void USussBrainComponent::BeginPlay()
{
	Super::BeginPlay();

	if (auto AIController = GetAIController())
	{
		PerceptionComp = GetOwner()->FindComponentByClass<UAIPerceptionComponent>();
	}


	if (auto Settings = GetDefault<USussSettings>())
	{
		if (PerceptionComp && Settings->BrainUpdateOnPerceptionChanges)
		{
			PerceptionComp->OnPerceptionUpdated.AddDynamic(this, &USussBrainComponent::OnPerceptionUpdated);
		}
	}
}


void USussBrainComponent::StartLogic()
{
	Super::StartLogic();

	bIsLogicStopped = false;
	LogicStoppedReason = "";

	if (GetOwner()->HasAuthority())
	{
		UpdateDistanceCategory();

		if (IsValid(BrainConfigAsset))
		{
			if (BrainConfig.ActionDefs.Num() || BrainConfig.ActionSets.Num())
			{
				UE_LOG(LogSuss, Warning, TEXT("SUSS embedded BrainConfig is being overwritten by asset link on BeginPlay"))
			}
			SetBrainConfigFromAsset(BrainConfigAsset);
		}
		else
		{
			BrainConfigChanged();
		}

		if (BrainConfig.PreventBrainUpdateIfAnyTags.Num() > 0)
		{
			if (auto Pawn = GetPawn())
			{
				// Listen on gameplay tag changes
				if (auto ASC = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Pawn))
				{
					for (auto Tag : BrainConfig.PreventBrainUpdateIfAnyTags)
					{
						TagDelegates.Add(Tag, ASC->RegisterGameplayTagEvent(Tag).AddUObject(this, &USussBrainComponent::OnGameplayTagEvent));
					}
				}
			}
		}
	}
	
}

float USussBrainComponent::GetDistanceToAnyPlayer() const
{
	auto Pawn = GetPawn();
	if (IsValid(Pawn))
	{
		const auto World = GetWorld();
		const FVector OurPos = Pawn->GetActorLocation();
		float MinSqDistance = std::numeric_limits<float>::max();

		for (int i = 0; i < UGameplayStatics::GetNumPlayerControllers(World); ++i)
		{
			auto PlayerPawn = UGameplayStatics::GetPlayerPawn(World, i);
			if (IsValid(PlayerPawn))
			{
				MinSqDistance = FMath::Min(MinSqDistance, FVector::DistSquared(OurPos, PlayerPawn->GetActorLocation()));
			}
		}

		return FMath::Sqrt(MinSqDistance);
	}

	return std::numeric_limits<float>::max();
}

void USussBrainComponent::UpdateActionScoreAdjustments(float DeltaTime)
{
	// Slowly reduce current score at a rate determined by its last run score (which includes inertia)
	if (IsActionInProgress() && CurrentActionResult.Score > 0)
	{
		const auto& ActionDef = CombinedActionsByPriority[CurrentActionResult.ActionDefIndex];
		if (ActionDef.ScoreCooldownTime > 0)
		{
			auto& H = ActionHistory[CurrentActionResult.ActionDefIndex];
			const float Decay = H.LastRunScore * (DeltaTime / ActionDef.ScoreCooldownTime);
			CurrentActionResult.Score = FMath::Max(CurrentActionResult.Score - Decay, 0);
		}
		else
		{
			CurrentActionResult.Score = 0;
		}
	}

	// Deal with repetition penalties and temp score adjustments
	for (int i = 0; i < ActionHistory.Num(); ++i)
	{
		auto& H = ActionHistory[i];
		const auto& ActionDef = CombinedActionsByPriority[i];
		if (H.RepetitionPenalty > 0)
		{
			if (i != CurrentActionResult.ActionDefIndex)
			{
				if (ActionDef.RepetitionPenaltyCooldown > 0)
				{
					// Not the current action anymore, bleed repetition penalty away
					const float Decay = ActionDef.RepetitionPenalty * (DeltaTime / ActionDef.RepetitionPenaltyCooldown);
					H.RepetitionPenalty = FMath::Max(H.RepetitionPenalty - Decay, 0);
				}
				else
				{
					H.RepetitionPenalty = 0;
				}
			}
		}
		// temp adjusts can be + or -
		if (!FMath::IsNearlyZero(H.TempScoreAdjust) && !FMath::IsNearlyZero(H.TempScoreAdjustCooldownRate))
		{
			// temp adjusts always cool down
			// always move towards 0
			if (H.TempScoreAdjust > 0)
			{
				// positive
				H.TempScoreAdjust = FMath::Max(H.TempScoreAdjust - H.TempScoreAdjustCooldownRate * DeltaTime, 0.0f);
			}
			else
			{
				// negative
				H.TempScoreAdjust = FMath::Min(H.TempScoreAdjust + H.TempScoreAdjustCooldownRate * DeltaTime, 0.0f);
			}
		}
	}

}

void USussBrainComponent::UpdateDistanceCategory()
{
	const float Dist = GetDistanceToAnyPlayer();
	float NewInterval = 1.0f;
	
	if (const auto Settings = GetDefault<USussSettings>())
	{
		if (Dist <= Settings->NearAgentSettings.MaxDistance)
		{
			DistanceCategory = ESussDistanceCategory::Near;
			NewInterval = Settings->NearAgentSettings.BrainUpdateRequestIntervalSeconds;
		}
		else if (Dist <= Settings->MidRangeAgentSettings.MaxDistance)
		{
			DistanceCategory = ESussDistanceCategory::MidRange;
			NewInterval = Settings->MidRangeAgentSettings.BrainUpdateRequestIntervalSeconds;
		}
		else if (Dist <= Settings->FarAgentSettings.MaxDistance)
		{
			DistanceCategory = ESussDistanceCategory::Far;
			NewInterval = Settings->FarAgentSettings.BrainUpdateRequestIntervalSeconds;
		}
		else
		{
			DistanceCategory = ESussDistanceCategory::OutOfRange;
			NewInterval = Settings->OutOfBoundsDistanceCheckInterval;
		}
	}

	auto& TM = GetWorld()->GetTimerManager();

	if (!UpdateRequestTimer.IsValid() || NewInterval != CurrentUpdateInterval)
	{
		// Randomise the time that brains start their update to spread them out
		float Delay = FMath::RandRange(0.0f, NewInterval);
		TM.SetTimer(UpdateRequestTimer, this, &USussBrainComponent::TimerCallback, NewInterval, true, Delay);
		CurrentUpdateInterval = NewInterval;
	}

	// Just in case this somehow gets called while agent is paused
	if (IsPaused())
	{
		TM.PauseTimer(UpdateRequestTimer);
	}
}

void USussBrainComponent::StopLogic(const FString& Reason)
{
	Super::StopLogic(Reason);

	bIsLogicStopped = true;
	LogicStoppedReason = Reason;
	
	StopCurrentAction();
	if (UpdateRequestTimer.IsValid())
	{
		GetWorld()->GetTimerManager().ClearTimer(UpdateRequestTimer);
	}
	// Note: we could have already queued an update, so that will need to be handled on Update

	if (TagDelegates.Num() > 0)
	{
		if (const auto Pawn = GetPawn())
		{
			// Listen on gameplay tag changes
			if (const auto ASC = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Pawn))
			{
				for (const auto Pair : TagDelegates)
				{
					ASC->UnregisterGameplayTagEvent(Pair.Value, Pair.Key);
				}
			}
		}

		TagDelegates.Empty();
	}

}

void USussBrainComponent::RestartLogic()
{
	Super::RestartLogic();

	bIsLogicStopped = true;
	LogicStoppedReason = "";

	StopCurrentAction();
	UpdateDistanceCategory();
}

void USussBrainComponent::PauseLogic(const FString& Reason)
{
	Super::PauseLogic(Reason);
	
	bIsLogicStopped = true;
	LogicStoppedReason = Reason;

	if (UpdateRequestTimer.IsValid())
	{
		GetWorld()->GetTimerManager().PauseTimer(UpdateRequestTimer);
	}
}

EAILogicResuming::Type USussBrainComponent::ResumeLogic(const FString& Reason)
{
	auto Ret = Super::ResumeLogic(Reason);
	if (Ret != EAILogicResuming::RestartedInstead)
	{
		// restarted calls RestartLogic
		if (UpdateRequestTimer.IsValid())
		{
			GetWorld()->GetTimerManager().UnPauseTimer(UpdateRequestTimer);
		}

		bIsLogicStopped = false;
		LogicStoppedReason = "";

	}
	return Ret;
}

void USussBrainComponent::InitActions()
{
	// Collate all the actions from referenced action sets, and actions only on this instance
	CombinedActionsByPriority.Empty();
	for (auto ActionSet : BrainConfig.ActionSets)
	{
		// Guard against bad config
		if (IsValid(ActionSet))
		{
			for (auto& Action : ActionSet->GetActions())
			{
				CombinedActionsByPriority.Add(Action);
			}
		}
	}
	for (auto& Action : BrainConfig.ActionDefs)
	{
		CombinedActionsByPriority.Add(Action);
	}

	// Sort by ascending priority
	CombinedActionsByPriority.Sort([](const FSussActionDef& A, const FSussActionDef& B)
	{
		return A.Priority < B.Priority;
	});

	// Init history
	ActionHistory.SetNum(CombinedActionsByPriority.Num());
}

ESussActionChoiceMethod USussBrainComponent::GetActionChoiceMethod(int Priority, int& OutTopN) const
{
	for (auto& C : BrainConfig.PriorityGroupActionChoiceOverrides)
	{
		if (C.Priority == Priority)
		{
			OutTopN = C.ChoiceTopN;
			return C.ChoiceMethod;
		}
	}
	OutTopN = BrainConfig.ActionChoiceTopN;
	return BrainConfig.ActionChoiceMethod;
}

void USussBrainComponent::RequestUpdate()
{
	if (GetOwner()->HasAuthority())
	{
		QueueForUpdate();
	}
}

void USussBrainComponent::GetPerceptionInfo(TArray<FSussActorPerceptionInfo>& OutPerceptionInfo,
                                            bool bIncludeKnownButNotCurrent,
                                            bool bHostileOnly,
                                            TSubclassOf<UAISense> SenseClass,
                                            bool bSenseClassInclude) const
{
	if (IsValid(PerceptionComp))
	{
		const FAISenseID SenseID = SenseClass ? UAISense::GetSenseID(SenseClass) : FAISenseID::InvalidID();
		for (auto It = PerceptionComp->GetPerceptualDataConstIterator(); It; ++It)
		{
			const FActorPerceptionInfo& Info = It->Value;

			if (SenseClass)
			{
				if (bSenseClassInclude && !Info.HasKnownStimulusOfSense(SenseID))
					continue;
				if (!bSenseClassInclude && Info.HasKnownStimulusOfSense(SenseID))
					continue;
			}
			if (bHostileOnly && !Info.bIsHostile)
				continue;
			
			if (bIncludeKnownButNotCurrent || Info.HasAnyCurrentStimulus())
			{
				OutPerceptionInfo.Add(FSussActorPerceptionInfo(Info));
			}
		}
	}
}

TArray<FSussActorPerceptionInfo> USussBrainComponent::GetPerceptionInfo(bool bIncludeKnownButNotCurrent,
	bool bHostileOnly,
	TSubclassOf<UAISense> SenseClass,
	bool bSenseClassInclude)
{
	// BP-friendly version
	TArray<FSussActorPerceptionInfo> Ret;
	GetPerceptionInfo(Ret, bIncludeKnownButNotCurrent, bHostileOnly, SenseClass, bSenseClassInclude);
	return Ret;
}
UE_DISABLE_OPTIMIZATION
FSussActorPerceptionInfo USussBrainComponent::GetMostRecentPerceptionInfo(bool& bIsValid, bool bHostileOnly,
                                                                          TSubclassOf<UAISense> SenseClass,
                                                                          bool bSenseClassInclude)
{
	float BestAge = FAIStimulus::NeverHappenedAge;
	const FActorPerceptionInfo* BestInfo = nullptr;
	
	if (IsValid(PerceptionComp))
	{
		const FAISenseID SenseID = SenseClass ? UAISense::GetSenseID(SenseClass) : FAISenseID::InvalidID();
		for (auto It = PerceptionComp->GetPerceptualDataConstIterator(); It; ++It)
		{
			const FActorPerceptionInfo& Info = It->Value;

			if (SenseClass)
			{
				if (bSenseClassInclude && !Info.HasKnownStimulusOfSense(SenseID))
					continue;
				if (!bSenseClassInclude && Info.HasKnownStimulusOfSense(SenseID))
					continue;
			}
			if (bHostileOnly && !Info.bIsHostile)
				continue;

			for (auto& Stim : Info.LastSensedStimuli)
			{
				if (Stim.WasSuccessfullySensed() && !Stim.IsExpired() && Stim.GetAge() < BestAge)
				{
					BestAge = Stim.GetAge();
					BestInfo = &Info;
					// Don't break, incase even better aged stimulus in this result
				}
			}
		}
	}

	if (BestInfo)
	{
		bIsValid = true;
		return FSussActorPerceptionInfo(*BestInfo);
	}
	bIsValid = false;
	return FSussActorPerceptionInfo();
}
UE_ENABLE_OPTIMIZATION
bool USussBrainComponent::IsUpdatePrevented() const
{
	if (BrainConfig.PreventBrainUpdateIfAnyTags.Num() > 0)
	{
		if (auto Pawn = GetPawn())
		{
			// Listen on gameplay tag changes
			if (auto ASC = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Pawn))
			{
				if (ASC->HasAnyMatchingGameplayTags(BrainConfig.PreventBrainUpdateIfAnyTags))
				{
					return true;
				}
			}
		}
	}

	return false;
}

void USussBrainComponent::QueueForUpdate()
{
	if (!bQueuedForUpdate)
	{
		if (IsUpdatePrevented())
		{
			bWasPreventedFromUpdating = true;
		}
		else
		{
			if (auto SS = GetSussWorldSubsystem(GetWorld()))
			{
				SS->QueueBrainUpdate(this);
				bQueuedForUpdate = true;
				bWasPreventedFromUpdating = false;
			}
		}
	}
}

void USussBrainComponent::OnGameplayTagEvent(const FGameplayTag InTag, int32 NewCount)
{
	// By nature this has to be one of the brain config's prevent update tags
	// We don't need to check > 0 because that's checked on update
	// We just need to check if we need to immediately update
	if (NewCount == 0 && bWasPreventedFromUpdating)
	{
		// This will check for the presence of any blocking tags again
		QueueForUpdate();
	}
}

void USussBrainComponent::TimerCallback()
{
	UpdateActionScoreAdjustments(CurrentUpdateInterval);
	UpdateDistanceCategory();

	// We still get timer callbacks for being out of range, we simply check the distance
	if (DistanceCategory != ESussDistanceCategory::OutOfRange)
	{
		QueueForUpdate();
	}
}

void USussBrainComponent::ChooseActionFromCandidates()
{
	if (CandidateActions.IsEmpty())
	{
#if ENABLE_VISUAL_LOG
		UE_VLOG(GetLogOwner(), LogSuss, Log, TEXT("No candidate actions"));
		if (IsActionInProgress())
		{
			const FSussActionDef& CurrentActionDef =  CombinedActionsByPriority[CurrentActionResult.ActionDefIndex];
			UE_VLOG(GetLogOwner(),
			        LogSuss,
			        Log,
			        TEXT("No Action Change, continue: %s %s"),
			        CurrentActionDef.Description.IsEmpty() ? *CurrentActionDef.ActionTag.ToString() : *CurrentActionDef.
			        Description,
			        *CurrentActionResult.Context.ToString());
			CurrentActionResult.Context.VisualLog(GetLogOwner());

		}
#endif
		return;
	}

	CandidateActions.Sort([](const FSussActionScoringResult& L, const FSussActionScoringResult& R)
	{
		// sort from highest to lowest
		return L.Score > R.Score;
	});

	// All actions in the candidate list will always be from the same priority group
	const int Priority = CombinedActionsByPriority[CandidateActions[0].ActionDefIndex].Priority;
	int TopN = 0;
	ESussActionChoiceMethod ChoiceMethod = GetActionChoiceMethod(Priority, TopN);

	if (ChoiceMethod == ESussActionChoiceMethod::HighestScoring)
	{
#if ENABLE_VISUAL_LOG
		UE_VLOG(GetLogOwner(), LogSuss, Log, TEXT("Choice method: Highest Scoring"));
#endif
		ChooseAction(CandidateActions[0]);
	}
	else
	{
		// Weighted random of some kind
		float TotalScores = 0;
		int ChoiceCount = 0;
		const float BestScore = CandidateActions[0].Score;
		const float ScoreLimit = ChoiceMethod == ESussActionChoiceMethod::WeightedRandomTopNPercent ?
			BestScore - (BestScore * ((float)TopN / 100.0f)): 0;
		for (int i = 0; i < CandidateActions.Num(); ++i, ++ChoiceCount)
		{
			if (ChoiceMethod == ESussActionChoiceMethod::WeightedRandomTopN)
			{
				if (i == TopN)
				{
					break;
				}
			}
			else if (ChoiceMethod == ESussActionChoiceMethod::WeightedRandomTopNPercent)
			{
				if (CandidateActions[i].Score < ScoreLimit)
				{
					break;
				}
			}

			TotalScores += CandidateActions[i].Score;
		}

		const float Rand = FMath::RandRange(0.0f, TotalScores);
		float ScoreAccum = 0;
		for (int i = 0; i < ChoiceCount; ++i)
		{
			ScoreAccum += CandidateActions[i].Score;

			if (Rand < ScoreAccum)
			{
#if ENABLE_VISUAL_LOG
				UE_VLOG(GetLogOwner(), LogSuss, Log,
				        TEXT("Choice method: %s (%d) [%4.2f/%4.2f]"),
				        *StaticEnum<ESussActionChoiceMethod>()->GetDisplayNameTextByValue((int64)ChoiceMethod).ToString(),
					    TopN,
				        Rand,
				        TotalScores);
#endif
				ChooseAction(CandidateActions[i]);
				break;
			}
		}
	}
}

void USussBrainComponent::StopCurrentAction()
{
	CancelCurrentAction(nullptr);
}
void USussBrainComponent::CancelCurrentAction(TSubclassOf<USussAction> Interrupter)
{
	// Cancel previous action
	if (CurrentActionInstance.IsValid())
	{
		CurrentActionInstance->InternalOnActionCompleted.Unbind();
		CurrentActionInstance->CancelAction(Interrupter);
		RecordAndResetCurrentAction();
	}
}

void USussBrainComponent::RecordAndResetCurrentAction()
{
	auto& History = ActionHistory[CurrentActionResult.ActionDefIndex];
	History.LastEndTime = GetWorld()->GetTimeSeconds();
	// Repetition penalties are CUMULATIVE
	History.RepetitionPenalty += CombinedActionsByPriority[CurrentActionResult.ActionDefIndex].RepetitionPenalty;

	// This will free automatically
	CurrentActionInstance = nullptr;
	CurrentActionResult.ActionDefIndex = -1;
	CurrentActionResult.Score = 0;
}

bool USussBrainComponent::IsActionInProgress()
{
	return CurrentActionInstance != nullptr;
}

void USussBrainComponent::ChooseAction(const FSussActionScoringResult& ActionResult)
{
	checkf(ActionResult.ActionDefIndex >= 0, TEXT("No supplied action def"));

	const FSussActionDef& Def = CombinedActionsByPriority[ActionResult.ActionDefIndex];
	if (IsActionInProgress() && IsActionSameAsCurrent(ActionResult.ActionDefIndex, ActionResult.Context))
	{
		// We're already running it, so just continue
		// However, update the score in case we've decided again
		CurrentActionResult.Score = ActionResult.Score;
#if ENABLE_VISUAL_LOG
		UE_VLOG(GetLogOwner(), LogSuss, Log, TEXT("No Action Change, continue: %s %s"), Def.Description.IsEmpty() ? *Def.ActionTag.ToString() : *Def.Description, *ActionResult.Context.ToString());
		ActionResult.Context.VisualLog(GetLogOwner());
#endif
		CurrentActionInstance->ContinueAction(ActionResult.Context, Def.ActionParams);
		return;
	}

	auto SUSS = GetSUSS(GetWorld());
	const TSubclassOf<USussAction> ActionClass = SUSS->GetActionClass(Def.ActionTag);

#if ENABLE_VISUAL_LOG
	UE_VLOG(GetLogOwner(), LogSuss, Log, TEXT("Chose NEW action: %s %s"), Def.Description.IsEmpty() ? *Def.ActionTag.ToString() : *Def.Description, *ActionResult.Context.ToString());
		ActionResult.Context.VisualLog(GetLogOwner());
#endif

	TSubclassOf<USussAction> PreviousActionClass = nullptr;
	if (CurrentActionInstance.IsValid())
	{
		PreviousActionClass = CurrentActionInstance->GetClass();
	}
	StopCurrentAction();
	CurrentActionResult = ActionResult;

	// This is a new action, so we add inertia to the score now
	CurrentActionResult.Score += Def.Inertia;

	if (ActionClass)
	{
		// Record the start of the action
		auto& History = ActionHistory[ActionResult.ActionDefIndex];
		History.LastStartTime = GetWorld()->GetTimeSeconds();
		History.LastRunScore = ActionResult.Score;
		History.LastContext = ActionResult.Context;
		
		// Note that to allow BP classes we need to construct using the default object
		CurrentActionInstance = GetSussPool(GetWorld())->ReserveAction(ActionClass, ActionClass->GetDefaultObject());
		CurrentActionInstance->Init(this, ActionResult.Context, ActionResult.ActionDefIndex);
		CurrentActionInstance->InternalOnActionCompleted.BindUObject(this, &USussBrainComponent::OnActionCompleted);
		CurrentActionInstance->PerformAction(ActionResult.Context, Def.ActionParams, PreviousActionClass);
	}
	else
	{
		// No action class provided for this tag, do nothing
		CurrentActionInstance = nullptr;

		UE_LOG(LogSuss, Warning, TEXT("No action class for tag %s, so doing nothing"), *Def.ActionTag.ToString());
		
	}

	
}

void USussBrainComponent::OnActionCompleted(USussAction* SussAction)
{
	// Sometimes possible for actions to call us back late when we've already abandoned them, ignore that
	if (CurrentActionInstance.IsValid() && CurrentActionInstance.Get() == SussAction)
	{
#if ENABLE_VISUAL_LOG
		UE_VLOG(GetLogOwner(), LogSuss, Log, TEXT("Action completed: %s"), *SussAction->GetActionTag().ToString());
#endif
		
		SussAction->InternalOnActionCompleted.Unbind();
		RecordAndResetCurrentAction();
		// Immediately queue for update so no hesitation after completion
		QueueForUpdate();

	}

}

void USussBrainComponent::Update()
{
	bQueuedForUpdate = false;
	
	if (!GetOwner()->HasAuthority())
		return;

	OnPreBrainUpdate.Broadcast(this);

	// This is to catch updates called after StopLogic/PauseLogic because they were already queued
	if (bIsLogicStopped)
		return;

	if (CombinedActionsByPriority.IsEmpty())
		return;

	/// If we can't be interrupted, no need to check what else we could be doing
	if (CurrentActionInstance.IsValid() && !CurrentActionInstance->CanBeInterrupted())
		return;

#if ENABLE_VISUAL_LOG
	UE_VLOG(GetLogOwner(), LogSuss, Log, TEXT("Brain Update"));
#endif

	auto SUSS = GetSUSS(GetWorld());
	auto Pool = GetSussPool(GetWorld());
	AActor* Self = GetSelf();

	const FSussActionDef* CurrentActionDef = IsActionInProgress() ? &CombinedActionsByPriority[CurrentActionResult.ActionDefIndex] : nullptr;
	
	int CurrentPriority = CombinedActionsByPriority[0].Priority;
	// Use reset not empty in order to keep memory stable
	CandidateActions.Reset();
	bool bAddedCurrentAction = false;
	for (int i = 0; i < CombinedActionsByPriority.Num(); ++i)
	{
		const FSussActionDef& NextAction = CombinedActionsByPriority[i];

		if (CurrentActionInstance.IsValid() && CurrentActionInstance->AllowInterruptionsFromHigherPriorityGroupsOnly() && CurrentActionDef->Priority <= NextAction.Priority)
		{
			// Don't consider anything else of equal or lower priority
			break;
		}
		
		// Priority grouping - use the best option from the highest priority group first
		if (CurrentPriority != NextAction.Priority)
		{
			// End of priority group
			if (!CandidateActions.IsEmpty())
			{
				// OK we pick from these & don't consider the others
				break;
			}

			// Otherwise we had no candidates in that group, carry on to the next one
			CurrentPriority = NextAction.Priority;
		}

		// Ignore zero-weighted actions
		if (NextAction.Weight < UE_KINDA_SMALL_NUMBER)
			continue;

		// Ignore bad config or globally disabled actions
		if (!NextAction.ActionTag.IsValid() || !USussUtility::IsActionEnabled(NextAction.ActionTag))
			continue;

		// Check required/blocking tags on self
		if (NextAction.RequiredTags.Num() > 0 && !USussUtility::ActorHasAllTags(GetOwner(), NextAction.RequiredTags))
			continue;
		if (NextAction.BlockingTags.Num() > 0 && USussUtility::ActorHasAnyTags(GetOwner(), NextAction.BlockingTags))
			continue;

		auto ArrayPool = GetSussPool(GetWorld());
		
		FSussScopeReservedArray ContextsScope = ArrayPool->ReserveArray<FSussContext>();
		TArray<FSussContext>& Contexts = *ContextsScope.Get<FSussContext>();
		GenerateContexts(Self, NextAction, Contexts);

#if ENABLE_VISUAL_LOG
		UE_VLOG(GetLogOwner(), LogSuss, Log, TEXT("Action: %s  Priority: %d Weight: %4.2f Contexts: %d"),
			NextAction.Description.IsEmpty() ? *NextAction.ActionTag.ToString() : *NextAction.Description,
			NextAction.Priority,
			NextAction.Weight,
			Contexts.Num());
#endif
		
		// Evaluate this action for every applicable context
		for (const auto& Ctx : Contexts)
		{
#if ENABLE_VISUAL_LOG
			UE_VLOG(GetLogOwner(), LogSuss, Log, TEXT(" - %s"), *Ctx.ToString());
#endif
			float Score = NextAction.Weight;
			for (const auto& Consideration : NextAction.Considerations)
			{
				if (const auto InputProvider = SUSS->GetInputProvider(Consideration.InputTag))
				{
					// Resolve parameters
					FSussScopeReservedMap ResolvedQueryParamsScope = Pool->ReserveMap<FName, FSussParameter>();
					TMap<FName, FSussParameter>& ResolvedParams = *ResolvedQueryParamsScope.Get<FName, FSussParameter>();
					ResolveParameters(Self, Consideration.Parameters, ResolvedParams);

					const float RawInputValue = InputProvider->Evaluate(this, Ctx, ResolvedParams);

					// Normalise to bookends and clamp
					const float NormalisedInput = FMath::Clamp(FMath::GetRangePct(
						                                           ResolveParameter(
							                                           Ctx,
							                                           Consideration.BookendMin).FloatValue,
						                                           ResolveParameter(
							                                           Ctx,
							                                           Consideration.BookendMax).FloatValue,
						                                           RawInputValue),
					                                           0.f,
					                                           1.f);

					// Transform through curve
					const float ConScore = Consideration.EvaluateCurve(NormalisedInput);

#if ENABLE_VISUAL_LOG
					UE_VLOG(GetLogOwner(), LogSuss, Log, TEXT("  * Consideration: %s  Input: %4.2f  Normalised: %4.2f  Final: %4.2f"),
						Consideration.Description.IsEmpty() ? *Consideration.InputTag.ToString() : *Consideration.Description,
						RawInputValue, NormalisedInput, ConScore);
#endif

					// Accumulate with overall score
					Score *= ConScore;

					// Early-out if we've ended up at zero, nothing can change this now
					if (FMath::IsNearlyZero(Score))
					{
						break;
					}
					
				}
			}
			
			const bool bIsCurrentAction = IsActionSameAsCurrent(i, Ctx);			
			if (bIsCurrentAction)
			{
				// We preserve the previous score if better, which bleeds away over time
				// This is so that if an action is decided on with a given score (plus inertia), even if it's not in the
				// running anymore, we won't interrupt it without a much better option
				if (CurrentActionResult.Score > Score)
				{
#if ENABLE_VISUAL_LOG
					UE_VLOG(GetLogOwner(), LogSuss, Log, TEXT("  * Current Action Score upgrade from %4.2f to %4.2f"), Score, CurrentActionResult.Score);
#endif
					Score = CurrentActionResult.Score;
				}
			}

			const auto& Hist = ActionHistory[i];
			// Add repetition penalty if applicable
			if (ShouldSubtractRepetitionPenaltyToProposedAction(i, Ctx))
			{
				Score -= Hist.RepetitionPenalty;
#if ENABLE_VISUAL_LOG
				UE_VLOG(GetLogOwner(), LogSuss, Log, TEXT("  * Repetition Penalty: -%4.2f"), Hist.RepetitionPenalty);
#endif
			}
			if (!FMath::IsNearlyZero(Hist.TempScoreAdjust))
			{
				// Add temp adjustments
				Score += Hist.TempScoreAdjust;
#if ENABLE_VISUAL_LOG
				UE_VLOG(GetLogOwner(), LogSuss, Log, TEXT("  * Temp Adjust: %4.2f"), Hist.TempScoreAdjust);
#endif
				
			}

#if ENABLE_VISUAL_LOG
			UE_VLOG(GetLogOwner(), LogSuss, Log, TEXT(" - TOTAL: %4.2f"), Score);
#endif

			if (!FMath::IsNearlyZero(Score))
			{
				CandidateActions.Add(FSussActionScoringResult { i, Ctx, Score });
				if (bIsCurrentAction)
				{
					bAddedCurrentAction = true;
				}
			}
		}
		
	}

	if (!bAddedCurrentAction && IsActionInProgress() && CurrentActionResult.Score > 0)
	{
		// If the current action wasn't added because it wasn't scoring > 0 right now, we should still add back
		// the current action with its current score. This is to avoid cases where an action changes the state which
		// made it valid in the first place, but it still has an ongoing task to do (but is interruptible as well)
		CandidateActions.Add(CurrentActionResult);
	}

	ChooseActionFromCandidates();

	OnPostBrainUpdate.Broadcast(this);
	
}

void USussBrainComponent::ResolveParameters(AActor* Self,
	const TMap<FName, FSussParameter>& InParams,
	TMap<FName, FSussParameter>& OutParams)
{
	FSussContext SelfContext { Self };
	for (const auto& Param : InParams)
	{
		OutParams.Add(Param.Key, ResolveParameter(SelfContext, Param.Value));
	}
}

const UObject* USussBrainComponent::GetLogOwner() const
{
	// VLog on pawn rather than AI controller for ease of use
	if (const auto Pawn = GetPawn())
	{
		return Pawn;
	}
	return this;
}

FSussParameter USussBrainComponent::ResolveParameter(const FSussContext& SelfContext, const FSussParameter& Value) const
{
	static TMap<FName, FSussParameter> DummyParams;

	if (Value.Type == ESussParamType::AutoParameter)
	{
		auto SUSS = GetSUSS(GetWorld());
		if (Value.InputOrParameterTag.MatchesTag(TAG_SussInputParentTag))
		{
			// Inputs always resolve to float
			if (auto InputProvider = SUSS->GetInputProvider(Value.InputOrParameterTag))
			{
				return InputProvider->Evaluate(this, SelfContext, DummyParams);
			}
		}
		else if (Value.InputOrParameterTag.MatchesTag(TAG_SussParamParentTag))
		{
			// Other auto params can return any value
			if (auto ParamProvider = SUSS->GetParameterProvider(Value.InputOrParameterTag))
			{
				return ParamProvider->Evaluate(this, SelfContext, DummyParams);
			}
		}
	}
	// Fallback
	return Value;
}

void USussBrainComponent::GenerateContexts(AActor* Self, const FSussActionDef& Action, TArray<FSussContext>& OutContexts)
{
	auto SUSS = GetSUSS(GetWorld());

	auto Pool = GetSussPool(GetWorld());

	if (Action.Queries.Num() > 0)
	{
		TSet<ESussQueryContextElement> ContextElements;
		TSet<FName> NamedQueryValues;

		for (const auto& Query : Action.Queries)
		{
			auto QueryProvider = SUSS->GetQueryProvider(Query.QueryTag);
			if (!QueryProvider)
				continue;

			FSussScopeReservedMap ResolvedQueryParamsScope = Pool->ReserveMap<FName, FSussParameter>();
			TMap<FName, FSussParameter>& ResolvedParams = *ResolvedQueryParamsScope.Get<FName, FSussParameter>();
			ResolveParameters(Self, Query.Params, ResolvedParams);

			// Because we use the results from each query to multiply combinations with existing results, we cannot have >1 query
			// returning the same element (you'd multiply Targets * Targets for example)
			const auto Element = QueryProvider->GetProvidedContextElement();
			// Special case for Named Values, we can have multiples, just not providing the same name
			if (Element != ESussQueryContextElement::NamedValue && ContextElements.Contains(Element))
			{
				UE_LOG(LogSuss,
				       Warning,
				       TEXT("Action %s has more than one query returning %s, ignoring extra one %s"),
				       *Action.ActionTag.ToString(),
				       *StaticEnum<ESussQueryContextElement>()->GetValueAsString(Element),
				       *Query.QueryTag.ToString())
				continue;
			}
			ContextElements.Add(Element);

			if (Element == ESussQueryContextElement::NamedValue)
			{
				if (auto NQP = Cast<USussNamedValueQueryProvider>(QueryProvider))
				{
					const FName ValueName = NQP->GetQueryValueName();
					// Make sure we haven't seen this name before; since we allow multiple named type queries
					if (NamedQueryValues.Contains(ValueName))
					{
						UE_LOG(LogSuss,
							   Warning,
							   TEXT("Action %s has more than one query returning named value %s, ignoring extra one %s"),
							   *Action.ActionTag.ToString(),
							   *ValueName.ToString(),
							   *Query.QueryTag.ToString());
						continue;
					}
					NamedQueryValues.Add(ValueName);
				}
			}

			if (QueryProvider->IsCorrelatedWithContext())
			{
				IntersectCorrelatedContexts(Self, Query, QueryProvider, ResolvedParams, OutContexts);
			}
			else
			{
				if (!AppendUncorrelatedContexts(Self, Query, QueryProvider, ResolvedParams, OutContexts))
				{
					// This query generated no results, therefore instead of NxM it's Nx0 == no results at all
					OutContexts.Empty();
					return;
				}
			}
			
		}
	}
	else
	{
		// No queries, just self
		OutContexts.Add(FSussContext { Self });
	}
	
}

void USussBrainComponent::IntersectCorrelatedContexts(AActor* Self,
                                                   const FSussQuery& Query,
                                                   USussQueryProvider* QueryProvider,
                                                   const TMap<FName, FSussParameter>& Params,
                                                   TArray<FSussContext>& InOutContexts)
{
	// Correlated results run a query once for each existing context generated from previous queries, then combine the
	// results with that one context, meaning that instead of C * N contexts, you get N(C1) + N(C2) + .. N(Cx) contexts

	auto Pool = GetSussPool(GetWorld());
	const auto Element = QueryProvider->GetProvidedContextElement();

	int InContextCount = InOutContexts.Num();

	for (int i = 0; i < InContextCount; ++i)
	{
		FSussContext& SourceContext = InOutContexts[i];
		int NumResults = 0;
		switch(Element)
		{
		case ESussQueryContextElement::Target:
			{
				FSussScopeReservedArray Targets = Pool->ReserveArray<TWeakObjectPtr<AActor>>();
				QueryProvider->GetResultsInContext<TWeakObjectPtr<AActor>>(this, Self, SourceContext, Params, *Targets.Get<TWeakObjectPtr<AActor>>());

				NumResults = Targets.Get<TWeakObjectPtr<AActor>>()->Num();
				if (NumResults > 0)
				{
					AppendCorrelatedContexts<TWeakObjectPtr<AActor>>(Self,
					                                                 Targets,
					                                                 SourceContext,
					                                                 InOutContexts,
					                                                 [](const TWeakObjectPtr<AActor>& Target,
					                                                    FSussContext& Ctx)
					                                                 {
						                                                 Ctx.Target = Target;
					                                                 });
				}
				break;
			}
		case ESussQueryContextElement::Location:
			{
				FSussScopeReservedArray Targets = Pool->ReserveArray<FVector>();
				QueryProvider->GetResultsInContext<FVector>(this, Self, SourceContext, Params, *Targets.Get<FVector>());

				NumResults = Targets.Get<FVector>()->Num();
				if (NumResults > 0)
				{
					AppendCorrelatedContexts<FVector>(Self,
													  Targets,
													  SourceContext,
													  InOutContexts,
													  [](const FVector& Location, FSussContext& Ctx)
													  {
														  Ctx.Location = Location;
													  });
				}
				break;
			}
		case ESussQueryContextElement::NamedValue:
			{
				if (auto NQP = Cast<USussNamedValueQueryProvider>(QueryProvider))
				{
					const FName ValueName = NQP->GetQueryValueName();
					FSussScopeReservedArray NamedValues = Pool->ReserveArray<FSussContextValue>();
					QueryProvider->GetResultsInContext<FSussContextValue>(this, Self, SourceContext, Params, *NamedValues.Get<FSussContextValue>());
					NumResults = NamedValues.Get<FSussContextValue>()->Num();
					if (NumResults > 0)
					{
						AppendCorrelatedContexts<FSussContextValue>(Self,
																	NamedValues,
																	SourceContext,
																	InOutContexts,
																	[ValueName](const FSussContextValue& Value,
																				FSussContext& Ctx)
																	{
																		Ctx.NamedValues.Add(ValueName, Value);
																	});
					}
				}
				break;
			}
		}

		if (NumResults == 0)
		{
			// Correlated queries require results from BOTH (intersection). If this query didn't return any
			// results, it means that we must remove this incoming context because it's not valid
			InOutContexts.RemoveAt(i);
			--i;
			--InContextCount;
		}
	}
}

bool USussBrainComponent::AppendUncorrelatedContexts(AActor* Self,
                                                     const FSussQuery& Query,
                                                     USussQueryProvider* QueryProvider,
                                                     const TMap<FName, FSussParameter>& Params,
                                                     TArray<FSussContext>& OutContexts)
{
	// Uncorrelated results run a query once, and combine the results in every combination with any existing

	auto Pool = GetSussPool(GetWorld());
	const auto Element = QueryProvider->GetProvidedContextElement();
	bool bAnyResults = false;
	switch (Element)
	{
	case ESussQueryContextElement::Target:
		{
			FSussScopeReservedArray Targets = Pool->ReserveArray<TWeakObjectPtr<AActor>>();
			const auto TargetArray = Targets.Get<TWeakObjectPtr<AActor>>();
			TargetArray->Append(
				QueryProvider->GetResults<TWeakObjectPtr<AActor>>(this, Self, Query.MaxFrequency, Params));
			AppendUncorrelatedContexts<TWeakObjectPtr<AActor>>(Self,
			                                       Targets,
			                                       OutContexts,
			                                       [](const TWeakObjectPtr<AActor>& Target, FSussContext& Ctx)
			                                       {
				                                       Ctx.Target = Target;
			                                       });
			bAnyResults = TargetArray->Num() > 0;
			break;
		}
	case ESussQueryContextElement::Location:
		{
			FSussScopeReservedArray Locations = Pool->ReserveArray<FVector>();
			const auto LocationArray = Locations.Get<FVector>();
			LocationArray->Append(
				QueryProvider->GetResults<FVector>(this, Self, Query.MaxFrequency, Params));
			AppendUncorrelatedContexts<FVector>(Self,
			                        Locations,
			                        OutContexts,
			                        [](const FVector& Loc, FSussContext& Ctx)
			                        {
				                        Ctx.Location = Loc;
			                        });
			bAnyResults = LocationArray->Num() > 0;
			break;
		}
	case ESussQueryContextElement::NamedValue:
		{
			if (auto NQP = Cast<USussNamedValueQueryProvider>(QueryProvider))
			{
				const FName ValueName = NQP->GetQueryValueName();
				FSussScopeReservedArray NamedValues = Pool->ReserveArray<FSussContextValue>();
				const auto ValArray = NamedValues.Get<FSussContextValue>();
				ValArray->Append(
					QueryProvider->GetResults<FSussContextValue>(this, Self, Query.MaxFrequency, Params));
				AppendUncorrelatedContexts<FSussContextValue>(Self,
				                                  NamedValues,
				                                  OutContexts,
				                                  [ValueName](const FSussContextValue& Value, FSussContext& Ctx)
				                                  {
					                                  Ctx.NamedValues.Add(ValueName, Value);
				                                  });
				bAnyResults = ValArray->Num() > 0;
			}
			break;
		}
	}

	return bAnyResults;
}

bool USussBrainComponent::IsActionSameAsCurrent(int NewActionIndex,
                                                           const FSussContext& NewCtx)
{
	// Tolerance that locations must be within squared distance to be considered the same
	// Allow more wiggle room than usual 
	static constexpr float LocationToleranceSq = 30*30;
	if (IsActionInProgress() && NewActionIndex == CurrentActionResult.ActionDefIndex)
	{
		// OK this is the same action, but is the context the same or similar enough?
		const auto& CurrCtx = CurrentActionResult.Context;

		// Targets must match (use Get instead of direct != since that constructs temp ptr)
		if (CurrCtx.Target.Get() != NewCtx.Target.Get())
		{
			return false;
		}

		// Locations must be close enough
		if (FVector::DistSquared(CurrCtx.Location, NewCtx.Location) > LocationToleranceSq)
		{
			return false;
		}

		// Check named params, assume they're relevant
		if (CurrCtx.NamedValues.Num() != NewCtx.NamedValues.Num())
		{
			return false;
		}
		for (const auto& KV : CurrCtx.NamedValues)
		{
			if (auto pVal = NewCtx.NamedValues.Find(KV.Key))
			{
				if (KV.Value != *pVal)
				{
					return false;
				}
			}
			else
			{
				return false;
			}
		}

		return true;
	}

	return false;
				
}

bool USussBrainComponent::ShouldSubtractRepetitionPenaltyToProposedAction(int NewActionIndex,
	const FSussContext& NewContext)
{
	// We only add repetition penalties to previously run actions
	if (!IsActionInProgress() || NewActionIndex != CurrentActionResult.ActionDefIndex)
	{
		return ActionHistory[NewActionIndex].LastEndTime > 0;
	}
	return false;
}


AAIController* USussBrainComponent::GetAIController() const
{
	if (const auto Cached = AiController.Get())
	{
		return Cached;
	}

	AAIController* Found = Cast<AAIController>(GetOwner());
	if (!Found)
	{
		if (const APawn* Pawn = Cast<APawn>(GetOwner()))
		{
			Found = Cast<AAIController>(Pawn->GetController());
		}
	}

	if (Found)
	{
		AiController = Found;
		return Found;
	}

	return nullptr;
	
}

UCharacterMovementComponent* USussBrainComponent::GetCharacterMovement() const
{
	if (const ACharacter* Char = Cast<ACharacter>(GetOwner()))
	{
		return Char->GetCharacterMovement();
	}

	return nullptr;
	
}

APawn* USussBrainComponent::GetPawn() const
{
	if (auto AI = GetAIController())
	{
		return AI->GetPawn();
	}
	return nullptr;
}

AActor* USussBrainComponent::GetSelf() const
{
	if (auto Ctrl = GetAIController())
	{
		return Ctrl->GetPawn();
	}

	// Fallback support for brains directly on actor (mostly for testing)
	return GetOwner();
}

double USussBrainComponent::GetTimeSinceActionPerformed(FGameplayTag ActionTag) const
{
	double LastTime = -UE_DOUBLE_BIG_NUMBER;

	if (ActionTag.IsValid())
	{
		for (int i = 0; i < ActionHistory.Num(); ++i)
		{
			const auto& H = ActionHistory[i];
			const auto& Def = CombinedActionsByPriority[i];
			if (Def.ActionTag == ActionTag)
			{
				// Use the last END time, that way an action can ask about its *own* last run during execution
				// If we used the start time then if you did that you'd only ever get 0 seconds
				LastTime = FMath::Max(LastTime, H.LastEndTime);
			}
		}
	}
	return GetWorld()->GetTimeSeconds() - LastTime;
}

void USussBrainComponent::OnPerceptionUpdated(const TArray<AActor*>& Actors)
{
	if (DistanceCategory != ESussDistanceCategory::OutOfRange)
	{
		QueueForUpdate();
	}
}

void USussBrainComponent::SetTemporaryActionScoreAdjustment(FGameplayTag ActionTag, float Value, float CooldownTime)
{
	// Can potentially apply to multiple actions, if the same tag is used multiple times with eg diff params
	for (int i = 0; i < CombinedActionsByPriority.Num(); ++i)
	{
		if (CombinedActionsByPriority[i].ActionTag == ActionTag)
		{
			SetTemporaryActionScoreAdjustment(i, Value, CooldownTime);
		}
	}
}

void USussBrainComponent::AddTemporaryActionScoreAdjustment(FGameplayTag ActionTag, float Value, float CooldownTime)
{
	// Can potentially apply to multiple actions, if the same tag is used multiple times with eg diff params
	for (int i = 0; i < CombinedActionsByPriority.Num(); ++i)
	{
		if (CombinedActionsByPriority[i].ActionTag == ActionTag)
		{
			AddTemporaryActionScoreAdjustment(i, Value, CooldownTime);
		}
	}
}

void USussBrainComponent::ResetTemporaryActionScoreAdjustment(FGameplayTag ActionTag)
{
	// Can potentially apply to multiple actions, if the same tag is used multiple times with eg diff params
	for (int i = 0; i < CombinedActionsByPriority.Num(); ++i)
	{
		if (CombinedActionsByPriority[i].ActionTag == ActionTag)
		{
			ResetTemporaryActionScoreAdjustment(i);
		}
	}
}

void USussBrainComponent::ResetAllTemporaryActionScoreAdjustments()
{
	for (int i = 0; i < ActionHistory.Num(); ++i)
	{
		ActionHistory[i].TempScoreAdjust = 0;
	}
}

void USussBrainComponent::SetTemporaryActionScoreAdjustment(int ActionIndex, float Value, float CooldownTime)
{
	if (ActionHistory.IsValidIndex(ActionIndex))
	{
		auto& H = ActionHistory[ActionIndex];
		H.TempScoreAdjust = Value;
		H.TempScoreAdjustCooldownRate = CooldownTime > 0 ? Value / CooldownTime : 0;
	}
}

void USussBrainComponent::AddTemporaryActionScoreAdjustment(int ActionIndex, float Value, float CooldownTime)
{
	if (ActionHistory.IsValidIndex(ActionIndex))
	{
		auto& H = ActionHistory[ActionIndex];
		float PrevCooldownTimeRemaining = 0;
		if (!FMath::IsNearlyZero(H.TempScoreAdjust) && !FMath::IsNearlyZero(H.TempScoreAdjustCooldownRate))
		{
			PrevCooldownTimeRemaining = H.TempScoreAdjustCooldownRate > 0 ? H.TempScoreAdjust / H.TempScoreAdjustCooldownRate : 0;
		}
		H.TempScoreAdjust += Value;
		const float NewCooldownTime = CooldownTime + PrevCooldownTimeRemaining;
		H.TempScoreAdjustCooldownRate = NewCooldownTime > 0 ? H.TempScoreAdjust / NewCooldownTime : 0;
	}
}

void USussBrainComponent::ResetTemporaryActionScoreAdjustment(int ActionIndex)
{
	if (ActionHistory.IsValidIndex(ActionIndex))
	{
		auto& H = ActionHistory[ActionIndex];
		H.TempScoreAdjust = 0;
		H.TempScoreAdjustCooldownRate = 0;
	}
}

FString USussBrainComponent::GetDebugSummaryString() const
{
	TStringBuilder<256> Builder;
	Builder.Appendf(TEXT("Distance Category: %s  UpdateFreq: %4.2f\n"), *StaticEnum<ESussDistanceCategory>()->GetValueAsString(DistanceCategory), CurrentUpdateInterval);
	if (bIsLogicStopped)
	{
		Builder.Appendf(TEXT("Logic currently stopped, reason: %s\n"),*LogicStoppedReason);
	}
	
	if (CombinedActionsByPriority.IsValidIndex(CurrentActionResult.ActionDefIndex))
	{
		// Log all actions
		// Log all considerations?
		const FSussActionDef& Def = CombinedActionsByPriority[CurrentActionResult.ActionDefIndex];
		const auto& H = ActionHistory[CurrentActionResult.ActionDefIndex];
		Builder.Appendf(
			TEXT(
				"Current Action: {yellow}%s{white}\nOriginal Score: {yellow}%4.2f{white}\nCurrent Score: {yellow}%4.2f{white}"),
				Def.Description.IsEmpty() ? 
					*CurrentActionInstance->GetClass()->GetName() :
					*Def.Description,
				H.LastRunScore,
				CurrentActionResult.Score);
	}

	return Builder.ToString();
		
}

void USussBrainComponent::DebugLocations(TArray<FVector>& OutLocations, bool bIncludeDetails) const
{
	if (CurrentActionInstance.IsValid())
	{
		CurrentActionInstance->DebugLocations(OutLocations, bIncludeDetails);
	}
}

void USussBrainComponent::GetDebugDetailLines(TArray<FString>& OutLines) const
{
	OutLines.Reset();
	OutLines.Add(TEXT("Candidate Actions:"));
	for (const auto& Action : CandidateActions)
	{
		const FSussActionDef& Def = CombinedActionsByPriority[Action.ActionDefIndex];
		OutLines.Add(FString::Printf(
			TEXT(" - {yellow}%s  {white}%4.2f"),
			Def.Description.IsEmpty()
				? *Def.ActionTag.ToString()
				: *Def.Description,
			Action.Score
			));

		// If we want to list consideration scores here, we have to store them
	}
}
