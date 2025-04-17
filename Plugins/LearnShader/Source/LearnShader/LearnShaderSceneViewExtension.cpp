#include "LearnShaderSceneViewExtension.h"

LearnShaderSceneViewExtension::LearnShaderSceneViewExtension(const FAutoRegister& AutoRegister): FSceneViewExtensionBase(AutoRegister){}
LearnShaderSceneViewExtension::~LearnShaderSceneViewExtension(){}
void LearnShaderSceneViewExtension::SetupViewFamily(FSceneViewFamily& InViewFamily){}
void LearnShaderSceneViewExtension::SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView){}
void LearnShaderSceneViewExtension::BeginRenderViewFamily(FSceneViewFamily& InViewFamily){}

void LearnShaderSceneViewExtension::PrePostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View,
	const FPostProcessingInputs& Inputs)
{
	FSceneViewExtensionBase::PrePostProcessPass_RenderThread(GraphBuilder, View, Inputs);
}