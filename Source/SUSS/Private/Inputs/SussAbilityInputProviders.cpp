﻿#include "Inputs/SussAbilityInputProviders.h"

#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "SussCommon.h"
#include "SussParameter.h"

UE_DEFINE_GAMEPLAY_TAG_COMMENT(TAG_SussInputCanActivateAbility, "Suss.Input.Ability.CanActivate", "Get a value of 1 if an ability can be activated (self), 0 otherwise. Requires a single parameter 'Tag' identifying the ability.")

float USussCanActivateAbilityInputProvider::EvaluateAbility(const AActor* Owner, const FGameplayTag& Tag) const
{
	if (Tag.IsValid() && IsValid(Owner))
	{
		if (auto ASC
			= UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Owner))
		{
			TArray<FGameplayAbilitySpec*> Abilities;
			ASC->GetActivatableGameplayAbilitySpecsByAllMatchingTags(FGameplayTagContainer(Tag), Abilities);

			for (auto Spec : Abilities)
			{
				if (Spec->Ability->CanActivateAbility(Spec->Handle, ASC->AbilityActorInfo.Get()))
				{
					// If we could activate at least one ability, return 1
					return 1;
				}
			}
		}
	}

	return 0;
}

USussCanActivateAbilityInputProvider::USussCanActivateAbilityInputProvider()
{
	InputTag = TAG_SussInputCanActivateAbility;
}

float USussCanActivateAbilityInputProvider::Evaluate_Implementation(const USussBrainComponent* Brain,
	const FSussContext& Context,
	const TMap<FName, FSussParameter>& Parameters) const
{
	if (auto pParam = Parameters.Find(SUSS::TagParamName))
	{
		return EvaluateAbility(Context.ControlledActor, pParam->Tag);
	}

	return 0;
}
