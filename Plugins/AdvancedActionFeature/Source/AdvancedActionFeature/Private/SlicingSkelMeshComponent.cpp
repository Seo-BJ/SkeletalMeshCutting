#include "SlicingSkelMeshComponent.h"

#include "KismetProceduralMeshLibrary.h"
#include "Components/SkeletalMeshComponent.h"
#include "ProceduralMeshComponent.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h" 
#include "DrawDebugHelpers.h"
#include "GeometryCollection/Facades/CollectionConstraintOverrideFacade.h"

USlicingSkelMeshComponent::USlicingSkelMeshComponent()
{
    PrimaryComponentTick.bCanEverTick = true; // 틱 활성화
    PrimaryComponentTick.bStartWithTickEnabled = true;
}

void USlicingSkelMeshComponent::BeginPlay()
{
    Super::BeginPlay();

    if (bConvertOnBeginPlay)
    {
        SliceMesh(false);
    }
}

void USlicingSkelMeshComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    if (bPerformVertexSkinning)
    {
        PerformEntireVertexSkinning(ProceduralMeshComponent, false);
        PerformEntireVertexSkinning(OtherHalfProceduralMeshComponent, true);
    }

    
    //PerformVertexSkinning(ProceduralMeshComponent, AttachBoneForOriginalPMC, false);
    //PerformVertexSkinning(OtherHalfProceduralMeshComponent, AttachBoneForOtherHalf, true);
}


bool USlicingSkelMeshComponent::SliceMesh(bool bForceNewPMC)
{
    USkeletalMeshComponent* SkelComp = GetOwnerSkeletalMeshComponent();
    if (!SkelComp || !SkelComp->GetSkeletalMeshAsset() || !SkelComp->GetSkeletalMeshAsset()->GetResourceForRendering()) return false;
    if (!SetupProceduralMeshComponent(bForceNewPMC))
    {
        UE_LOG(LogTemp, Error, TEXT("SkelToProcMeshComponent: Procedural Mesh Component 설정에 실패했습니다. 변환할 수 없습니다."));
        return false;
    }

    // ProceduralMeshComponent->SetWorldLocation(SkelComp->GetComponentLocation());
    // ProceduralMeshComponent->SetWorldRotation(SkelComp->GetComponentRotation());

    bool bSuccess = SliceMeshInternal(SkelComp);
    return bSuccess;
}


void USlicingSkelMeshComponent::MakeProcVerticiesIndexMap(const TMap<uint32, uint32>& SlicedPmcToPmcMap, TMap<uint32, uint32>& ResultMap)
{
    for (const TPair<uint32, uint32>& Pair : SlicedPmcToPmcMap)
    {
        int32 SlicedPMC_VertexIndex_Int = Pair.Key;
        int32 OriginalPMC_VertexIndex_Int = Pair.Value;
        
        if (OriginalPMC_VertexIndex_Int < 0)
        {
            continue;
        }
        
        // PMC_SkeletalVerticesMap에서 OriginalPMC_VertexIndex_Uint를 키로 사용하여
        // 원본 스켈레탈 메시의 버텍스 인덱스를 찾습니다.

        if (const uint32* SkeletalMesh_VertexIndex_Ptr = PMC_To_Skel_VerticesMap.Find(OriginalPMC_VertexIndex_Int))
        {
            uint32 SkeletalMesh_VertexIndex = *SkeletalMesh_VertexIndex_Ptr;
            ResultMap.Add(SlicedPMC_VertexIndex_Int, SkeletalMesh_VertexIndex);
        }
        else
        {
        }
    }
}

void USlicingSkelMeshComponent::SaveCuttingSurfaceVertices(UProceduralMeshComponent* ProcMesh, bool bOtherHalf)
{
    if (ProcMesh == nullptr) return;
    for (int32 SectionIndex = 0; SectionIndex < ProcMesh->GetNumSections(); SectionIndex++)
    {
        const FProcMeshSection* Section = ProcMesh->GetProcMeshSection(SectionIndex);
        if (!Section || Section->ProcVertexBuffer.Num() == 0) // 섹션 유효성 검사 강화
        {
            continue;
        }
        const TArray<FProcMeshVertex>& CurrentProcMeshVertices = Section->ProcVertexBuffer;
        for (int32 ProcVertexIndex = 0; ProcVertexIndex < CurrentProcMeshVertices.Num(); ProcVertexIndex++)
        {
            const FProcMeshVertex& CurrentVertex = CurrentProcMeshVertices[ProcVertexIndex];
            // 1. 이 정점에 대한 FBoneWeightsInfo 가져오기
            FBoneWeightsInfo* VertexBoneWeightsInfo = nullptr;
            uint32* OriginalSkelGlobalIndexUint32 = nullptr;
   
            if (bOtherHalf)
            {
                OriginalSkelGlobalIndexUint32 = OtherHalfPMC_To_Skel_VerticesMap.Find(ProcVertexIndex);
            }
            else
            {
                OriginalSkelGlobalIndexUint32 = SlicedPMC_To_Skel_VerticesMap.Find(ProcVertexIndex);
            }
            
            if (! (VertexBoneWeightsInfo && VertexBoneWeightsInfo->InfluencingBoneIndices.Num() && OriginalSkelGlobalIndexUint32 != nullptr)) 
            {
                if (bOtherHalf)
                {
                    OtherHalfPMC_CuttingSurfaceVertices.Add(ProcVertexIndex, CurrentVertex);
                }
                else
                {
                    SlicedPMC_CuttingSurfaceVertices.Add(ProcVertexIndex, CurrentVertex);
                }
            }
        }
    }
}

bool USlicingSkelMeshComponent::SliceMeshInternal(USkeletalMeshComponent* SkelComp)
{
    // --- 1. 메쉬 데이터 추출 ---
    TArray<FVector> Vertices;           // 버텍스 위치 배열
    TArray<FVector> Normals;            // 노멀 배열
    TArray<FProcMeshTangent> Tangents;  // 탄젠트 배열
    TArray<FVector2D> UV0;              // UV0 배열
    TArray<FLinearColor> VertexColors;  // 버텍스 컬러 배열
    TArray<int32> SectionMaterialIndices; // 각 섹션에서 사용하는 머티리얼 인덱스
    TArray<TArray<int32>> SectionIndices; // 각 섹션의 트라이앵글 인덱스

    bool bDataExtracted = GetFilteredSkeletalMeshDataByBoneName(SkelComp, 0.f, Vertices, Normals, Tangents, UV0, VertexColors, SectionMaterialIndices, SectionIndices);
    
     // --- 선택 사항: 노멀 재계산 ---
    if (bRecalculateNormals)
    {
        // 참고: 여기서 노멀을 재계산하는 것은 느릴 수 있습니다. 모든 섹션을 일시적으로 결합해야 합니다.
        // 이 기본 예제는 *전체* 버텍스 버퍼를 기반으로 재계산하므로, 섹션 사이에 하드 에지가 있는 경우 이상적이지 않을 수 있습니다.
        // 더 견고한 솔루션은 섹션별로 재계산하거나 더 고급 노멀 계산 라이브러리를 사용하는 것입니다.

        // 계산을 위해 인덱스 결합 (모두 동일한 버텍스 버퍼를 참조한다고 가정)
        TArray<int32> AllIndices;
        for(const TArray<int32>& Indices : SectionIndices)
        {
            AllIndices.Append(Indices);
        }

        if(AllIndices.Num() > 0)
        {
            // 노멀과 탄젠트 모두 재계산
            UKismetProceduralMeshLibrary::CalculateTangentsForMesh(Vertices, AllIndices, UV0, Normals, Tangents);
            UE_LOG(LogTemp, Log, TEXT("SkelToProcMeshComponent: LOD %d의 노멀과 탄젠트를 재계산했습니다."), TargetLODIndex);
        }
    }
    
    // --- 2. Procedural Mesh 섹션 생성 ---
    for (int32 SectionIdx = 0; SectionIdx < SectionIndices.Num(); ++SectionIdx)
    {
        if (SectionIndices[SectionIdx].Num() > 0) // 해당 섹션에 인덱스가 있는 경우에만 생성
        {
            // 메쉬 섹션 생성
            ProceduralMeshComponent->CreateMeshSection_LinearColor(
                SectionIdx,                     // 섹션 인덱스
                Vertices,                       // 이 LOD의 모든 버텍스
                SectionIndices[SectionIdx],     // 이 섹션의 트라이앵글 인덱스
                Normals,                        // 모든 버텍스의 노멀
                UV0,                            // 모든 버텍스의 UV0
                VertexColors,                   // 모든 버텍스의 버텍스 컬러 (선택 사항)
                Tangents,                       // 모든 버텍스의 탄젠트
                false                           // bCreateCollision - 콜리전이 필요하면 true로 설정
            );
            UE_LOG(LogTemp, Warning, TEXT("Vertices: %d, Normals: %d, UV0: %d, VertexColors: %d, Tangents: %d"), Vertices.Num(), Normals.Num(), UV0.Num(), VertexColors.Num(), Tangents.Num());

            // --- 3. 머티리얼 적용 ---
            if (SectionMaterialIndices.IsValidIndex(SectionIdx))
            {
                int32 MaterialIndex = SectionMaterialIndices[SectionIdx];
                UMaterialInterface* Material = SkelComp->GetMaterial(MaterialIndex); // 컴포넌트에서 머티리얼 인스턴스 가져오기

                 // 컴포넌트가 재정의하지 않은 경우 메시 에셋 머티리얼로 폴백
                if (!Material && SkelComp->GetSkeletalMeshAsset()->GetMaterials().IsValidIndex(MaterialIndex))
                {
                     Material = SkelComp->GetSkeletalMeshAsset()->GetMaterials()[MaterialIndex].MaterialInterface;
                }

                if (Material)
                {
                    ProceduralMeshComponent->SetMaterial(SectionIdx, Material); // PMC 섹션에 머티리얼 설정
                }
                else
                {
                    UE_LOG(LogTemp, Warning, TEXT("SkelToProcMeshComponent: 섹션 %d의 머티리얼 인덱스 %d에 대한 머티리얼을 찾을 수 없습니다."), SectionIdx, MaterialIndex);
                }
            }
        }
    }

    if (!SkelComp->DoesSocketExist(TargetBoneName) && (SkelComp->GetSkeletalMeshAsset()->GetRefSkeleton().FindBoneIndex(TargetBoneName) == INDEX_NONE))
    { 
        UE_LOG(LogTemp, Error, TEXT("SliceAndAttach: Target Bone or Socket '%s' does not exist on SkelComp!"), *TargetBoneName.ToString());
        return false;
    }

    FVector BoneLocation = SkelComp->GetBoneLocation(TargetBoneName);
    if (BoneLocation == FVector::ZeroVector && TargetBoneName != NAME_None)
    {
        UE_LOG(LogTemp, Warning, TEXT("SliceAndAttach: Got ZeroVector for Bone '%s' location. Ensure this is correct."), *TargetBoneName.ToString());
    }
    HideOriginalMeshVerticesByBone(SkelComp);
    
    USlicingSkeletalMeshLibrary::SliceProceduralMesh(
        ProceduralMeshComponent,     
        BoneLocation,                 // PlanePosition
        FVector::UpVector,             // PlaneNormal: 멤버 변수 또는 적절히 계산된 값 사용 (예: FVector::UpVector)
        true,                         // bCreateOtherHalf
        OtherHalfProceduralMeshComponent,                // OutOtherHalfProcMesh
        EProcMeshSliceCapOption::CreateNewSectionForCap,                    // EProcMeshSliceCapOption (멤버 변수 또는 로컬 값)
        CapMaterialInterface,         // UMaterialInterface* (멤버 변수 또는 로컬 값)
        SlicedPMC_To_OriginalPMC_VerticesMap,   // OutSlicedToBaseVertexIndex
        OtherHalfPMC_To_OrigianlPMC_VerticesMap // OutOtherSlicedToBaseVertexIndex
    );

    if (!OtherHalfProceduralMeshComponent) {
        UE_LOG(LogTemp, Error, TEXT("SliceAndAttach: SliceProceduralMesh completed but OtherHalfMesh is null for bone '%s'."), *TargetBoneName.ToString());
        return false;
    }
    
    MakeProcVerticiesIndexMap(SlicedPMC_To_OriginalPMC_VerticesMap, SlicedPMC_To_Skel_VerticesMap);
    MakeProcVerticiesIndexMap(OtherHalfPMC_To_OrigianlPMC_VerticesMap, OtherHalfPMC_To_Skel_VerticesMap);
    
    SlicedPMC_BoneWeightsMap = USlicingSkeletalMeshLibrary::GetBoneWeightsInfoMapForSlicedProcMeshVertices(TargetLODIndex, SkelComp, SlicedPMC_To_Skel_VerticesMap );
    OtherHalfPMC_BoneWeightsMap = USlicingSkeletalMeshLibrary::GetBoneWeightsInfoMapForSlicedProcMeshVertices(TargetLODIndex, SkelComp, OtherHalfPMC_To_Skel_VerticesMap );
    
    // --- 2. 원본 메쉬의 해당 부분 숨기기 ---
    HideOriginalMeshVerticesByBone(SkelComp); // LODIndexToCopy 사용 (기존 코드와 일치)
    
    // --- 3. PMC 조각들 설정 및 부착 ---
    ProceduralMeshComponent->SetSimulatePhysics(false);
    OtherHalfProceduralMeshComponent->SetSimulatePhysics(false);
    ProceduralMeshComponent->SetCollisionEnabled(ECollisionEnabled::PhysicsOnly);
    OtherHalfProceduralMeshComponent->SetCollisionEnabled(ECollisionEnabled::PhysicsOnly);

    // --- 부모 본 이름 가져오기 ---
    TargetBoneIndexInRefSkeleton = SkelComp->GetSkeletalMeshAsset()->GetRefSkeleton().FindBoneIndex(TargetBoneName);
    ParentBoneIndexInRefSkeleton = INDEX_NONE;
    if (TargetBoneIndexInRefSkeleton != INDEX_NONE)
    {
        ParentBoneIndexInRefSkeleton = SkelComp->GetSkeletalMeshAsset()->GetRefSkeleton().GetParentIndex(TargetBoneIndexInRefSkeleton);
        if (ParentBoneIndexInRefSkeleton != INDEX_NONE)
        {
            ParentBoneName = SkelComp->GetSkeletalMeshAsset()->GetRefSkeleton().GetBoneName(ParentBoneIndexInRefSkeleton);
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("SliceAndAttach: TargetBone '%s' is a root bone or has no parent."), *TargetBoneName.ToString());
            // 부모 본이 없는 경우, 예를 들어 월드에 직접 붙이거나 다른 대체 로직을 수행할 수 있습니다.
            // 여기서는 TargetBoneName을 그대로 사용하거나, 루트 본에 붙이는 등의 처리를 할 수 있습니다.
            // 일단은 ParentBoneName이 NAME_None으로 유지되도록 둡니다.
        }
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("SliceAndAttach: Could not find TargetBone '%s' in reference skeleton to determine parent."), *TargetBoneName.ToString());
        return false; // 부모 본을 찾을 수 없으면 실패 처리
    }

    // 부착할 본 이름 결정
    AttachBoneForOtherHalf = ParentBoneName;
    AttachBoneForOriginalPMC = TargetBoneName;  

    // 부모 본이 없는 경우 (TargetBone이 루트 본인 경우 등) 처리
    if (AttachBoneForOriginalPMC == NAME_None)
    {
        UE_LOG(LogTemp, Warning, TEXT("SliceAndAttach: ParentBoneName is None. Attaching Original PMC to TargetBone '%s' instead."), *TargetBoneName.ToString());
        AttachBoneForOriginalPMC = TargetBoneName; // 대체로 TargetBone에 같이 붙이거나, 다른 로직 (예: 루트 본)
        // 혹은 OtherHalfMesh만 TargetBone에 붙이고, ProceduralMeshComponent는 다른 처리를 할 수도 있습니다.
        // 여기서는 간단히 TargetBone에 둘 다 붙이도록 처리합니다. 상황에 맞게 조정 필요.
        // 또는 한 조각은 월드에 고정될 수도 있습니다.
    }


    // 부착할 본/소켓 유효성 검사
    if (AttachBoneForOriginalPMC == NAME_None || (!SkelComp->DoesSocketExist(AttachBoneForOriginalPMC) && SkelComp->GetSkeletalMeshAsset()->GetRefSkeleton().FindBoneIndex(AttachBoneForOriginalPMC) == INDEX_NONE)) {
        UE_LOG(LogTemp, Error, TEXT("SliceAndAttach: AttachBoneForOriginalPMC '%s' is None or does not exist!"), *AttachBoneForOriginalPMC.ToString());
        return false;
    }
    if (AttachBoneForOtherHalf == NAME_None || (!SkelComp->DoesSocketExist(AttachBoneForOtherHalf) && SkelComp->GetSkeletalMeshAsset()->GetRefSkeleton().FindBoneIndex(AttachBoneForOtherHalf) == INDEX_NONE)) {
        UE_LOG(LogTemp, Error, TEXT("SliceAndAttach: AttachBoneForOtherHalf '%s' is None or does not exist!"), *AttachBoneForOtherHalf.ToString());
        return false;
    }

    UE_LOG(LogTemp, Log, TEXT("Attaching ProceduralMeshComponent to '%s' and OtherHalfMesh to '%s'."), *AttachBoneForOriginalPMC.ToString(), *AttachBoneForOtherHalf.ToString());

    // 슬라이스 후에는 보통 월드 트랜스폼을 유지하며 붙이는 것이 초기 위치를 정확히 잡는 데 도움이 될 수 있습니다.
    // 이후 본을 따라가도록 하기 위함입니다.
    //ProceduralMeshComponent->AttachToComponent(SkelComp, FAttachmentTransformRules::KeepWorldTransform, AttachBoneForOriginalPMC);
   // OtherHalfProceduralMeshComponent->AttachToComponent(GetOwner()->GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform, AttachBoneForOtherHalf);
    USceneComponent* OwnerRoot = GetOwner()->GetRootComponent();
    if(OwnerRoot)
    {
       // OtherHalfProceduralMeshComponent->AttachToComponent(OwnerRoot, FAttachmentTransformRules::KeepWorldTransform);
    }
    if (!bNotAttach)
    {
        if (bAttachToSkelComp)
        {
            if (bKeepWorldTransform)
            {
                ProceduralMeshComponent->AttachToComponent(SkelComp, FAttachmentTransformRules::KeepWorldTransform, AttachBoneForOriginalPMC);
                OtherHalfProceduralMeshComponent->AttachToComponent(SkelComp, FAttachmentTransformRules::KeepWorldTransform, AttachBoneForOriginalPMC);
            }
            else
            {

                ProceduralMeshComponent->AttachToComponent(SkelComp, FAttachmentTransformRules::KeepRelativeTransform, AttachBoneForOriginalPMC);
                OtherHalfProceduralMeshComponent->AttachToComponent(SkelComp, FAttachmentTransformRules::KeepRelativeTransform, AttachBoneForOriginalPMC);
            }

        }
        else
        {
            if (bKeepWorldTransform)
            {
                ProceduralMeshComponent->AttachToComponent(OwnerRoot, FAttachmentTransformRules::KeepWorldTransform, AttachBoneForOriginalPMC);
                OtherHalfProceduralMeshComponent->AttachToComponent(OwnerRoot, FAttachmentTransformRules::KeepWorldTransform, AttachBoneForOriginalPMC);
            }
            else
            {
                ProceduralMeshComponent->AttachToComponent(OwnerRoot, FAttachmentTransformRules::KeepRelativeTransform, AttachBoneForOriginalPMC);
                OtherHalfProceduralMeshComponent->AttachToComponent(OwnerRoot, FAttachmentTransformRules::KeepRelativeTransform, AttachBoneForOriginalPMC);
            }
        } 

    }
    OtherHalfProceduralMeshComponent->AttachToComponent(SkelComp, FAttachmentTransformRules::KeepWorldTransform);
    
    SaveCuttingSurfaceVertices(ProceduralMeshComponent, false);
    SaveCuttingSurfaceVertices(OtherHalfProceduralMeshComponent, true);
    
    // --- 4. 부모 스켈레탈 메쉬 컴포넌트 레그돌화 ---
    if (bRagDoll)
    {
        SkelComp->SetCollisionProfileName(TEXT("Ragdoll"));
        SkelComp->SetSimulatePhysics(true);
    }

    if (bBreakConstraint)
    {
        SkelComp->BreakConstraint(FVector(5000.f, 5000.f, 5000.f), FVector::ZeroVector, TargetBoneName);
    }
    return true;
}


bool USlicingSkelMeshComponent::GetFilteredSkeletalMeshDataByBoneName(const USkeletalMeshComponent* SkelComp, float MinWeight, TArray<FVector>& OutVertices,
    TArray<FVector>& OutNormals, TArray<FProcMeshTangent>& OutTangents, TArray<FVector2D>& OutUV0, TArray<FLinearColor>& OutVertexColors,
    TArray<int32>& OutSectionMaterialIndices, TArray<TArray<int32>>& OutSectionIndices)
{
    // 0. 필수 컴포넌트 및 데이터 유효성 검사
    if (!SkelComp || !SkelComp->GetSkeletalMeshAsset() || !SkelComp->GetSkeletalMeshAsset()->GetResourceForRendering()) return false;
    
    FSkeletalMeshRenderData* RenderData = SkelComp->GetSkeletalMeshAsset()->GetResourceForRendering();
    if (!RenderData || !RenderData->LODRenderData.IsValidIndex(TargetLODIndex)) // LOD 인덱스 유효성 검사
    {
        UE_LOG(LogTemp, Warning, TEXT("LODRenderData[%d]이 유효하지 않습니다."), TargetLODIndex);
        return false; 
    }
    FSkeletalMeshLODRenderData& LODRenderData = RenderData->LODRenderData[TargetLODIndex];
    
    // 1. Bone Name으로 버텍스 필터링
    int32 TargetBoneIndex = INDEX_NONE;
    bool bShouldFilter = (TargetBoneName != NAME_None);
    const FSkinWeightVertexBuffer* SkinWeightBufferPtr = nullptr; // 필터링 시에만 사용

    if (bShouldFilter)
    {
        TargetBoneIndex = SkelComp->GetBoneIndex(TargetBoneName);
        if (TargetBoneIndex == INDEX_NONE)
        {
            UE_LOG(LogTemp, Warning, TEXT("GetFilteredSkeletalMeshData: TargetBone '%s' 을 찾을 수 없습니다. 필터링 실패."), *TargetBoneName.ToString());
            bShouldFilter = false;
        }
        else
        {
            if (LODRenderData.SkinWeightVertexBuffer.GetNumVertices() == 0)
            {
                UE_LOG(LogTemp, Error, TEXT("GetFilteredSkeletalMeshData: SkinWeightVertexBuffer가 LOD %d에 대해 유효하지 않습니다. 필터링 실패."), TargetLODIndex);
                return false; // 스킨 웨이트 없으면 필터링 불가
            }
            SkinWeightBufferPtr = LODRenderData.GetSkinWeightVertexBuffer(); // 유효하면 포인터 할당
            UE_LOG(LogTemp, Log, TEXT("GetFilteredSkeletalMeshData: 필터링 by bone '%s' (Index: %d), MinWeight: %f"), *TargetBoneName.ToString(), TargetBoneIndex, MinWeight);
        }
    }
    else
    {
        UE_LOG(LogTemp, Log, TEXT("GetFilteredSkeletalMeshData: TargetBone으로 필터링 하지 않음. LOD %d에서 모든 정점 사용."), TargetLODIndex);
    }
    
    // --- 스키닝된 버텍스 위치 가져오기 ---
    // *포즈가 적용된* 메쉬를 얻기 위한 핵심 부분
    
    if(LODRenderData.GetNumVertices() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("SkelToProcMeshComponent: LOD %d에 대해 GetNumVertices가 0개의 버텍스를 반환했습니다."), TargetLODIndex);
        return false; // 버텍스가 없으면 실패 처리
    }
    
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
    } else {
        // 그렇지 않으면 버텍스 컬러 배열 비우기
        OutVertexColors.Empty();
    }

    // Filter 전 후 Vertex Index를 비교하기 위한 TMap
    TMap<uint32, uint32> FilteredVertexIndexMap;
    uint32 FilteredVertexIndex = 0;
    
    FTransform MeshTransform = SkelComp->GetComponentTransform();
    FVector TargetBoneLocation = SkelComp->GetBoneLocation(TargetBoneName); 
    for (const FSkelMeshRenderSection& Section : LODRenderData.RenderSections)
    {
        const uint32 SectionNumvertices = Section.NumVertices;
        const uint32 SectionBaseVertexIndex = Section.BaseVertexIndex;
        {
            for (uint32 i = 0; i < SectionNumvertices; i++)
            {
                const uint32 VertexIndex = i + SectionBaseVertexIndex;
                if (USlicingSkeletalMeshLibrary::GetBoneWeightForVertex(VertexIndex, TargetBoneIndex, &Section, &LODRenderData, SkinWeightBufferPtr) > Threshold)
                {
                    // Vertex 처리
                    const FVector SkinnedVectorPosition = FVector(StaticVertexBuffers.PositionVertexBuffer.VertexPosition(VertexIndex));
     
                    OutVertices.Add(SkinnedVectorPosition);

                    PMC_To_Skel_VerticesMap.Add(FilteredVertexIndex, VertexIndex);
                    FilteredVertexIndexMap.Add(VertexIndex, FilteredVertexIndex);
                    FilteredVertexIndex += 1;
                    
                    // 노멀 (압축된 FPackedNormal로 저장됨)
                    // 버퍼 유형에 따라 FPackedNormal/FVector3f 변환 필요
                    const FVector Normal = FVector(StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(VertexIndex)); // Z는 노멀 포함
                    OutNormals.Add(Normal);
                    
                    const FVector TangentX = FVector(StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentX(VertexIndex)); // X는 탄젠트 포함
                    auto ProcMeshTangent = FProcMeshTangent(TangentX, false);
                    ProcMeshTangent.TangentX = TangentX;
                    OutTangents.Add(ProcMeshTangent);

                    // 외적과 부호를 사용하여 바이노멀(TangentY) 계산
                    // FVector TangentY = FVector::CrossProduct(Normal, TangentX) * TangentSign;
                    // CreateMeshSection용 FProcMeshTangent에는 TangentY를 직접 저장하지 않지만, Normal과 TangentX로부터 파생됨
                    
                    // UV (단순화를 위해 UV 채널 1개만 가정)
                    // 버퍼 유형에 따라 FVector2f 변환 필요
                    const FVector2D VertexUV = FVector2D(StaticVertexBuffers.StaticMeshVertexBuffer.GetVertexUV(VertexIndex, 0));
                    OutUV0.Add(VertexUV);
                    OutVertexColors.Add(FColor(0, 0, 0, 255));

                    /*
                    *                // 버텍스 컬러 (FColor로 저장됨)
                    if (bCopyVertexColors && StaticVertexBuffers.ColorVertexBuffer.IsInitialized() && StaticVertexBuffers.ColorVertexBuffer.GetNumVertices() > VertexIndex)
                    {
                    // 버텍스 컬러 복사 옵션이 켜져 있고, 버퍼가 초기화되었으며, 유효한 인덱스인 경우
                    OutVertexColors.Add(StaticVertexBuffers.ColorVertexBuffer.VertexColor(VertexIndex).ReinterpretAsLinear()); // FColor를 FLinearColor로 변환하여 저장
                     */
                    
                    
                }
            }
        }
    }
    
    // --- 인덱스 재구성 단계 (섹션 구조 유지) ---
    // 원본 인덱스 버퍼 데이터 가져오기
    TArray<uint32> IndexBuffer;
    LODRenderData.MultiSizeIndexContainer.GetIndexBuffer(IndexBuffer);

    const int32 NumSections = LODRenderData.RenderSections.Num();

    // 출력 배열 크기 설정 및 초기화 (OutSectionIndices는 TArray<TArray<int32>> 타입)
    OutSectionIndices.Empty(NumSections); // 이전 데이터 비우기
    OutSectionIndices.SetNum(NumSections); // 섹션 수만큼 내부 TArray 생성

    // 원본 머티리얼 인덱스 저장 (CopySkeletalLODToProcedural에서 사용하기 위해)
    OutSectionMaterialIndices.Empty(NumSections);
    OutSectionMaterialIndices.SetNum(NumSections);

    // 섹션별 루프
    for (int32 SectionIdx = 0; SectionIdx < NumSections; ++SectionIdx)
    {
        const FSkelMeshRenderSection& Section = LODRenderData.RenderSections[SectionIdx];
        const int32 NumTrianglesInSection = Section.NumTriangles;

        // 현재 섹션의 머티리얼 인덱스 저장
        OutSectionMaterialIndices[SectionIdx] = Section.MaterialIndex;

        // 현재 섹션의 필터링된 인덱스를 저장할 내부 배열 준비 (예상 크기 지정 가능)
        OutSectionIndices[SectionIdx].Empty(NumTrianglesInSection * 3);

        // 현재 섹션의 트라이앵글별 루프
        for (int32 TriIdx = 0; TriIdx < NumTrianglesInSection; ++TriIdx)
        {
            uint32 IndexOffset = Section.BaseIndex + TriIdx * 3;

            // 인덱스 버퍼 범위 안전 확인
            if (!IndexBuffer.IsValidIndex(IndexOffset + 2))
            {
                UE_LOG(LogTemp, Error, TEXT("Section %d: Index Buffer access out of bounds! Offset: %u"), SectionIdx, IndexOffset);
                continue; // 이 트라이앵글 건너뛰기
            }

            // 1. 원본 버텍스 인덱스 읽기
            uint32 OrigIdx0 = IndexBuffer[IndexOffset + 0];
            uint32 OrigIdx1 = IndexBuffer[IndexOffset + 1];
            uint32 OrigIdx2 = IndexBuffer[IndexOffset + 2];

            // 2. 세 원본 인덱스가 모두 FilteredVertexIndexMap에 있는지 확인 (Find 사용)
            const uint32* PtrNewIdx0 = FilteredVertexIndexMap.Find(OrigIdx0);
            const uint32* PtrNewIdx1 = FilteredVertexIndexMap.Find(OrigIdx1);
            const uint32* PtrNewIdx2 = FilteredVertexIndexMap.Find(OrigIdx2);

            // 3. 모두 존재하면, 새로운 인덱스를 현재 섹션의 출력 배열에 추가
            if (PtrNewIdx0 != nullptr && PtrNewIdx1 != nullptr && PtrNewIdx2 != nullptr)
            {
                OutSectionIndices[SectionIdx].Add(*PtrNewIdx0); // 새 인덱스 추가
                OutSectionIndices[SectionIdx].Add(*PtrNewIdx1);
                OutSectionIndices[SectionIdx].Add(*PtrNewIdx2);
            }
            // else: 트라이앵글의 버텍스 중 하나라도 필터링되었다면, 이 트라이앵글은 결과에 포함되지 않음
        }
    }

    // 중요: 모든 처리가 끝난 후, 일부 섹션의 인덱스 배열이 비어있을 수 있습니다.
    // SliceMeshInternal 함수에서는 비어있지 않은 섹션에 대해서만 CreateMeshSection을 호출해야 합니다.

    // 필터링 후 유효한 트라이앵글이 하나도 없는 경우 체크 (선택 사항)
    bool bFoundAnyTriangles = false;
    for(const auto& Indices : OutSectionIndices)
    {
        if (Indices.Num() > 0)
        {
            bFoundAnyTriangles = true;
            break;
        }
    }
    if (!bFoundAnyTriangles && OutVertices.Num() > 0) // 버텍스는 있는데 삼각형이 없으면
    {
         UE_LOG(LogTemp, Warning, TEXT("GetFilteredSkeletalMeshDataByBoneName: No triangles remained after filtering."));
         // return false; // 여기서 실패 처리할 수도 있음
    }
    return true; // 성
}


void USlicingSkelMeshComponent::HideOriginalMeshVerticesByBone(USkeletalMeshComponent* SourceSkeletalMeshComp, bool bClearOverride)
{
    if (!SourceSkeletalMeshComp || !SourceSkeletalMeshComp->GetSkeletalMeshAsset())
    {
        UE_LOG(LogTemp, Warning, TEXT("HideOriginalMeshVerticesByBone: Invalid Skeletal Mesh Component or Asset."));
        return;
    }

    USkeletalMesh* SkelMesh = SourceSkeletalMeshComp->GetSkeletalMeshAsset();
    const FReferenceSkeleton& RefSkeleton = SkelMesh->GetRefSkeleton();
    int32 TargetBoneIndex = RefSkeleton.FindBoneIndex(TargetBoneName);

    if (TargetBoneIndex == INDEX_NONE)
    {
        UE_LOG(LogTemp, Warning, TEXT("HideOriginalMeshVerticesByBone: Bone '%s' not found."), *TargetBoneName.ToString());
        return;
    }

    // FSkeletalMeshRenderData 접근은 게임 스레드에서 직접 하면 위험할 수 있으나, 읽기 목적 및 컴포넌트 상태 기반이므로
    // 일부 상황에서는 가능할 수 있습니다. 복잡한 경우 렌더 스레드 접근 고려.
    FSkeletalMeshRenderData* SkelRenderData = SourceSkeletalMeshComp->GetSkeletalMeshRenderData();
    if (!SkelRenderData || SkelRenderData->LODRenderData[TargetLODIndex].GetNumVertices() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("HideOriginalMeshVerticesByBone: Invalid LOD Index %d."), TargetLODIndex);
        return;
    }

    FSkeletalMeshLODRenderData& LODRenderData = SkelRenderData->LODRenderData[TargetLODIndex];
    FSkinWeightVertexBuffer* SkinWeightBuffer = SourceSkeletalMeshComp->GetSkinWeightBuffer(TargetLODIndex); // 인스턴스별 버퍼 가져오기

    if (!SkinWeightBuffer)
    {
        UE_LOG(LogTemp, Warning, TEXT("HideOriginalMeshVerticesByBone: Could not get SkinWeightVertexBuffer for LOD %d."), TargetLODIndex);
        return;
    }

    uint32 NumVertices = LODRenderData.GetNumVertices();
    if (NumVertices == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("HideOriginalMeshVerticesByBone: LOD %d has no vertices."), TargetLODIndex);
        return;
    }

    TArray<int32> VerticesToHideIndices;
    VerticesToHideIndices.Reserve(NumVertices / 4); // 예상 크기 예약

    // 1. 숨길 버텍스 식별 (GetBoneWeightForVertex 구현 필요)
    for (uint32 VertexIndex = 0; VertexIndex < NumVertices; ++VertexIndex)
    {
        // GetBoneWeightForVertex 함수는 FSkinWeightVertexBuffer 구조에 맞춰 구현해야 합니다.
        // 아래는 가상의 함수 호출입니다. 실제 구현은 SkinWeightBuffer->GetWeightForVertex 등을 사용해야 합니다.
        float Weight = USlicingSkeletalMeshLibrary::GetBoneWeightForVertex(VertexIndex, TargetBoneIndex, &LODRenderData.RenderSections[TargetLODIndex], &LODRenderData, SkinWeightBuffer);

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
        SourceSkeletalMeshComp->SetVertexColorOverride_LinearColor(TargetLODIndex, OverrideColors);
         UE_LOG(LogTemp, Log, TEXT("Applied vertex color override to hide %d vertices for bone '%s' on LOD %d."), VerticesToHideIndices.Num(), *TargetBoneName.ToString(), TargetLODIndex);
        return;
    }
    else if (bClearOverride)
    {
        // 숨길 버텍스가 없고, 이전 오버라이드를 지우도록 설정된 경우
        SourceSkeletalMeshComp->ClearVertexColorOverride(TargetLODIndex);
        UE_LOG(LogTemp, Log, TEXT("No vertices to hide for bone '%s' on LOD %d. Cleared override (if any)."), *TargetBoneName.ToString(), TargetLODIndex);
        return; // 작업은 성공적으로 완료됨 (숨길 것이 없었음)
    }

    return; // 숨길 것이 없었음
}

void USlicingSkelMeshComponent::PerformVertexSkinning(UProceduralMeshComponent* ProcMesh, FName AttachBoneName, bool bOtherHalfMesh)
{
    USkeletalMeshComponent* SkelComp = GetOwnerSkeletalMeshComponent();
    if (!SkelComp || !SkelComp->GetSkeletalMeshAsset() || TargetBoneName == NAME_None || !ProcMesh)
    {
        UE_LOG(LogTemp, Warning, TEXT("PerformVertexSkinning: Invalid input or member variables."));
        return;
    }

    TargetBoneIndexInRefSkeleton = SkelComp->GetSkeletalMeshAsset()->GetRefSkeleton().FindBoneIndex(AttachBoneName);
    if (TargetBoneIndexInRefSkeleton == INDEX_NONE)
    {
        UE_LOG(LogTemp, Error, TEXT("TargetBone '%s' not found in skeleton!"), *TargetBoneName.ToString());
        return;
    }
    
    FSkeletalMeshRenderData* RenderData = SkelComp->GetSkeletalMeshAsset()->GetResourceForRendering();
    if (!RenderData || !RenderData->LODRenderData.IsValidIndex(TargetLODIndex))
    {
        UE_LOG(LogTemp, Error, TEXT("Invalid RenderData or TargetLODIndex for skeleton: %s"), *SkelComp->GetSkeletalMeshAsset()->GetName());
        return;
    }
    FSkeletalMeshLODRenderData& LODRenderData = RenderData->LODRenderData[TargetLODIndex];
    
    const TArray<FTransform>& AllCurrentBoneTransforms_CS = SkelComp->GetComponentSpaceTransforms();
    if (!AllCurrentBoneTransforms_CS.IsValidIndex(TargetBoneIndexInRefSkeleton))
    {
        UE_LOG(LogTemp, Error, TEXT("Invalid TargetBoneIndexInRefSkeleton for CurrentBoneTransforms. Index: %d, ArraySize: %d"), TargetBoneIndexInRefSkeleton, AllCurrentBoneTransforms_CS.Num());
        return;
    }
    
    const FTransform CurrentAnimatedTargetBoneTransform_CS = AllCurrentBoneTransforms_CS[TargetBoneIndexInRefSkeleton];

    TArray<FTransform> RefBoneTransforms = SkelComp->GetSkinnedAsset()->GetRefSkeleton().GetRefBonePose();
    
    FTransform RefPoseTransform = FAnimationRuntime::GetComponentSpaceTransformRefPose(SkelComp->GetSkinnedAsset()->GetRefSkeleton(), TargetBoneIndexInRefSkeleton);
    const FTransform InverseOfTargetBoneBindPoseTransform_CS = RefPoseTransform.Inverse();
    
    const FTransform CombinedBoneTransform_CS = CurrentAnimatedTargetBoneTransform_CS * InverseOfTargetBoneBindPoseTransform_CS;
    
    for (int32 SectionIndex = 0; SectionIndex < ProcMesh->GetNumSections(); SectionIndex++)
    {
        FProcMeshSection* Section = ProcMesh->GetProcMeshSection(SectionIndex);
        const TArray<FProcMeshVertex>& CurrentProcMeshVertices = Section->ProcVertexBuffer;
        
        TArray<FVector> NewSkinnedPositions;
        TArray<FVector> NewSkinnedNormals;
        TArray<FProcMeshTangent> NewSkinnedTangents;

        // UV와 Vertex Color는 일반적으로 스키닝되지 않으므로 원본 값을 유지합니다.
        TArray<FVector2D> OriginalUV0s;
        TArray<FLinearColor> OriginalVertexColors_Linear; // UpdateMeshSection_LinearColor 사용을 권장
        
        NewSkinnedPositions.Reserve(CurrentProcMeshVertices.Num());
        NewSkinnedNormals.Reserve(CurrentProcMeshVertices.Num());
        NewSkinnedTangents.Reserve(CurrentProcMeshVertices.Num());
        OriginalUV0s.Reserve(CurrentProcMeshVertices.Num());
        OriginalVertexColors_Linear.Reserve(CurrentProcMeshVertices.Num());

        for (int32 ProcVertexIndex = 0; ProcVertexIndex < CurrentProcMeshVertices.Num(); ProcVertexIndex++)
        {
            const FProcMeshVertex& CurrentVertex = CurrentProcMeshVertices[ProcVertexIndex];
            
            // UV와 Vertex Color는 원본 값을 유지
           // NewSkinnedPositions.Add(CurrentVertex.Position);
            OriginalUV0s.Add(CurrentVertex.UV0);
            OriginalVertexColors_Linear.Add(CurrentVertex.Color); // FLinearColor로 유지
            
            // PMC_To_Skel_VerticesMap: Key(PMC Vertex Index) -> Value(Original Skeletal Mesh Vertex Index)
            const uint32* OriginalSkelIndexPtr = nullptr;
            if (bOtherHalfMesh)
            {
                const uint32* OriginalPMCIndexPtr = OtherHalfPMC_To_OrigianlPMC_VerticesMap.Find(ProcVertexIndex);
                if (OriginalPMCIndexPtr)
                {
                    OriginalSkelIndexPtr = PMC_To_Skel_VerticesMap.Find(*OriginalPMCIndexPtr);
                }
            }
            else
            {
                const uint32* OriginalPMCIndexPtr = SlicedPMC_To_OriginalPMC_VerticesMap.Find(ProcVertexIndex);
                if (OriginalPMCIndexPtr)
                {
                    OriginalSkelIndexPtr = PMC_To_Skel_VerticesMap.Find(*OriginalPMCIndexPtr);
                }
            }
                
   // 현재 처리 중인 ProcMesh의 로컬 VertexIndex
            if (OriginalSkelIndexPtr)
            {
                uint32 OriginalSkelGlobalIndex = *OriginalSkelIndexPtr;
                
                if (OriginalSkelGlobalIndex >= LODRenderData.GetNumVertices())
                {
                    UE_LOG(LogTemp, Warning, TEXT("PerformVertexSkinning: OriginalSkelGlobalIndex (%u) out of bounds for LODRenderData vertices (%u). ProcVertexIndex: %d. Using current PMC vertex data."),
                            OriginalSkelGlobalIndex, LODRenderData.GetNumVertices(), ProcVertexIndex);
                    NewSkinnedPositions.Add(FVector(CurrentVertex.Position));
                    NewSkinnedNormals.Add(FVector(CurrentVertex.Normal).GetSafeNormal());
                    NewSkinnedTangents.Add(FProcMeshTangent(FVector(CurrentVertex.Tangent.TangentX).GetSafeNormal(), CurrentVertex.Tangent.bFlipTangentY));
                    continue;
                }

                const FVector3f BindPoseVertexPosition_CS_F3f = LODRenderData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(OriginalSkelGlobalIndex);
                const FVector BindPoseVertexPosition_CS(BindPoseVertexPosition_CS_F3f); // FVector로 변환
                
                const FStaticMeshVertexBuffer& StaticVB = LODRenderData.StaticVertexBuffers.StaticMeshVertexBuffer;
                const FVector BindPoseNormal_CS(StaticVB.VertexTangentZ(OriginalSkelGlobalIndex)); 
                const FVector BindPoseTangentX_CS(StaticVB.VertexTangentX(OriginalSkelGlobalIndex));
                
                FVector VertexInTargetBoneLocal_AtBindPose = InverseOfTargetBoneBindPoseTransform_CS.TransformPosition(FVector(BindPoseVertexPosition_CS));
                FVector SkinnedVertex_CS = CurrentAnimatedTargetBoneTransform_CS.TransformPosition(VertexInTargetBoneLocal_AtBindPose);
                NewSkinnedPositions.Add(SkinnedVertex_CS);
                
                // 노멀 스키닝 (TransformVector 사용 및 정규화)
                NewSkinnedNormals.Add(CombinedBoneTransform_CS.TransformVector(BindPoseNormal_CS).GetSafeNormal());

                // 탄젠트 스키닝 (TransformVector 사용 및 정규화)
                FVector SkinnedTangentX = CombinedBoneTransform_CS.TransformVector(BindPoseTangentX_CS);
                NewSkinnedTangents.Add(FProcMeshTangent(SkinnedTangentX.GetSafeNormal(), CurrentVertex.Tangent.bFlipTangentY));
            }
            else
            {
                NewSkinnedPositions.Add(FVector(CurrentVertex.Position));
                NewSkinnedNormals.Add(FVector(CurrentVertex.Normal).GetSafeNormal()); // 현재 노멀 사용
                NewSkinnedTangents.Add(FProcMeshTangent(FVector(CurrentVertex.Tangent.TangentX).GetSafeNormal(), CurrentVertex.Tangent.bFlipTangentY)); // 현재 탄젠트 사용
            }

        }
        
        // FLinearColor를 사용하므로 UpdateMeshSection_LinearColor 사용 권장
        ProcMesh->UpdateMeshSection_LinearColor(
            SectionIndex,
            NewSkinnedPositions,
            NewSkinnedNormals,
            OriginalUV0s,
            OriginalVertexColors_Linear, 
            NewSkinnedTangents
        );
    }
}

void USlicingSkelMeshComponent::SaveSlicingSurfaceVertexInfos()
{

    
}


void USlicingSkelMeshComponent::PerformEntireVertexSkinning(UProceduralMeshComponent* ProcMesh, bool bOtherHalfMesh /* FName AttachBoneName - 이 파라미터는 이제 스키닝 계산에 직접 사용되지 않을 수 있습니다 */)
{
    USkeletalMeshComponent* SkelComp = GetOwnerSkeletalMeshComponent();
    if (!SkelComp || !SkelComp->GetSkeletalMeshAsset() || !ProcMesh) 
    {
        UE_LOG(LogTemp, Warning, TEXT("PerformVertexSkinning: Invalid input or member variables."));
        return;
    }

    FSkeletalMeshRenderData* RenderData = SkelComp->GetSkeletalMeshAsset()->GetResourceForRendering();
    if (!RenderData || !RenderData->LODRenderData.IsValidIndex(TargetLODIndex))
    {
        UE_LOG(LogTemp, Error, TEXT("Invalid RenderData or TargetLODIndex for skeleton: %s"), *SkelComp->GetSkeletalMeshAsset()->GetName());
        return;
    }
    FSkeletalMeshLODRenderData& LODRenderData = RenderData->LODRenderData[TargetLODIndex];
    
    const TArray<FTransform>& AllCurrentBoneTransforms_CS = SkelComp->GetComponentSpaceTransforms();
    const USkeletalMesh* SkeletalMeshAsset = SkelComp->GetSkeletalMeshAsset();
    const FReferenceSkeleton& RefSkeleton = SkeletalMeshAsset->GetRefSkeleton();

    FTransform TargetBoneTransform_Bind_CS  = FAnimationRuntime::GetComponentSpaceTransformRefPose(SkeletalMeshAsset->GetRefSkeleton(), TargetBoneIndexInRefSkeleton);
    FTransform ParentBoneTransform_Bind_CS  = FAnimationRuntime::GetComponentSpaceTransformRefPose(SkeletalMeshAsset->GetRefSkeleton(), ParentBoneIndexInRefSkeleton);

    FTransform RelativeOffset_Bind = TargetBoneTransform_Bind_CS.GetRelativeTransform(ParentBoneTransform_Bind_CS);
    
    for (int32 SectionIndex = 0; SectionIndex < ProcMesh->GetNumSections(); SectionIndex++)
    {
        FProcMeshSection* Section = ProcMesh->GetProcMeshSection(SectionIndex);
        if (!Section || Section->ProcVertexBuffer.Num() == 0) // 섹션 유효성 검사 강화
        {
            continue;
        }
        const TArray<FProcMeshVertex>& CurrentProcMeshVertices = Section->ProcVertexBuffer;
        
        TArray<FVector> NewSkinnedPositions;
        TArray<FVector> NewSkinnedNormals;
        TArray<FProcMeshTangent> NewSkinnedTangents;
        TArray<FVector2D> OriginalUV0s;
        TArray<FLinearColor> NewVertexColors_Linear;
        
        NewSkinnedPositions.Reserve(CurrentProcMeshVertices.Num());
        NewSkinnedNormals.Reserve(CurrentProcMeshVertices.Num());
        NewSkinnedTangents.Reserve(CurrentProcMeshVertices.Num());
        OriginalUV0s.Reserve(CurrentProcMeshVertices.Num());
        NewVertexColors_Linear.Reserve(CurrentProcMeshVertices.Num());

        for (int32 ProcVertexIndex = 0; ProcVertexIndex < CurrentProcMeshVertices.Num(); ProcVertexIndex++)
        {
            const FProcMeshVertex& CurrentVertex = CurrentProcMeshVertices[ProcVertexIndex];
            
            OriginalUV0s.Add(CurrentVertex.UV0);
            // 1. 이 정점에 대한 FBoneWeightsInfo 가져오기
            FBoneWeightsInfo* VertexBoneWeightsInfo = nullptr;
            uint32* OriginalSkelGlobalIndexUint32 = nullptr;
            float CorrectionWeight = 0.0f;
            if (bOtherHalfMesh)
            {
                VertexBoneWeightsInfo = OtherHalfPMC_BoneWeightsMap.Find(ProcVertexIndex);
                OriginalSkelGlobalIndexUint32 = OtherHalfPMC_To_Skel_VerticesMap.Find(ProcVertexIndex);
            }
            else
            {
                VertexBoneWeightsInfo = SlicedPMC_BoneWeightsMap.Find(ProcVertexIndex);
                OriginalSkelGlobalIndexUint32 = SlicedPMC_To_Skel_VerticesMap.Find(ProcVertexIndex);
            }
            
            if (VertexBoneWeightsInfo && VertexBoneWeightsInfo->InfluencingBoneIndices.Num() && OriginalSkelGlobalIndexUint32 != nullptr) // 유효하고 Slice 과정에서 생성되지 않은 정점
            {
                uint32 OriginalSkelGlobalIndex = *OriginalSkelGlobalIndexUint32;
                if (OriginalSkelGlobalIndex >= LODRenderData.GetNumVertices())
                {
                    NewSkinnedPositions.Add(FVector(CurrentVertex.Position));
                    NewSkinnedNormals.Add(FVector(CurrentVertex.Normal).GetSafeNormal());
                    NewSkinnedTangents.Add(FProcMeshTangent(FVector(CurrentVertex.Tangent.TangentX).GetSafeNormal(), CurrentVertex.Tangent.bFlipTangentY));
                    continue;
                }

                FVector FinalSkinnedPosition_CS = FVector::ZeroVector;
                FVector FinalSkinnedNormal_CS = FVector::ZeroVector;
                FVector FinalSkinnedTangentX_CS = FVector::ZeroVector;

                // 바인드 포즈 데이터는 한 번만 가져옴
                const FVector3f BindPoseVertexPosition_CS_F3f = LODRenderData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(OriginalSkelGlobalIndex);
                FVector BindPoseVertexPosition_CS(BindPoseVertexPosition_CS_F3f);
                
                const FStaticMeshVertexBuffer& StaticVB = LODRenderData.StaticVertexBuffers.StaticMeshVertexBuffer;
                const FVector BindPoseNormal_CS(StaticVB.VertexTangentZ(OriginalSkelGlobalIndex)); 
                const FVector BindPoseTangentX_CS(StaticVB.VertexTangentX(OriginalSkelGlobalIndex));
                
                for (int32 i = 0; i < VertexBoneWeightsInfo->InfluencingBoneIndices.Num(); ++i)
                {
                    int32 BoneIndex = VertexBoneWeightsInfo->InfluencingBoneIndices[i];
                    if (( bOtherHalfMesh == false && BoneIndex < TargetBoneIndexInRefSkeleton) ||( bOtherHalfMesh == true && BoneIndex > ParentBoneIndexInRefSkeleton) )
                    {
                        CorrectionWeight += VertexBoneWeightsInfo->BoneWeights[i];
                    }
                }
                
                for (int32 i = 0; i < VertexBoneWeightsInfo->InfluencingBoneIndices.Num(); ++i)
                {
                    int32 BoneIndex = VertexBoneWeightsInfo->InfluencingBoneIndices[i];
                    float Weight = VertexBoneWeightsInfo->BoneWeights[i] * (1 / (1 - CorrectionWeight));
                    if (Weight == 0) 
                    {
                        continue;
                    }
                    
                    if ((bOtherHalfMesh == false && BoneIndex < TargetBoneIndexInRefSkeleton) || (bOtherHalfMesh == true && BoneIndex > ParentBoneIndexInRefSkeleton))
                    {
                        continue;
                    }
                    
                    if (!AllCurrentBoneTransforms_CS.IsValidIndex(BoneIndex))
                    {
                        UE_LOG(LogTemp, Warning, TEXT("PerformVertexSkinning: Invalid BoneIndex %d from BoneWeightsInfo."), BoneIndex);
                        continue;
                    }
             
                    const FTransform& CurrentAnimatedBoneTransform_CS = AllCurrentBoneTransforms_CS[BoneIndex];
                    FTransform BindPoseBoneTransform_CS = FAnimationRuntime::GetComponentSpaceTransformRefPose(SkeletalMeshAsset->GetRefSkeleton(), BoneIndex);
                    const FTransform InverseBindPoseBoneTransform_CS = BindPoseBoneTransform_CS.Inverse();
                    const FTransform CombinedBoneTransform_CS = CurrentAnimatedBoneTransform_CS * InverseBindPoseBoneTransform_CS;
                    
                    FinalSkinnedPosition_CS += CurrentAnimatedBoneTransform_CS.TransformPosition(InverseBindPoseBoneTransform_CS.TransformPosition(BindPoseVertexPosition_CS)) * Weight;
                    FinalSkinnedNormal_CS += CombinedBoneTransform_CS.TransformVector(BindPoseNormal_CS) * Weight;
                    FinalSkinnedTangentX_CS += CombinedBoneTransform_CS.TransformVector(BindPoseTangentX_CS) * Weight;
                }
                NewSkinnedPositions.Add(FinalSkinnedPosition_CS);
                NewSkinnedNormals.Add(FinalSkinnedNormal_CS.GetSafeNormal());  // 합산 후 정규화
                NewSkinnedTangents.Add(FProcMeshTangent(FinalSkinnedTangentX_CS.GetSafeNormal(), CurrentVertex.Tangent.bFlipTangentY));  // 합산 후 정규화
                NewVertexColors_Linear.Add(CurrentVertex.Color);
            }
            else //Slice 과정에서 추가된 절단면 및 주변 부 버텍스 또는 유효하지 않는 버텍스
            {
                FVector FinalSkinnedPosition_CS = FVector::ZeroVector;
                FVector FinalSkinnedNormal_CS = FVector::ZeroVector;
                FVector FinalSkinnedTangentX_CS = FVector::ZeroVector;
                
                if (bOtherHalfMesh == false)
                {
                    const FTransform& CurrentAnimatedBoneTransform_CS = AllCurrentBoneTransforms_CS[TargetBoneIndexInRefSkeleton];
                    FTransform BindPoseBoneTransform_CS = FAnimationRuntime::GetComponentSpaceTransformRefPose(SkeletalMeshAsset->GetRefSkeleton(), TargetBoneIndexInRefSkeleton);
                    const FTransform InverseBindPoseBoneTransform_CS = BindPoseBoneTransform_CS.Inverse();
                    const FTransform CombinedBoneTransform_CS = CurrentAnimatedBoneTransform_CS * InverseBindPoseBoneTransform_CS;

                    FProcMeshVertex ProcMeshVertex = SlicedPMC_CuttingSurfaceVertices[ProcVertexIndex];
                    FinalSkinnedPosition_CS += CurrentAnimatedBoneTransform_CS.TransformPosition(InverseBindPoseBoneTransform_CS.TransformPosition(ProcMeshVertex.Position));
                    FinalSkinnedNormal_CS += CombinedBoneTransform_CS.TransformVector(ProcMeshVertex.Normal);
                    FinalSkinnedTangentX_CS += CombinedBoneTransform_CS.TransformVector(ProcMeshVertex.Tangent.TangentX);
                }
                else
                {
                    const FTransform& CurrentAnimatedBoneTransform_CS = AllCurrentBoneTransforms_CS[ParentBoneIndexInRefSkeleton];
                    FTransform BindPoseBoneTransform_CS = FAnimationRuntime::GetComponentSpaceTransformRefPose(SkeletalMeshAsset->GetRefSkeleton(), ParentBoneIndexInRefSkeleton);
                    const FTransform InverseBindPoseBoneTransform_CS = BindPoseBoneTransform_CS.Inverse();
                    const FTransform CombinedBoneTransform_CS = CurrentAnimatedBoneTransform_CS * InverseBindPoseBoneTransform_CS;

                    FProcMeshVertex ProcMeshVertex = OtherHalfPMC_CuttingSurfaceVertices[ProcVertexIndex];
                    FinalSkinnedPosition_CS += CurrentAnimatedBoneTransform_CS.TransformPosition(InverseBindPoseBoneTransform_CS.TransformPosition(ProcMeshVertex.Position));
                    FinalSkinnedNormal_CS += CombinedBoneTransform_CS.TransformVector(ProcMeshVertex.Normal);
                    FinalSkinnedTangentX_CS += CombinedBoneTransform_CS.TransformVector(ProcMeshVertex.Tangent.TangentX);

                }
                NewSkinnedPositions.Add(FinalSkinnedPosition_CS);
                NewSkinnedNormals.Add(FinalSkinnedNormal_CS.GetSafeNormal());  // 합산 후 정규화
                NewSkinnedTangents.Add(FProcMeshTangent(FinalSkinnedTangentX_CS.GetSafeNormal(), CurrentVertex.Tangent.bFlipTangentY));  // 합산 후 정규화
                NewVertexColors_Linear.Add(CurrentVertex.Color);
            }
        }

        ProcMesh->UpdateMeshSection_LinearColor(
            SectionIndex,
            NewSkinnedPositions,
            NewSkinnedNormals,
            OriginalUV0s,
            NewVertexColors_Linear, 
            NewSkinnedTangents
        );
    }
}

USkeletalMeshComponent* USlicingSkelMeshComponent::GetOwnerSkeletalMeshComponent() const
{
    AActor* Owner = GetOwner();
    if (!Owner) return nullptr;
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

bool USlicingSkelMeshComponent::SetupProceduralMeshComponent(bool bForceNew)
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

void USlicingSkelMeshComponent::DrawDebugBoneWeightsOnVertices(const UWorld* InWorld, UProceduralMeshComponent* InProcMeshComponent,
    const TMap<int32, float>& BoneWeightsMap, FColor DebugColor, bool bPersistentLines, float LifeTime, float SphereRadius ) const
{
      if (!InWorld || !InProcMeshComponent || BoneWeightsMap.Num() == 0)
    {
        return;
    }

    const FTransform ComponentToWorldTransform = InProcMeshComponent->GetComponentTransform();

    // BoneWeightsMap (글로벌 PMC 버텍스 인덱스 -> BoneWeight)을 순회합니다.
    for (const auto& Pair : BoneWeightsMap)
    {
        int32 VertexIndex = Pair.Key;
        float BoneWeight = Pair.Value;

        if (BoneWeight < 0.f) BoneWeight = 0.f; // -1 (캡 버텍스 등)은 가중치 0으로 간주

        // GlobalVertexIndex에 해당하는 FProcMeshVertex의 위치를 찾아야 합니다.
        FVector VertexLocalPosition = FVector::ZeroVector;
        bool bFoundVertex = false;

        int32 VertexCountSoFar = 0;
        for (int32 SectionIndex = 0; SectionIndex < InProcMeshComponent->GetNumSections(); ++SectionIndex)
        {
            const FProcMeshSection* ProcSection = InProcMeshComponent->GetProcMeshSection(SectionIndex);
            if (!ProcSection) continue;
            
            if (ProcSection->ProcVertexBuffer.IsValidIndex(VertexIndex))
            {
                VertexLocalPosition = ProcSection->ProcVertexBuffer[VertexIndex].Position;
                bFoundVertex = true;
                break; // 해당 버텍스를 찾았으므로 섹션 루프 중단
            }
            
            VertexCountSoFar += ProcSection->ProcVertexBuffer.Num();
        }

        if (bFoundVertex)
        {
            FVector WorldVertexLocation = ComponentToWorldTransform.TransformPosition(VertexLocalPosition);
            
            FColor VertexDebugColor = FLinearColor::LerpUsingHSV(FLinearColor(1, 0, 1, 1), FLinearColor::Red, BoneWeight).ToFColor(true);
            
            DrawDebugSphere(
                InWorld,
                WorldVertexLocation,
                SphereRadius,
                8, // Segments
                VertexDebugColor,
                bPersistentLines,
                LifeTime,
                0 // DepthPriority
            );

            // FString BoneWeightText = FString::Printf(TEXT("%.2f"), BoneWeight);
            // DrawDebugString(InWorld, WorldVertexLocation, BoneWeightText, nullptr, VertexDebugColor, LifeTime, true);
        }
        else
        {
            // 이론적으로 이 경우는 발생하지 않아야 함 (BoneWeightsMap의 인덱스가 유효하다면)
            UE_LOG(LogTemp, Warning, TEXT("DrawDebugBoneWeightsOnVertices: Could not find vertex position for global index %d in ProcMeshComponent %s"), VertexIndex, *InProcMeshComponent->GetName());
        }
    }
}
