#include "LearnShaderSceneViewExtension.h"

#include "Rendering/LeaernShaderPS.h"
#include "PixelShaderUtils.h"
#include "Runtime/Renderer/Private/SceneRendering.h"

DECLARE_GPU_DRAWCALL_STAT(ColourMix);

LearnShaderSceneViewExtension::LearnShaderSceneViewExtension(const FAutoRegister& AutoRegister): FSceneViewExtensionBase(AutoRegister){}
LearnShaderSceneViewExtension::~LearnShaderSceneViewExtension(){}
void LearnShaderSceneViewExtension::SetupViewFamily(FSceneViewFamily& InViewFamily){}
void LearnShaderSceneViewExtension::SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView){}
void LearnShaderSceneViewExtension::BeginRenderViewFamily(FSceneViewFamily& InViewFamily){}

void LearnShaderSceneViewExtension::PrePostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View,
	const FPostProcessingInputs& Inputs)
{
	FSceneViewExtensionBase::PrePostProcessPass_RenderThread(GraphBuilder, View, Inputs);
	
	checkSlow(View.bIsViewInfo);
	const FIntRect Viewport = static_cast<const FViewInfo&>(View).ViewRect;
	const FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

	//Trace analysis
	
	//Unreal Insights
	RDG_GPU_STAT_SCOPE(GraphBuilder,ColourMix);
	// RenderDoc
	RDG_EVENT_SCOPE(GraphBuilder,"Colour Mix");
	
	//Grab the scene texture
	const FSceneTextureShaderParameters SceneTextures = CreateSceneTextureShaderParameters(GraphBuilder, View, ESceneTextureSetupMode::SceneColor | ESceneTextureSetupMode::GBuffers);
	//This is the color that actually has the shadow and the shade
	const FScreenPassTexture SceneColourTexture((*Inputs.SceneTextures)->SceneColorTexture, Viewport);


	//Set Global Shader data, allocate memory
	FLearnShaderPS::FParameters* Parameters = GraphBuilder.AllocParameters<FLearnShaderPS::FParameters>();
	Parameters->SceneColorTexture = SceneColourTexture.Texture;
	Parameters->SceneTextures = SceneTextures;
	Parameters->TargetColour = FVector3f(1.0f, 0.0f, 1.0f);

	//Set RenderTarget and Return Texture
	Parameters->RenderTargets[0] = FRenderTargetBinding((*Inputs.SceneTextures)->SceneColorTexture, ERenderTargetLoadAction::ELoad);

	TShaderMapRef<FLearnShaderPS> PixelShader(GlobalShaderMap);
	FPixelShaderUtils::AddFullscreenPass(GraphBuilder, GlobalShaderMap, FRDGEventName(TEXT("Colour Mix Pass")), PixelShader, Parameters, Viewport);
}