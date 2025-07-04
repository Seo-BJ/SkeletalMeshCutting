
#pragma once

#include "CoreMinimal.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "SceneTexturesConfig.h"
#include "PostProcess/PostProcessInputs.h"


//Shader Property Struct
BEGIN_SHADER_PARAMETER_STRUCT(FColourExtractParams,)
	//定义颜色、贴图参数
	SHADER_PARAMETER(FVector3f, TargetColour)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
	SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureShaderParameters, SceneTextures)

	//运行时绑定渲染目标
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

class FLearnShaderPS : public FGlobalShader
{
public:
	//生成着色器类型并且序列化
	DECLARE_EXPORTED_SHADER_TYPE(FLearnShaderPS,Global,)
	using FParameters = FColourExtractParams;
	SHADER_USE_PARAMETER_STRUCT(FLearnShaderPS, FGlobalShader);

	//确定在某些情况下是否应该编译该着色器
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		
		//SET_SHADER_DEFINE(OutEnvironment, YOUR_SHADER_MARCO, 0);
	}
};

static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
{
	return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
}

static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		
	//SET_SHADER_DEFINE(OutEnvironment, YOUR_SHADER_MARCO, 0);
}