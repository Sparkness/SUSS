﻿// 

#pragma once

#include "CoreMinimal.h"
#include "NativeGameplayTags.h"
#include "SussAction.h"
#include "SussAbilityActions.generated.h"

class UGameplayAbility;

UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_SussActionActivateAbility);

/**
 * Base action class which activates a gameplay ability.
 * You can derive from this class in a Blueprint and set the specific Ability to run.
 * Alternatively, use the more generic USussActivateAbilityAction and use tag parameters instead.
 */
UCLASS(Abstract)
class SUSS_API USussActivateAbilityByClassAction : public USussAction
{
	GENERATED_BODY()
protected:
	/// A single ability to try to activate by class when executing this action.
	/// You can use this OR the AbilityTags option
	UPROPERTY(BlueprintReadOnly, EditDefaultsOnly)
	TSubclassOf<UGameplayAbility> AbilityClass;

	/// One or more tags to identify abilities to activate: an ability must match ALL of them.
	/// You can use this OR the AbilityClass option
	UPROPERTY(BlueprintReadOnly, EditDefaultsOnly)
	FGameplayTagContainer AbilityTags;

	/// If true, will remotely activate local and server abilities, if false it will only try to locally activate the ability
	UPROPERTY(BlueprintReadOnly, EditDefaultsOnly)
	bool bAllowRemoteActivation = true;

	/// If > 0, will delay the call to ActionCompleted by this many seconds
	UPROPERTY(BlueprintReadOnly, EditDefaultsOnly)
	float CompletionDelay = 0;

	UFUNCTION()
	void DelayedCompletion();
public:
	
	virtual void PerformAction_Implementation(const FSussContext& Context,
		const TMap<FName, FSussParameter>& Params,
		TSubclassOf<USussAction> PreviousActionClass) override;
};

/**
 * Generic action which activates a gameplay ability by tag.
 * Parameters are
 *   - "Tag": the tag to match abilities to activate
 *   - "CompletionDelay": Number of seconds to delay calling ActionCompleted (default 0)
 *   - "AllowRemote": If true, will remotely activate local and server abilities, if false it will only try to locally activate the ability (default true)
 *
 * This action is general and doesn't need subclassing. You can also pair it with USussCanActivateAbilityInputProvider
 * in order to filter out abilities that are not available.
 */
UCLASS()
class SUSS_API USussActivateAbilityAction : public USussAction
{
	GENERATED_BODY()
protected:
	
	UFUNCTION()
	void DelayedCompletion();
public:
	USussActivateAbilityAction();
	virtual void PerformAction_Implementation(const FSussContext& Context,
		const TMap<FName, FSussParameter>& Params,
		TSubclassOf<USussAction> PreviousActionClass) override;
};
