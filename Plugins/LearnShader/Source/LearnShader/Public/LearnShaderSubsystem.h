
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/EngineSubsystem.h"
#include "LearnShaderSubsystem.generated.h"

class FSceneViewExtensionBase;

UCLASS()
//DLL export flag
class LEARNSHADER_API ULearnShaderSubsystem : public UEngineSubsystem
{
	GENERATED_BODY()

private:
	TSharedPtr<FSceneViewExtensionBase, ESPMode::ThreadSafe> CustomSceneViewExtension;

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	//Executed when removed
	virtual void Deinitialize() override;
};