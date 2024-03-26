﻿
#include "Queries/SussEQSQueryProvider.h"

#include "EnvironmentQuery/EnvQueryManager.h"
#include "EnvironmentQuery/Items/EnvQueryItemType_ActorBase.h"
#include "EnvironmentQuery/Items/EnvQueryItemType_VectorBase.h"
#include "Misc/TransactionObjectEvent.h"

TSharedPtr<FEnvQueryResult> USussEQSQueryProvider::RunEQSQuery(USussBrainComponent* Brain,
                                        AActor* Self,
                                        const TMap<FName, FSussParameter>& Params)
{
	if (!EQSQuery)
		return nullptr;

	if (UEnvQueryManager* EQS = UEnvQueryManager::GetCurrent(Self->GetWorld()))
	{
		// We *could* allow EQS to be run in steps over many frames here
		// See ExecuteOneStep inside this function, or EQSTestingPawn
		// Or we could use RunQuery with callback.
		// For now, run synchronous for simplicity, and limit time between AIs
		FEnvQueryRequest QueryRequest(EQSQuery, Self);
		for (FAIDynamicParam& Param : QueryConfig)
		{
			QueryRequest.SetDynamicParam(Param);
		}
		// Also set params from incoming instance request
		for (const auto& Param : Params)
		{
			// Only set params which are registered to this query, so shared params can be used
			if (ParamNames.Contains(Param.Key))
			{
				const FSussParameter& InParam = Param.Value;
				FAIDynamicParam DParam;
				DParam.ParamName = Param.Key;
				switch (Param.Value.Type)
				{
				case ESussParamType::Float:
					DParam.ParamType = EAIParamType::Float;
					DParam.Value = InParam.FloatValue;
					break;
				case ESussParamType::Int:
					DParam.ParamType = EAIParamType::Int;
					DParam.Value = InParam.IntValue;
					break;
				case ESussParamType::Tag:
				case ESussParamType::Input:
					// Not supported
					continue;
				}
				QueryRequest.SetDynamicParam(DParam);
			}
		}
		return EQS->RunInstantQuery(QueryRequest, QueryMode);
	}

	return nullptr;
}

void USussEQSTargetQueryProvider::ExecuteQuery(USussBrainComponent* Brain,
	AActor* Self,
	const TMap<FName, FSussParameter>& Params,
	TArray<TWeakObjectPtr<AActor>>& OutResults)
{
	const auto Result = RunEQSQuery(Brain, Self, Params);
	if (Result->ItemType->IsChildOf(UEnvQueryItemType_ActorBase::StaticClass()))
	{
		const UEnvQueryItemType_ActorBase* DefTypeOb =  Result->ItemType->GetDefaultObject<UEnvQueryItemType_ActorBase>();
		for (const auto& Item : Result->Items)
		{
			OutResults.Add(DefTypeOb->GetActor(Result->RawData.GetData() + Item.DataOffset));
		}
	}
}

void USussEQSLocationQueryProvider::ExecuteQuery(USussBrainComponent* Brain,
                                                 AActor* Self,
                                                 const TMap<FName, FSussParameter>& Params,
                                                 TArray<FVector>& OutResults)
{
	const auto Result = RunEQSQuery(Brain, Self, Params);
	if (Result->ItemType->IsChildOf(UEnvQueryItemType_VectorBase::StaticClass()))
	{
		const UEnvQueryItemType_VectorBase* DefTypeOb =  Result->ItemType->GetDefaultObject<UEnvQueryItemType_VectorBase>();
		for (const auto& Item : Result->Items)
		{
			OutResults.Add(DefTypeOb->GetItemLocation(Result->RawData.GetData() + Item.DataOffset));
		}
	}
}


#if WITH_EDITOR
void USussEQSQueryProvider::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	if (PropertyChangedEvent.Property != nullptr)
	{
		OnPropertyChanged(PropertyChangedEvent.MemberProperty->GetFName());
	}

	Super::PostEditChangeProperty(PropertyChangedEvent);
}

void USussEQSQueryProvider::PostTransacted(const FTransactionObjectEvent& TransactionEvent)
{
	Super::PostTransacted(TransactionEvent);

	if (TransactionEvent.GetEventType() == ETransactionObjectEventType::UndoRedo)
	{
		if (TransactionEvent.GetChangedProperties().Num() > 0)
		{
			// targeted update
			for (const FName PropertyName : TransactionEvent.GetChangedProperties())
			{
				OnPropertyChanged(PropertyName);
			}
		}
	}
}

void USussEQSQueryProvider::OnPropertyChanged(const FName PropName)
{
	// This is to update our parameter list based on the query selected
	static const FName NAME_Query = GET_MEMBER_NAME_CHECKED(USussEQSQueryProvider, EQSQuery);
	static const FName NAME_QueryConfig = GET_MEMBER_NAME_CHECKED(USussEQSQueryProvider, QueryConfig);

	if (PropName == NAME_Query || PropName == NAME_QueryConfig)
	{
		if (EQSQuery)
		{
			EQSQuery->CollectQueryParams(*this, QueryConfig);
		}
	}
}
#endif // WITH_EDITOR