// Copyright Epic Games, Inc. All Rights Reserved.

#include "SkeletalMeshCuttingGameMode.h"
#include "SkeletalMeshCuttingCharacter.h"
#include "UObject/ConstructorHelpers.h"

ASkeletalMeshCuttingGameMode::ASkeletalMeshCuttingGameMode()
	: Super()
{
	// set default pawn class to our Blueprinted character
	static ConstructorHelpers::FClassFinder<APawn> PlayerPawnClassFinder(TEXT("/Game/FirstPerson/Blueprints/BP_FirstPersonCharacter"));
	DefaultPawnClass = PlayerPawnClassFinder.Class;

}
