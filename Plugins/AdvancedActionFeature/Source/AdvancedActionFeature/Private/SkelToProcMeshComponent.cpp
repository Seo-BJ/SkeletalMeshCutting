#include "SkelToProcMeshComponent.h"

#include "KismetProceduralMeshLibrary.h"
#include "Components/SkeletalMeshComponent.h"
#include "ProceduralMeshComponent.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h" 
#include "DrawDebugHelpers.h"

USkelToProcMeshComponent::USkelToProcMeshComponent()
{
    PrimaryComponentTick.bCanEverTick = true; // 런타임 스키닝을 위해 틱 활성화
    // PrimaryComponentTick.bStartWithTickEnabled = false; // 기본적으로는 비활성화, 필요할 때만 활성화
}

void USkelToProcMeshComponent::BeginPlay()
{
    Super::BeginPlay();

    if (bConvertOnBeginPlay)
    {
        PrimaryComponentTick.bStartWithTickEnabled = true; 
        ConvertSkeletalMeshToProceduralMesh(false, BoneName);
    }
}

void USkelToProcMeshComponent::TickComponent(float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    UpdateProceduralMeshesSkinning();
}

bool USkelToProcMeshComponent::ConvertSkeletalMeshToProceduralMesh(bool bForceNewPMC, FName TargetBoneName)
{
    USkeletalMeshComponent* SkelComp = GetOwnerSkeletalMeshComponent();
    if (!SkelComp || !SkelComp->GetSkeletalMeshAsset())
    {
        UE_LOG(LogTemp, Error, TEXT("SkelToProcMeshComponent: 유효하지 않은 Skeletal Mesh Component 또는 Asset입니다. 변환할 수 없습니다."));
        return false;
    }

    
    if (!SetupProceduralMeshComponent(bForceNewPMC))
    {
        UE_LOG(LogTemp, Error, TEXT("SkelToProcMeshComponent: Procedural Mesh Component 설정에 실패했습니다. 변환할 수 없습니다."));
        return false;
    }

    
    ProceduralMeshComponent->SetWorldLocation(SkelComp->GetComponentLocation());
    // ProceduralMeshComponent->SetWorldRotation(SkelComp->GetComponentRotation());
    
    // 원본 스켈레탈 메시의 역 바인드 포즈 행렬 가져오기
    const FReferenceSkeleton& RefSkeleton = SkelComp->GetSkinnedAsset()->GetRefSkeleton();
    const TArray<FTransform>& RefBonePose = RefSkeleton.GetRefBonePose();
    
    RefBoneInverseBindMatrices.Empty(RefBonePose.Num());
    
    for (const FTransform& BonePose : RefBonePose)
    {
        // 컴포넌트 공간에서의 역 바인드 포즈 (스켈레탈 메시는 보통 본들이 루트에 대해 상대적으로 정의됨)
        RefBoneInverseBindMatrices.Add(BonePose.ToMatrixWithScale().Inverse());
    }
    // 데이터
    // 복사 및 스키닝 정보 빌드
    bool bSuccess = CopySkeletalLODToProcedural(SkelComp, TargetBoneName, LODIndexToCopy);

    if(bSuccess)
    {
        UE_LOG(LogTemp, Log, TEXT("SkelToProcMeshComponent: 성공적으로 LOD %d의 Sekeltal Mesh를 Procedural Mesh로 변환 완료. 그리고 skinning data 구축 시작."), LODIndexToCopy);
        if (bEnableRuntimeSkinning)
        {
            PrimaryComponentTick.SetTickFunctionEnable(true); // 런타임 스키닝이 활성화되어 있으면 틱 시작
        }
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("SkelToProcMeshComponent: LOD %d의 스켈레탈 메쉬 변환 실패 또는 skinning data 구축 실패."), LODIndexToCopy);
    }
    return bSuccess;
}


bool USkelToProcMeshComponent::CopySkeletalLODToProcedural(USkeletalMeshComponent* SkelComp, FName TargetBoneName, int32 LODIndex)
{
    TArray<FVector> Vertices_Main;
    TArray<FVector> Normals_Main;
    TArray<FProcMeshTangent> Tangents_Main;
    TArray<FVector2D> UV0_Main;
    TArray<FLinearColor> VertexColors_Main;
    TArray<int32> SectionMaterialIndices_Main;
    TArray<TArray<int32>> SectionIndices_Main;

    OriginalToMainProcVertexMap.Empty(); // 멤버 변수 초기화

    // GetFilteredSkeletalMeshDataByBoneName 호출하여 OriginalToMainProcVertexMap 채우기
    bool bDataExtracted = GetFilteredSkeletalMeshDataByBoneName(
        SkelComp, TargetBoneName, Threshold, LODIndex, // Threshold는 멤버 변수 사용
        Vertices_Main, Normals_Main, Tangents_Main, UV0_Main, VertexColors_Main,
        SectionMaterialIndices_Main, SectionIndices_Main,
        OriginalToMainProcVertexMap); // 이 맵이 채워짐

    if (!bDataExtracted)
    {
        UE_LOG(LogTemp, Error, TEXT("CopySkeletalLODToProcedural: Failed to extract mesh data for TargetBone '%s'."), *TargetBoneName.ToString());
        return false;
    }
    
    // 노멀 재계산 (선택 사항, 기존 로직)
    if (bRecalculateNormals && Vertices_Main.Num() > 0)
    {
        TArray<int32> AllIndices_Main;
        for(const TArray<int32>& Indices : SectionIndices_Main) AllIndices_Main.Append(Indices);
        if(AllIndices_Main.Num() > 0)
        {
            UKismetProceduralMeshLibrary::CalculateTangentsForMesh(Vertices_Main, AllIndices_Main, UV0_Main, Normals_Main, Tangents_Main);
        }
    }
    
    // 메인 프로시저럴 메시 생성
    for (int32 SectionIdx = 0; SectionIdx < SectionIndices_Main.Num(); ++SectionIdx)
    {
        if (SectionIndices_Main[SectionIdx].Num() > 0)
        {
            ProceduralMeshComponent->CreateMeshSection_LinearColor(
                SectionIdx, Vertices_Main, SectionIndices_Main[SectionIdx],
                Normals_Main, UV0_Main, VertexColors_Main, Tangents_Main, false);

            if (SectionMaterialIndices_Main.IsValidIndex(SectionIdx))
            {
                UMaterialInterface* Material = SkelComp->GetMaterial(SectionMaterialIndices_Main[SectionIdx]);
                if (!Material && SkelComp->GetSkeletalMeshAsset()->GetMaterials().IsValidIndex(SectionMaterialIndices_Main[SectionIdx]))
                {
                    Material = SkelComp->GetSkeletalMeshAsset()->GetMaterials()[SectionMaterialIndices_Main[SectionIdx]].MaterialInterface;
                }
                if (Material) ProceduralMeshComponent->SetMaterial(SectionIdx, Material);
            }
        }
    }
    
    MainProcMeshSkinningData.Empty();
    if (!BuildSkinningDataForProceduralMesh(SkelComp, LODIndex, Vertices_Main, OriginalToMainProcVertexMap, MainProcMeshSkinningData))
    {
        UE_LOG(LogTemp, Warning, TEXT("CopySkeletalLODToProcedural: Failed to build skinning data for Main Procedural Mesh. Runtime skinning might not work."));
        // 실패해도 절단은 계속 진행될 수 있도록 처리 (스키닝만 안됨)
    }

    
    // --- 메쉬 슬라이스 및 OtherHalf 처리 ---
    FVector BoneLocation = SkelComp->GetSocketLocation(TargetBoneName); // GetBoneLocation은 시뮬레이션 중인 본 위치를 줄 수 있음. 소켓 위치가 더 안정적일 수 있음.
    UProceduralMeshComponent* TempOtherHalfMesh = nullptr; // 로컬 변수로 선언

    // SliceMesh 호출
    UKismetProceduralMeshLibrary::SliceProceduralMesh(
        ProceduralMeshComponent, BoneLocation, SkelComp->GetUpVector(), // 절단면 법선 (예: 본의 UpVector)
        true, TempOtherHalfMesh, EProcMeshSliceCapOption::CreateNewSectionForCap, CapMaterialInterface);

    OtherHalfProceduralMeshComponent = TempOtherHalfMesh; // 멤버 변수에 할당
    
    if (OtherHalfProceduralMeshComponent)
    {
        // OtherHalf 메시도 SkelComp에 부착하거나 월드에 유지할지 결정. 여기서는 부착한다고 가정.
        OtherHalfProceduralMeshComponent->AttachToComponent(SkelComp, FAttachmentTransformRules::KeepWorldTransform, OtherHalfMeshAttachSocketName);
        OtherHalfProceduralMeshComponent->SetSimulatePhysics(false);
        OtherHalfProceduralMeshComponent->SetCollisionEnabled(ECollisionEnabled::QueryOnly);

        // OtherHalf 메시에 대한 스키닝 정보 빌드는 복잡한 작업.
        // SliceProceduralMesh는 버텍스를 새로 생성하고 인덱스를 변경할 수 있음.
        // 가장 정확한 방법은 슬라이스된 OtherHalfProcMesh의 각 버텍스가
        // 원본 스켈레탈 메쉬의 어떤 버텍스에 해당했는지 역추적하거나,
        // 슬라이스 평면과의 관계를 이용해 스키닝 정보를 근사하는 것.
        // 여기서는 OtherHalf의 스키닝 정보 빌드는 생략하고, 필요시 추가 구현.
        // 만약 OtherHalf도 스키닝하려면, 해당 PMC의 버텍스 정보를 가져와서(GetProcMeshSection),
        // 그 버텍스들이 MainPMC의 어떤 버텍스였는지 매핑하거나, 다시 원본 SkelMesh와 매핑해야 함.
        UE_LOG(LogTemp, Log, TEXT("SliceMesh created OtherHalf. Skinning data for it is not built in this version."));
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("SliceMesh did not create OtherHalf for bone '%s'."), *TargetBoneName.ToString());
    }

    // --- 원본 메쉬 숨기기, PMC 부착, 레그돌 (기존 로직) ---
    HideOriginalMeshVerticesByBone(SkelComp, LODIndex, TargetBoneName);

    ProceduralMeshComponent->SetSimulatePhysics(false);
    ProceduralMeshComponent->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    if (!ProceduralMeshAttachSocketName.IsNone() && SkelComp->DoesSocketExist(ProceduralMeshAttachSocketName))
    {
        ProceduralMeshComponent->AttachToComponent(SkelComp, FAttachmentTransformRules::KeepWorldTransform, ProceduralMeshAttachSocketName);
    }
    else
    {
         ProceduralMeshComponent->AttachToComponent(SkelComp, FAttachmentTransformRules::KeepWorldTransform, TargetBoneName); // 폴백
         UE_LOG(LogTemp, Warning, TEXT("ProceduralMeshAttachSocketName invalid or not found, attaching to TargetBoneName: %s"), *TargetBoneName.ToString());
    }
    
    SkelComp->SetCollisionProfileName(TEXT("Ragdoll"));
    SkelComp->SetSimulatePhysics(true);
    // SkelComp->AddImpulseAtLocation(...) 또는 BreakConstraint
    SkelComp->BreakConstraint(SkelComp->GetRightVector() * ImpulseMagnitude, BoneLocation, TargetBoneName); // 예시 임펄스

    return true;
}


bool USkelToProcMeshComponent::GetFilteredSkeletalMeshDataByBoneName(const USkeletalMeshComponent* SkelComp, FName TargetBoneName, float MinWeight, int32 LODIndex, TArray<FVector>& OutVertices,
    TArray<FVector>& OutNormals, TArray<FProcMeshTangent>& OutTangents, TArray<FVector2D>& OutUV0, TArray<FLinearColor>& OutVertexColors,
    TArray<int32>& OutSectionMaterialIndices, TArray<TArray<int32>>& OutSectionIndices, TMap<uint32, uint32>& OutOriginalToProcVertexMap)
{
    // 0. 필수 컴포넌트 및 데이터 유효성 검사
    if (!SkelComp || !SkelComp->GetSkeletalMeshAsset() || !SkelComp->GetSkeletalMeshAsset()->GetResourceForRendering()) return false;
    
    FSkeletalMeshRenderData* RenderData = SkelComp->GetSkeletalMeshAsset()->GetResourceForRendering();
    if (!RenderData || !RenderData->LODRenderData.IsValidIndex(LODIndex)) // LOD 인덱스 유효성 검사
    {
        UE_LOG(LogTemp, Warning, TEXT("GetFilteredSkeletalMeshDataByBoneName : LODRenderData[%d]이 유효하지 않습니다."), LODIndex);
        return false; 
    }
    
    FSkeletalMeshLODRenderData& LODRenderData = RenderData->LODRenderData[LODIndex];

    int32 FoundTargetBoneIndex = SkelComp->GetBoneIndex(TargetBoneName); // TargetBoneName에 대한 실제 본 인덱스
    if (TargetBoneName != NAME_None && FoundTargetBoneIndex == INDEX_NONE)
    {
        UE_LOG(LogTemp, Warning, TEXT("GetFilteredSkeletalMeshDataByBoneName: TargetBone '%s' not found in SkeletalMesh."), *TargetBoneName.ToString());
        // TargetBoneName이 지정되었으나 찾을 수 없는 경우, 필터링 없이 진행할지 여부 결정 필요. 여기서는 필터링 실패로 간주 가능.
        // 또는 모든 버텍스를 가져오도록 bShouldFilter = false 처리. 여기서는 지정된 본이 없으면 실패로 가정하지 않음 (모든 버텍스 대상 가능)
    }

    const FSkinWeightVertexBuffer* SkinWeightBufferPtr = LODRenderData.GetSkinWeightVertexBuffer();
    if (!SkinWeightBufferPtr || SkinWeightBufferPtr->GetNumVertices() == 0)
    {
        UE_LOG(LogTemp, Error, TEXT("GetFilteredSkeletalMeshDataByBoneName: SkinWeightVertexBuffer is invalid or empty for LOD %d."), LODIndex);
        return false; 
    }
    
    OutOriginalToProcVertexMap.Empty(); // 맵 초기화
    uint32 CurrentProcVertexIndex = 0;

    // 임시: 버텍스 컬러 기본값 설정 (bCopyVertexColors 로직은 유지)
    FLinearColor DefaultColor = FLinearColor::White;
    
    // --- 정적 버퍼 가져오기 (노멀, 탄젠트, UV, 컬러) ---
    // 참고: 이들은 일반적으로 *기본* 메쉬의 데이터이며, 나중에 bRecalculateNormals가 true가 아닌 이상 프레임마다 동적으로 재계산되지 않습니다.
    FStaticMeshVertexBuffers& StaticVertexBuffers = LODRenderData.StaticVertexBuffers;
    uint32 NumVertices = StaticVertexBuffers.PositionVertexBuffer.GetNumVertices(); // 버텍스 수 가져오기

    // 버퍼 크기 적절하게 조정
    //OutNormals.SetNumUninitialized(NumVertices);
    //OutTangents.SetNumUninitialized(NumVertices);
    //OutUV0.SetNumUninitialized(NumVertices);
    
    if (bCopyVertexColors && StaticVertexBuffers.ColorVertexBuffer.IsInitialized())
    {
        // 버텍스 컬러 복사 옵션이 켜져 있고 컬러 버퍼가 초기화된 경우 크기 설정
        OutVertexColors.SetNumUninitialized(NumVertices);
    }
    else
    {
        // 그렇지 않으면 버텍스 컬러 배열 비우기
        OutVertexColors.Empty();
    }
    
    for (const FSkelMeshRenderSection& Section : LODRenderData.RenderSections)
    {
        const uint32 SectionBaseVertexIndex = Section.BaseVertexIndex;
        {
            for (uint32 i = 0; i < Section.NumVertices; ++i)
            {
                const uint32 OriginalSkelVertexIndex = SectionBaseVertexIndex + i;
                if (GetBoneWeightForVertex(OriginalSkelVertexIndex, FoundTargetBoneIndex, &Section, &LODRenderData, SkinWeightBufferPtr) > Threshold)
                {
                    // Vertex 처리
                    const FVector SkinnedVectorPosition = FVector(StaticVertexBuffers.PositionVertexBuffer.VertexPosition(OriginalSkelVertexIndex));
                    OutVertices.Add(SkinnedVectorPosition);
                    
                    // Normal, 버퍼 유형에 따라 FPackedNormal/FVector3f 변환 필요
                    const FVector Normal = FVector(StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(OriginalSkelVertexIndex)); // Z는 노멀 포함
                    OutNormals.Add(Normal);
                    
                    // 외적과 부호를 사용하여 바이노멀(TangentY) 계산
                    // FVector TangentY = FVector::CrossProduct(Normal, TangentX) * TangentSign;
                    // CreateMeshSection용 FProcMeshTangent에는 TangentY를 직접 저장하지 않지만, Normal과 TangentX로부터 파생됨
                    const FVector TangentX = FVector(StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentX(OriginalSkelVertexIndex)); // X는 탄젠트 포함
                    auto ProcMeshTangent = FProcMeshTangent(TangentX, false);
                    ProcMeshTangent.TangentX = TangentX;
                    OutTangents.Add(ProcMeshTangent);
                    
                    // UV (단순화를 위해 UV 채널 1개만 가정)
                    // 버퍼 유형에 따라 FVector2f 변환 필요
                    const FVector2D VertexUV = FVector2D(StaticVertexBuffers.StaticMeshVertexBuffer.GetVertexUV(OriginalSkelVertexIndex, 0));
                    OutUV0.Add(VertexUV);
                    
                    // FilteredVertexIndexMap 업데이트
                    OutOriginalToProcVertexMap.Add(OriginalSkelVertexIndex, CurrentProcVertexIndex);
                    CurrentProcVertexIndex += 1;
                }
            }
        }
    }
    
    // --- 인덱스 재구성 단계 (섹션 구조 유지) ---
    // 원본 인덱스 버퍼 데이터 가져오기
    TArray<uint32> GlobalIndexBuffer;
    LODRenderData.MultiSizeIndexContainer.GetIndexBuffer(GlobalIndexBuffer);

    const int32 NumSections = LODRenderData.RenderSections.Num();
    OutSectionIndices.Empty(NumSections);
    OutSectionIndices.SetNum(NumSections);
    OutSectionMaterialIndices.Empty(NumSections);
    OutSectionMaterialIndices.SetNum(NumSections);
    
    for (int32 SectionIdx = 0; SectionIdx < NumSections; ++SectionIdx)
    {
        const FSkelMeshRenderSection& Section = LODRenderData.RenderSections[SectionIdx];
        OutSectionMaterialIndices[SectionIdx] = Section.MaterialIndex;
        TArray<int32>& CurrentSectionIndices = OutSectionIndices[SectionIdx];
        CurrentSectionIndices.Empty(Section.NumTriangles * 3);

        for (uint32 TriIdx = 0; TriIdx < Section.NumTriangles; ++TriIdx)
        {
            uint32 OriginalVIdx0 = GlobalIndexBuffer[Section.BaseIndex + TriIdx * 3 + 0];
            uint32 OriginalVIdx1 = GlobalIndexBuffer[Section.BaseIndex + TriIdx * 3 + 1];
            uint32 OriginalVIdx2 = GlobalIndexBuffer[Section.BaseIndex + TriIdx * 3 + 2];

            const uint32* ProcVIdx0Ptr = OutOriginalToProcVertexMap.Find(OriginalVIdx0);
            const uint32* ProcVIdx1Ptr = OutOriginalToProcVertexMap.Find(OriginalVIdx1);
            const uint32* ProcVIdx2Ptr = OutOriginalToProcVertexMap.Find(OriginalVIdx2);

            if (ProcVIdx0Ptr && ProcVIdx1Ptr && ProcVIdx2Ptr) // 세 버텍스 모두 필터링을 통과했으면
            {
                CurrentSectionIndices.Add(*ProcVIdx0Ptr);
                CurrentSectionIndices.Add(*ProcVIdx1Ptr);
                CurrentSectionIndices.Add(*ProcVIdx2Ptr);
            }
        }
    }
    bool bFoundAnyTriangles = false;
    for(const auto& Indices : OutSectionIndices)
    {
        if (Indices.Num() > 0)
        {
            bFoundAnyTriangles = true;
            break;
        }
    }
    if (!bFoundAnyTriangles && OutVertices.Num() > 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("GetFilteredSkeletalMeshDataByBoneName: No triangles remained after filtering for TargetBone '%s'."), *TargetBoneName.ToString());
    }
    return OutVertices.Num() > 0 && bFoundAnyTriangles; // 버텍스와 트라이앵글이 모두 있어야 성공
}

bool USkelToProcMeshComponent::SliceMesh(UProceduralMeshComponent* InProcMesh, FVector PlanePosition, FVector PlaneNormal, bool bCreateOtherHalf, UProceduralMeshComponent*& OutOtherHalfProcMesh,
    EProcMeshSliceCapOption CapOption, UMaterialInterface* CapMaterial)
{
    UKismetProceduralMeshLibrary::SliceProceduralMesh(InProcMesh, PlanePosition, PlaneNormal, bCreateOtherHalf, OutOtherHalfProcMesh, CapOption, CapMaterial);
    if (OutOtherHalfProcMesh != nullptr)
    {
        return true;
    }
    return false;
}

float USkelToProcMeshComponent::GetBoneWeightForVertex(int32 VertexIndex, int32 TargetBoneIndex, const FSkelMeshRenderSection* SkelMeshRenderSection, const FSkeletalMeshLODRenderData* LODRenderData,
    const FSkinWeightVertexBuffer* SkinWeightBuffer)
{
    if (!SkinWeightBuffer || SkinWeightBuffer->GetNumVertices() == 0) return -1.f;
    const int32 MaxInfluences = SkinWeightBuffer->GetMaxBoneInfluences();
    const TArray<FBoneIndexType>& BoneMap = SkelMeshRenderSection->BoneMap;
    for (int32 InfluenceIdx = 0; InfluenceIdx < MaxInfluences; InfluenceIdx++)
    {
        int32 LocalBoneIndex = SkinWeightBuffer->GetBoneIndex(VertexIndex, InfluenceIdx);
        if (LocalBoneIndex >= BoneMap.Num()) continue; // Ensure we are within bounds
        
        int32 GlobalBoneIndex = BoneMap[LocalBoneIndex];
        float BoneWeight = SkinWeightBuffer->GetBoneWeight(VertexIndex, InfluenceIdx) / 65535.0f;
        
        if (GlobalBoneIndex == TargetBoneIndex && BoneWeight > 0)
        {
            return BoneWeight;
        }
    }
    return -1.f;
}

USkeletalMeshComponent* USkelToProcMeshComponent::GetOwnerSkeletalMeshComponent() const
{
    AActor* Owner = GetOwner();
    if (!Owner)
    {
        UE_LOG(LogTemp, Warning, TEXT("SkelToProcMeshComponent: Owner를 찾을 수 없습니다."));
        return nullptr;
    }
    USkeletalMeshComponent* SkelComp = Owner->FindComponentByClass<USkeletalMeshComponent>();
    if (!SkelComp)
    {
        UE_LOG(LogTemp, Warning, TEXT("SkelToProcMeshComponent: Owner '%s'에 SkeletalMeshComponent가 없습니다."), *Owner->GetName());
        return nullptr;
    }
    if (!SkelComp->GetSkeletalMeshAsset())
    {
        UE_LOG(LogTemp, Warning, TEXT("SkelToProcMeshComponent: '%s'의 SkeletalMeshComponent에 Skeletal Mesh Asset이 할당되지 않았습니다."), *Owner->GetName());
        return nullptr;
    }
    return SkelComp;
}

bool USkelToProcMeshComponent::HideOriginalMeshVerticesByBone(USkeletalMeshComponent* SourceSkeletalMeshComp, int32 LODIndex, FName TargetBoneName, bool bClearOverride)
{
    if (!SourceSkeletalMeshComp || !SourceSkeletalMeshComp->GetSkeletalMeshAsset())
    {
        UE_LOG(LogTemp, Warning, TEXT("HideOriginalMeshVerticesByBone: Invalid Skeletal Mesh Component or Asset."));
        return false;
    }

    USkeletalMesh* SkelMesh = SourceSkeletalMeshComp->GetSkeletalMeshAsset();
    const FReferenceSkeleton& RefSkeleton = SkelMesh->GetRefSkeleton();
    int32 TargetBoneIndex = RefSkeleton.FindBoneIndex(TargetBoneName);

    if (TargetBoneIndex == INDEX_NONE)
    {
        UE_LOG(LogTemp, Warning, TEXT("HideOriginalMeshVerticesByBone: Bone '%s' not found."), *TargetBoneName.ToString());
        return false;
    }

    // FSkeletalMeshRenderData 접근은 게임 스레드에서 직접 하면 위험할 수 있으나, 읽기 목적 및 컴포넌트 상태 기반이므로
    // 일부 상황에서는 가능할 수 있습니다. 복잡한 경우 렌더 스레드 접근 고려.
    FSkeletalMeshRenderData* SkelRenderData = SourceSkeletalMeshComp->GetSkeletalMeshRenderData();
    if (!SkelRenderData || SkelRenderData->LODRenderData[LODIndex].GetNumVertices() == 0)
    {
         UE_LOG(LogTemp, Warning, TEXT("HideOriginalMeshVerticesByBone: Invalid LOD Index %d."), LODIndex);
        return false;
    }

    FSkeletalMeshLODRenderData& LODRenderData = SkelRenderData->LODRenderData[LODIndex];
    FSkinWeightVertexBuffer* SkinWeightBuffer = SourceSkeletalMeshComp->GetSkinWeightBuffer(LODIndex); // 인스턴스별 버퍼 가져오기

    if (!SkinWeightBuffer)
    {
         UE_LOG(LogTemp, Warning, TEXT("HideOriginalMeshVerticesByBone: Could not get SkinWeightVertexBuffer for LOD %d."), LODIndex);
        return false;
    }

    uint32 NumVertices = LODRenderData.GetNumVertices();
    if (NumVertices == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("HideOriginalMeshVerticesByBone: LOD %d has no vertices."), LODIndex);
        return false;
    }

    TArray<int32> VerticesToHideIndices;
    VerticesToHideIndices.Reserve(NumVertices / 4); // 예상 크기 예약

    // 1. 숨길 버텍스 식별 (GetBoneWeightForVertex 구현 필요)
    for (uint32 VertexIndex = 0; VertexIndex < NumVertices; ++VertexIndex)
    {
        // GetBoneWeightForVertex 함수는 FSkinWeightVertexBuffer 구조에 맞춰 구현해야 합니다.
        // 아래는 가상의 함수 호출입니다. 실제 구현은 SkinWeightBuffer->GetWeightForVertex 등을 사용해야 합니다.
        float Weight = GetBoneWeightForVertex(VertexIndex, TargetBoneIndex, &LODRenderData.RenderSections[LODIndex], &LODRenderData, SkinWeightBuffer);

        if (Weight > Threshold)
        {
            VerticesToHideIndices.Add(VertexIndex);
        }
    }

    // 2. 숨길 버텍스가 있는 경우에만 오버라이드 적용
    if (VerticesToHideIndices.Num() > 0)
    {
        // 3. 버텍스 컬러 오버라이드 배열 준비
        TArray<FLinearColor> OverrideColors;
        OverrideColors.SetNum(NumVertices);

        // 기존 컬러 버퍼 확인
        const FColorVertexBuffer* ExistingColorBuffer = LODRenderData.StaticVertexBuffers.ColorVertexBuffer.IsInitialized() ?
                                                        &LODRenderData.StaticVertexBuffers.ColorVertexBuffer : nullptr;

        // 배열 초기화 (기존 색상 또는 흰색)
        for (uint32 i = 0; i < NumVertices; ++i)
        {
            if (ExistingColorBuffer && i < ExistingColorBuffer->GetNumVertices())
            {
                OverrideColors[i] = ExistingColorBuffer->VertexColor(i).ReinterpretAsLinear();
            }
            else
            {
                OverrideColors[i] = FLinearColor::White; // 기본값 (Alpha = 1)
            }
        }

        // 4. 숨길 버텍스의 알파 값을 0으로 설정
        for (int32 HideIndex : VerticesToHideIndices)
        {
            if (OverrideColors.IsValidIndex(HideIndex))
            {
                OverrideColors[HideIndex].A = 0.0f; // 알파를 0으로 만들어 숨김
            }
        }

        // 5. 컴포넌트에 오버라이드 적용
        SourceSkeletalMeshComp->SetVertexColorOverride_LinearColor(LODIndex, OverrideColors);
         UE_LOG(LogTemp, Log, TEXT("Applied vertex color override to hide %d vertices for bone '%s' on LOD %d."), VerticesToHideIndices.Num(), *TargetBoneName.ToString(), LODIndex);
        return true;
    }
    else if (bClearOverride)
    {
        // 숨길 버텍스가 없고, 이전 오버라이드를 지우도록 설정된 경우
        SourceSkeletalMeshComp->ClearVertexColorOverride(LODIndex);
        UE_LOG(LogTemp, Log, TEXT("No vertices to hide for bone '%s' on LOD %d. Cleared override (if any)."), *TargetBoneName.ToString(), LODIndex);
        return true; // 작업은 성공적으로 완료됨 (숨길 것이 없었음)
    }

    return true; // 숨길 것이 없었음
}

bool USkelToProcMeshComponent::BuildSkinningDataForProceduralMesh(
    const USkeletalMeshComponent* SkelComp,
    int32 LODIndex,
    const TArray<FVector>& InProcMeshVertices, // 이 버텍스들은 로컬 바인드 포즈 위치
    const TMap<uint32, uint32>& InOriginalToProcVertexMap, // Key: OriginalSkelVIdx, Value: ProcMeshVIdx (InProcMeshVertices 배열의 인덱스)
    TArray<FProceduralVertexSkinningData>& OutSkinningData)
{
    if (!SkelComp || !SkelComp->GetSkeletalMeshAsset()) return false;

    FSkeletalMeshRenderData* RenderData = SkelComp->GetSkeletalMeshAsset()->GetResourceForRendering();
    if (!RenderData || !RenderData->LODRenderData.IsValidIndex(LODIndex)) return false;

    const FSkeletalMeshLODRenderData& LODRenderData = RenderData->LODRenderData[LODIndex];
    const FSkinWeightVertexBuffer* SkinWeightBuffer = LODRenderData.GetSkinWeightVertexBuffer();

    if (!SkinWeightBuffer || SkinWeightBuffer->GetNumVertices() == 0)
    {
        UE_LOG(LogTemp, Error, TEXT("BuildSkinningDataForProceduralMesh: SkinWeightVertexBuffer is invalid or empty for LOD %d."), LODIndex);
        return false;
    }

    OutSkinningData.Empty(InProcMeshVertices.Num());
    OutSkinningData.SetNum(InProcMeshVertices.Num()); // 프로시저럴 메시 버텍스 수만큼 초기화

    // InOriginalToProcVertexMap은 (Original Skel VIdx -> Proc VIdx) 이므로, 역방향 맵이 필요하거나 순회 방식 변경 필요.
    // 또는, ProcMesh 버텍스를 순회하면서 Original Skel VIdx를 찾아야 함.

    // 현재 InOriginalToProcVertexMap (Key: OriginalSkelVertexIdx, Value: ProcMeshVertexIdx)를 사용한다고 가정.
    // 이 맵은 모든 '필터링된' 원본 버텍스에 대한 정보만 담고 있음.
    // OutSkinningData는 ProcMesh 버텍스 순서대로 채워져야 함.
    // 따라서 ProcMesh 버텍스를 0부터 N-1까지 순회하면서, 각 ProcMesh 버텍스에 해당하는 OriginalSkelVertexIdx를 찾아야 함.

    // InOriginalToProcVertexMap의 Value를 기준으로 OriginalSkelVertexIdx를 찾는 것은 비효율적.
    // ProcMesh 버텍스 배열(InProcMeshVertices)을 만들 때, 각 ProcMesh 버텍스가 어떤 OriginalSkelVertexIdx에서 왔는지
    // 별도의 배열(ProcToOriginalVertexMap[ProcVIdx] = OriginalSkelVIdx)로 저장하는 것이 더 효율적.
    // 현재 OriginalToMainProcVertexMap (멤버변수)이 (OriginalSkelVIdx -> MainProcVIdx)로 채워짐.

    for (auto const& Pair : OriginalToMainProcVertexMap) // OriginalToMainProcVertexMap은 (Original Idx -> Proc Idx)
    {
        uint32 OriginalSkelVertexIdx = Pair.Key;
        uint32 ProcMeshVertexIdx = Pair.Value; // 이 인덱스가 OutSkinningData 배열의 인덱스가 됨

        if (OutSkinningData.IsValidIndex(ProcMeshVertexIdx))
        {
            OutSkinningData[ProcMeshVertexIdx].LocalBindPosePosition = InProcMeshVertices[ProcMeshVertexIdx]; // 이미 로컬 위치로 제공됨
            if (!GetSkinWeightsForOriginalVertex(SkelComp, OriginalSkelVertexIdx, LODRenderData, SkinWeightBuffer, OutSkinningData[ProcMeshVertexIdx]))
            {
                UE_LOG(LogTemp, Warning, TEXT("BuildSkinningData: Failed to get skin weights for OriginalSkelVertexIdx %u (ProcMeshVertexIdx %u)"), OriginalSkelVertexIdx, ProcMeshVertexIdx);
                // 스키닝 정보가 없는 버텍스는 어떻게 처리할지? (예: 루트 본에 고정, 스키닝 안함 등)
                // 여기서는 일단 LocalBindPosePosition만 설정된 상태로 둠.
            }
        }
        else
        {
            UE_LOG(LogTemp, Error, TEXT("BuildSkinningData: ProcMeshVertexIdx %u from map is out of bounds for OutSkinningData (Size: %d)"), ProcMeshVertexIdx, OutSkinningData.Num());
        }
    }

    return true;
}

bool USkelToProcMeshComponent::GetSkinWeightsForOriginalVertex(
    const USkeletalMeshComponent* SkelComp,
    uint32 OriginalSkelVertexIndex,
    const FSkeletalMeshLODRenderData& LODRenderData,
    const FSkinWeightVertexBuffer* SkinWeightBuffer,
    FProceduralVertexSkinningData& OutVertexSkinningData) // BoneMapIndices와 BoneWeights를 채움
{
    if (!SkinWeightBuffer || OriginalSkelVertexIndex >= SkinWeightBuffer->GetNumVertices()) return false;

    const int32 MaxInfluences = SkinWeightBuffer->GetMaxBoneInfluences();
    const FReferenceSkeleton& RefSkeleton = SkelComp->GetSkeletalMeshAsset()->GetRefSkeleton();

    OutVertexSkinningData.BoneMapIndices.Empty(MaxInfluences);
    OutVertexSkinningData.BoneWeights.Empty(MaxInfluences);

    float TotalWeight = 0.f;

    for (int32 InfluenceIdx = 0; InfluenceIdx < MaxInfluences; ++InfluenceIdx)
    {
        uint16 RawWeight = SkinWeightBuffer->GetBoneWeight(OriginalSkelVertexIndex, InfluenceIdx);
        if (RawWeight > 0) // 가중치가 0인 본은 무시
        {
            // SkinWeightBuffer.GetBoneIndex()는 LODRenderData.ActiveBoneIndices 배열에 대한 인덱스를 반환.
            FBoneIndexType LODActiveBoneIndex = SkinWeightBuffer->GetBoneIndex(OriginalSkelVertexIndex, InfluenceIdx);

            if (LODRenderData.ActiveBoneIndices.IsValidIndex(LODActiveBoneIndex))
            {
                // ActiveBoneIndices 배열의 값은 실제 RefSkeleton에 대한 본 인덱스.
                FBoneIndexType SkeletonBoneIndex = LODRenderData.ActiveBoneIndices[LODActiveBoneIndex];
                
                OutVertexSkinningData.BoneMapIndices.Add(SkeletonBoneIndex);
                float NormalizedWeight = static_cast<float>(RawWeight) / 255.0f; // GetMaxBoneWeight()는 보통 255.0f
                OutVertexSkinningData.BoneWeights.Add(NormalizedWeight);
                TotalWeight += NormalizedWeight;
            }
            else
            {
                UE_LOG(LogTemp, Warning, TEXT("GetSkinWeightsForOriginalVertex: Invalid LODActiveBoneIndex %d from SkinWeightBuffer for OriginalSkelVertexIndex %u"), LODActiveBoneIndex, OriginalSkelVertexIndex);
            }
        }
    }
    
    // 가중치 합계가 1에 가깝도록 정규화 (선택 사항, 보통 이미 정규화되어 있음)
    if (TotalWeight > KINDA_SMALL_NUMBER && FMath::Abs(TotalWeight - 1.0f) > KINDA_SMALL_NUMBER)
    {
        //UE_LOG(LogTemp, Verbose, TEXT("Normalizing weights for vertex %u. Original total: %f"), OriginalSkelVertexIndex, TotalWeight);
        for (int i = 0; i < OutVertexSkinningData.BoneWeights.Num(); ++i)
        {
            OutVertexSkinningData.BoneWeights[i] /= TotalWeight;
        }
    }

    return OutVertexSkinningData.BoneMapIndices.Num() > 0;
}


void USkelToProcMeshComponent::UpdateProceduralMeshesSkinning()
{
    USkeletalMeshComponent* SkelComp = GetOwnerSkeletalMeshComponent();
    if (!SkelComp || !SkelComp->GetSkeletalMeshAsset() || RefBoneInverseBindMatrices.Num() == 0)
    {
        //UE_LOG(LogTemp, Verbose, TEXT("UpdateProceduralMeshesSkinning: Prerequisites not met (SkelComp, Asset, or InvBindMatrices)."));
        return;
    }

    // 현재 본 트랜스폼 (컴포넌트 공간)
    const TArray<FTransform>& CurrentBoneTransforms = SkelComp->GetBoneSpaceTransforms();
    if (CurrentBoneTransforms.Num() == 0)
    {
        //UE_LOG(LogTemp, Verbose, TEXT("UpdateProceduralMeshesSkinning: CurrentBoneTransforms is empty."));
        return;
    }

    auto PerformSkinning = [&](UProceduralMeshComponent* ProcMesh, const TArray<FProceduralVertexSkinningData>& SkinningData)
    {
        if (!ProcMesh || ProcMesh->GetNumSections() == 0 || SkinningData.Num() == 0) return;

        // 첫 번째 섹션의 버텍스 수와 스키닝 데이터 수가 일치하는지 확인 (간단한 경우)
        // 실제로는 모든 섹션의 버텍스를 합한 수와 일치해야 하거나, 섹션별로 스키닝 데이터를 가져야 함.
        // 현재 스키닝 데이터는 전체 프로시저럴 메시에 대해 하나의 배열로 관리.
        // CreateMeshSection_LinearColor에서 버텍스 배열을 전체로 넘겼다면 이 방식이 맞음.
        
        FProcMeshSection* Section = ProcMesh->GetProcMeshSection(0); // 첫번째 섹션으로 가정
        if (!Section || Section->ProcVertexBuffer.Num() != SkinningData.Num())
        {
            //UE_LOG(LogTemp, Warning, TEXT("UpdateProceduralMeshesSkinning: Vertex count mismatch for ProcMesh %s. Section0 Verts: %d, SkinData Verts: %d"),*ProcMesh->GetName(), Section ? Section->ProcVertexBuffer.Num() : -1, SkinningData.Num());
            // return; // 수가 다르면 일단 스키닝 중단 또는 다른 섹션도 확인
        }


        TArray<FVector> NewSkinnedVertexPositions;
        NewSkinnedVertexPositions.SetNumUninitialized(SkinningData.Num());

        // 노멀과 탄젠트도 스키닝하려면 추가 배열 필요
        TArray<FVector> NewSkinnedNormals;
        TArray<FProcMeshTangent> NewSkinnedTangents;
        bool bSkinNormalsAndTangents = true; // 필요에 따라 false로 설정 가능

        if (bSkinNormalsAndTangents)
        {
            NewSkinnedNormals.SetNumUninitialized(SkinningData.Num());
            NewSkinnedTangents.SetNumUninitialized(SkinningData.Num());
        }


        for (int32 VertexIdx = 0; VertexIdx < SkinningData.Num(); ++VertexIdx)
        {
            const FProceduralVertexSkinningData& VertexSkinData = SkinningData[VertexIdx];
            FVector SkinnedPosition = FVector::ZeroVector;
            FVector SkinnedNormal = FVector::ZeroVector;   // (0,0,1) 또는 원본 노멀로 초기화 가능
            FVector SkinnedTangentX = FVector::ZeroVector; // (1,0,0) 또는 원본 탄젠트로 초기화 가능

            // 원본 버텍스의 노멀/탄젠트 가져오기 (스키닝 데이터에 저장되어 있지 않으므로, ProcMesh에서 읽거나, 바인드 포즈 값을 알아야 함)
            // 여기서는 간단히 바인드 포즈의 Z축/X축으로 가정하거나, 실제 값을 가져와야 함.
            // GetFilteredSkeletalMeshDataByBoneName에서 Normals_Main, Tangents_Main을 가져왔으므로,
            // 이 값들을 FProceduralVertexSkinningData에 추가로 저장해두는 것이 좋음. (LocalBindPoseNormal, LocalBindPoseTangent)
            // 지금은 LocalBindPosePosition만 있음.
            // 임시로, 바인드 포즈 노멀/탄젠트는 ProceduralMeshComponent에서 직접 읽어온다고 가정 (최초 생성 시의 값)
            // 또는, SkinningData 구조체에 바인드 포즈 노멀/탄젠트도 저장해야 함.

            FVector OriginalNormal = FVector::UpVector; // 임시 기본값
            FVector OriginalTangentX = FVector::RightVector; // 임시 기본값

            if (Section && Section->ProcVertexBuffer.IsValidIndex(VertexIdx))
            {
                 // ProceduralMeshComponent는 스키닝 전의 바인드포즈 데이터를 가지고 있어야 함.
                 // 만약 매번 UpdateMeshSection으로 덮어쓰면 이 값은 현재 스키닝된 값이 됨.
                 // 따라서 LocalBindPosePosition 등을 사용.
                 // 노멀/탄젠트도 스키닝 데이터에 LocalBindPoseNormal/Tangent 로 저장하는 것이 좋음.
                 // 현재는 없으므로, 스키닝 정보의 LocalBindPosePosition으로 위치를 가져오고,
                 // 노멀/탄젠트는 해당 버텍스의 원래 값 (예: ProcMesh->GetProcMeshSection(0)->ProcVertexBuffer[VertexIdx].Normal)을 사용한다고 가정.
                 // 하지만 이 값은 이전 프레임에 스키닝된 값일 수 있으므로 주의.
                 // ===> FProceduralVertexSkinningData에 LocalBindPoseNormal, LocalBindPoseTangentX 추가 필요!

                 // 임시: 원본 노멀/탄젠트가 없으므로 스키닝 시 기본값 사용하거나, 위치만 스키닝
            }


            for (int32 InfluenceIdx = 0; InfluenceIdx < VertexSkinData.BoneMapIndices.Num(); ++InfluenceIdx)
            {
                int32 BoneMapIndex = VertexSkinData.BoneMapIndices[InfluenceIdx];
                float BoneWeight = VertexSkinData.BoneWeights[InfluenceIdx];

                if (CurrentBoneTransforms.IsValidIndex(BoneMapIndex) && RefBoneInverseBindMatrices.IsValidIndex(BoneMapIndex))
                {
                    const FMatrix InverseBindMatrix = RefBoneInverseBindMatrices[BoneMapIndex];
                    const FMatrix CurrentBoneMatrix = CurrentBoneTransforms[BoneMapIndex].ToMatrixWithScale();
                    
                    // 최종 스키닝 매트릭스 (로컬 바인드 -> 현재 컴포넌트 공간)
                    FMatrix FinalSkinMatrix = InverseBindMatrix * CurrentBoneMatrix;

                    SkinnedPosition += FinalSkinMatrix.TransformPosition(VertexSkinData.LocalBindPosePosition) * BoneWeight;

                    if (bSkinNormalsAndTangents)
                    {
                        // 노멀과 탄젠트는 방향 벡터이므로 TransformVector 사용 (Non-uniform scale 시 문제 가능성, 정규화 필요)
                        // FProceduralVertexSkinningData 에 LocalBindPoseNormal, LocalBindPoseTangentX 가 있다고 가정하고 추가해야 함.
                        // SkinnedNormal += FinalSkinMatrix.TransformVector(VertexSkinData.LocalBindPoseNormal).GetSafeNormal() * BoneWeight;
                        // SkinnedTangentX += FinalSkinMatrix.TransformVector(VertexSkinData.LocalBindPoseTangentX).GetSafeNormal() * BoneWeight;
                    }
                }
            }
            NewSkinnedVertexPositions[VertexIdx] = SkinnedPosition;
            if (bSkinNormalsAndTangents)
            {
                // NewSkinnedNormals[VertexIdx] = SkinnedNormal.GetSafeNormal();
                // NewSkinnedTangents[VertexIdx] = FProcMeshTangent(SkinnedTangentX.GetSafeNormal(), false); // Assume bFlipTangentY is false
                // 임시: 노멀/탄젠트 스키닝은 FProceduralVertexSkinningData에 원본 값 추가 후 구현
                 if (Section && Section->ProcVertexBuffer.IsValidIndex(VertexIdx))
                 {
                    NewSkinnedNormals[VertexIdx] = Section->ProcVertexBuffer[VertexIdx].Normal; // 임시로 기존 값 유지
                    NewSkinnedTangents[VertexIdx] = Section->ProcVertexBuffer[VertexIdx].Tangent; // 임시로 기존 값 유지
                 }
                 else
                 {
                    NewSkinnedNormals[VertexIdx] = FVector::UpVector;
                    NewSkinnedTangents[VertexIdx] = FProcMeshTangent(FVector::RightVector, false);
                 }

            }
        }

        // 프로시저럴 메시 섹션 업데이트 (현재는 첫 번째 섹션만)
        // UV, VertexColor 등은 업데이트하지 않으므로 빈 배열 전달
        if (bSkinNormalsAndTangents)
        {
            ProcMesh->UpdateMeshSection_LinearColor(0, NewSkinnedVertexPositions, NewSkinnedNormals,
                                                TArray<FVector2D>(), TArray<FLinearColor>(), NewSkinnedTangents);
        }
        else
        {
            // 위치만 업데이트
            TArray<FVector> EmptyNormals;
            TArray<FProcMeshTangent> EmptyTangents;
             ProcMesh->UpdateMeshSection_LinearColor(0, NewSkinnedVertexPositions, EmptyNormals,
                                                 TArray<FVector2D>(), TArray<FLinearColor>(), EmptyTangents);
        }
    };

    // 메인 프로시저럴 메시 스키닝
    PerformSkinning(ProceduralMeshComponent, MainProcMeshSkinningData);

    // 다른 쪽 프로시저럴 메시 스키닝
    if (OtherHalfProceduralMeshComponent) // OtherHalfProcMeshSkinningData가 빌드되었다면
    {
        // PerformSkinning(OtherHalfProceduralMeshComponent, OtherHalfProcMeshSkinningData);
        // 현재 OtherHalfProcMeshSkinningData는 빌드 로직이 없으므로 호출하지 않음.
    }
}




 
bool USkelToProcMeshComponent::SetupProceduralMeshComponent(bool bForceNew)
{
    AActor* Owner = GetOwner();
    if (!Owner) return false;

    if (bForceNew && ProceduralMeshComponent)
    {
        // 강제로 새로 생성하는 경우 기존 PMC 파괴
        ProceduralMeshComponent->DestroyComponent();
        ProceduralMeshComponent = nullptr;
    }

    if (!ProceduralMeshComponent)
    {
        // 할당되지 않은 경우 기존 컴포넌트를 먼저 찾아봅니다.
        ProceduralMeshComponent = Owner->FindComponentByClass<UProceduralMeshComponent>();

        if(!ProceduralMeshComponent)
        {
             // 새 PMC 생성
            ProceduralMeshComponent = NewObject<UProceduralMeshComponent>(Owner, TEXT("GeneratedProceduralMesh"));
            if (ProceduralMeshComponent)
            {
                ProceduralMeshComponent->RegisterComponent();
                // 원하는 경우 씬 루트나 다른 컴포넌트에 어태치합니다.
                USceneComponent* OwnerRoot = Owner->GetRootComponent();
                if(OwnerRoot)
                {
                    ProceduralMeshComponent->AttachToComponent(OwnerRoot, FAttachmentTransformRules::KeepRelativeTransform);
                }
                UE_LOG(LogTemp, Log, TEXT("SkelToProcMeshComponent: 새 ProceduralMeshComponent를 생성했습니다."));
            }
            else
            {
                 UE_LOG(LogTemp, Error, TEXT("SkelToProcMeshComponent: ProceduralMeshComponent 생성에 실패했습니다."));
                 return false;
            }
        }
         else
        {
             UE_LOG(LogTemp, Log, TEXT("SkelToProcMeshComponent: 기존 ProceduralMeshComponent를 찾았습니다."));
        }
    }

    // 이전 지오메트리 제거
    ProceduralMeshComponent->ClearAllMeshSections();
    
    return (ProceduralMeshComponent != nullptr);
}
