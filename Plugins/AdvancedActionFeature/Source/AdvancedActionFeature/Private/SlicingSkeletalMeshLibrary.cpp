// Fill out your copyright notice in the Description page of Project Settings.


#include "SlicingSkeletalMeshLibrary.h"
#include "KismetProceduralMeshLibrary.h"
#include "Components/StaticMeshComponent.h"
#include "PhysicsEngine/BodySetup.h"
#include "Materials/MaterialInterface.h"

#include "StaticMeshResources.h"
#include "Engine/StaticMesh.h"
#include "GeomTools.h"

#include "Components/SkeletalMeshComponent.h"

#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h" 
#include "DrawDebugHelpers.h"

#include "Logging/MessageLog.h"


/** Util that returns 1 ir on positive side of plane, -1 if negative, or 0 if split by plane */
int32 BoxPlaneCompare(FBox InBox, const FPlane& InPlane)
{
	FVector BoxCenter, BoxExtents;
	InBox.GetCenterAndExtents(BoxCenter, BoxExtents);

	// Find distance of box center from plane
	FVector::FReal BoxCenterDist = InPlane.PlaneDot(BoxCenter);

	// See size of box in plane normal direction
	FVector::FReal BoxSize = FVector::BoxPushOut(InPlane, BoxExtents);

	if (BoxCenterDist > BoxSize)
	{
		return 1;
	}
	else if (BoxCenterDist < -BoxSize)
	{
		return -1;
	}
	else
	{
		return 0;
	}
}

/** Take two static mesh verts and interpolate all values between them */
FProcMeshVertex InterpolateVert(const FProcMeshVertex& V0, const FProcMeshVertex& V1, float Alpha)
{
	FProcMeshVertex Result;

	// Handle dodgy alpha
	if (FMath::IsNaN(Alpha) || !FMath::IsFinite(Alpha))
	{
		Result = V1;
		return Result;
	}

	Result.Position = FMath::Lerp(V0.Position, V1.Position, Alpha);

	Result.Normal = FMath::Lerp(V0.Normal, V1.Normal, Alpha);

	Result.Tangent.TangentX = FMath::Lerp(V0.Tangent.TangentX, V1.Tangent.TangentX, Alpha);
	Result.Tangent.bFlipTangentY = V0.Tangent.bFlipTangentY; // Assume flipping doesn't change along edge...

	Result.UV0 = FMath::Lerp(V0.UV0, V1.UV0, Alpha);

	Result.Color.R = FMath::Clamp(FMath::TruncToInt(FMath::Lerp(float(V0.Color.R), float(V1.Color.R), Alpha)), 0, 255);
	Result.Color.G = FMath::Clamp(FMath::TruncToInt(FMath::Lerp(float(V0.Color.G), float(V1.Color.G), Alpha)), 0, 255);
	Result.Color.B = FMath::Clamp(FMath::TruncToInt(FMath::Lerp(float(V0.Color.B), float(V1.Color.B), Alpha)), 0, 255);
	Result.Color.A = FMath::Clamp(FMath::TruncToInt(FMath::Lerp(float(V0.Color.A), float(V1.Color.A), Alpha)), 0, 255);

	return Result;
}

/** Transform triangle from 2D to 3D static-mesh triangle. */
void Transform2DPolygonTo3D(const FUtilPoly2D& InPoly, const FMatrix& InMatrix, TArray<FProcMeshVertex>& OutVerts, FBox& OutBox)
{
	FVector3f PolyNormal = (FVector3f)-InMatrix.GetUnitAxis(EAxis::Z);
	FProcMeshTangent PolyTangent(InMatrix.GetUnitAxis(EAxis::X), false);

	for (int32 VertexIndex = 0; VertexIndex < InPoly.Verts.Num(); VertexIndex++)
	{
		const FUtilVertex2D& InVertex = InPoly.Verts[VertexIndex];

		FProcMeshVertex NewVert;

		NewVert.Position = InMatrix.TransformPosition(FVector(InVertex.Pos.X, InVertex.Pos.Y, 0.f));
		NewVert.Normal = (FVector)PolyNormal;
		NewVert.Tangent = PolyTangent;
		NewVert.Color = InVertex.Color;
		NewVert.UV0 = InVertex.UV;

		OutVerts.Add(NewVert);

		// Update bounding box
		OutBox += NewVert.Position;
	}
}

/** Given a polygon, decompose into triangles. */
bool TriangulatePoly(TArray<uint32>& OutTris, const TArray<FProcMeshVertex>& PolyVerts, int32 VertBase, const FVector3f& PolyNormal)
{
	// Can't work if not enough verts for 1 triangle
	int32 NumVerts = PolyVerts.Num() - VertBase;
	if (NumVerts < 3)
	{
		OutTris.Add(0);
		OutTris.Add(2);
		OutTris.Add(1);

		// Return true because poly is already a tri
		return true;
	}

	// Remember initial size of OutTris, in case we need to give up and return to this size
	const int32 TriBase = OutTris.Num();

	// Init array of vert indices, in order. We'll modify this
	TArray<int32> VertIndices;
	VertIndices.AddUninitialized(NumVerts);
	for (int VertIndex = 0; VertIndex < NumVerts; VertIndex++)
	{
		VertIndices[VertIndex] = VertBase + VertIndex;
	}

	// Keep iterating while there are still vertices
	while (VertIndices.Num() >= 3)
	{
		// Look for an 'ear' triangle
		bool bFoundEar = false;
		for (int32 EarVertexIndex = 0; EarVertexIndex < VertIndices.Num(); EarVertexIndex++)
		{
			// Triangle is 'this' vert plus the one before and after it
			const int32 AIndex = (EarVertexIndex == 0) ? VertIndices.Num() - 1 : EarVertexIndex - 1;
			const int32 BIndex = EarVertexIndex;
			const int32 CIndex = (EarVertexIndex + 1) % VertIndices.Num();

			const FProcMeshVertex& AVert = PolyVerts[VertIndices[AIndex]];
			const FProcMeshVertex& BVert = PolyVerts[VertIndices[BIndex]];
			const FProcMeshVertex& CVert = PolyVerts[VertIndices[CIndex]];

			// Check that this vertex is convex (cross product must be positive)
			const FVector3f ABEdge = FVector3f(BVert.Position - AVert.Position);
			const FVector3f ACEdge = FVector3f(CVert.Position - AVert.Position);
			const float TriangleDeterminant = (ABEdge ^ ACEdge) | PolyNormal;
			if (TriangleDeterminant > 0.f)
			{
				continue;
			}

			bool bFoundVertInside = false;
			// Look through all verts before this in array to see if any are inside triangle
			for (int32 VertexIndex = 0; VertexIndex < VertIndices.Num(); VertexIndex++)
			{
				const FProcMeshVertex& TestVert = PolyVerts[VertIndices[VertexIndex]];

				if(	VertexIndex != AIndex && 
					VertexIndex != BIndex && 
					VertexIndex != CIndex &&
					FGeomTools::PointInTriangle((FVector3f)AVert.Position, (FVector3f)BVert.Position, (FVector3f)CVert.Position, (FVector3f)TestVert.Position) )
				{
					bFoundVertInside = true;
					break;
				}
			}

			// Triangle with no verts inside - its an 'ear'! 
			if (!bFoundVertInside)
			{
				OutTris.Add(VertIndices[AIndex]);
				OutTris.Add(VertIndices[CIndex]);
				OutTris.Add(VertIndices[BIndex]);

				// And remove vertex from polygon
				VertIndices.RemoveAt(EarVertexIndex);

				bFoundEar = true;
				break;
			}
		}

		// If we couldn't find an 'ear' it indicates something is bad with this polygon - discard triangles and return.
		if (!bFoundEar)
		{
			OutTris.SetNum(TriBase, EAllowShrinking::Yes);
			return false;
		}
	}

	return true;
}

/** Util to slice a convex hull with a plane */
void SliceConvexElem(const FKConvexElem& InConvex, const FPlane& SlicePlane, TArray<FVector>& OutConvexVerts)
{
	// Get set of planes that make up hull
	TArray<FPlane> ConvexPlanes;
	InConvex.GetPlanes(ConvexPlanes);

	if (ConvexPlanes.Num() >= 4)
	{
		// Add on the slicing plane (need to flip as it culls geom in the opposite sense to our geom culling code)
		ConvexPlanes.Add(SlicePlane.Flip());

		// Create output convex based on new set of planes
		FKConvexElem SlicedElem;
		bool bSuccess = SlicedElem.HullFromPlanes(ConvexPlanes, InConvex.VertexData);
		if (bSuccess)
		{
			OutConvexVerts = SlicedElem.VertexData;
		}
	}
}


void USlicingSkeletalMeshLibrary::SliceProceduralMesh(UProceduralMeshComponent* InProcMesh, FVector PlanePosition,
	FVector PlaneNormal, bool bCreateOtherHalf, UProceduralMeshComponent*& OutOtherHalfProcMesh,
	EProcMeshSliceCapOption CapOption, UMaterialInterface* CapMaterial, TMap<uint32, uint32>& OutSlicedToBaseVertexIndex,
        TMap<uint32, uint32>& OutOtherSlicedToBaseVertexIndex)
{
	if (InProcMesh != nullptr)
	{
		// Transform plane from world to local space
		FTransform ProcCompToWorld = InProcMesh->GetComponentToWorld();
		FVector LocalPlanePos = ProcCompToWorld.InverseTransformPosition(PlanePosition);
		FVector LocalPlaneNormal = ProcCompToWorld.InverseTransformVectorNoScale(PlaneNormal);
		LocalPlaneNormal = LocalPlaneNormal.GetSafeNormal(); // Ensure normalized

		FPlane SlicePlane(LocalPlanePos, LocalPlaneNormal);

		// Set of sections to add to the 'other half' component
		TArray<FProcMeshSection> OtherSections;
		// Material for each section of other half
		TArray<UMaterialInterface*> OtherMaterials;
		
		// Set of new edges created by clipping polys by plane
		TArray<FUtilEdge3D> ClipEdges;

		// InProcMesh (잘리고 남은 부분)의 섹션별 누적 버텍스 수 (글로벌 인덱스 계산용)
		int32 GlobalVertexOffsetForSlicedMesh = 0;
		// OtherHalfProcMesh의 섹션별 누적 버텍스 수 (글로벌 인덱스 계산용, OtherSections 기준)
		int32 GlobalVertexOffsetForOtherHalf = 0;
		
		for (int32 SectionIndex = 0; SectionIndex < InProcMesh->GetNumSections(); SectionIndex++)
		{
			FProcMeshSection* BaseSection = InProcMesh->GetProcMeshSection(SectionIndex);
			// If we have a section, and it has some valid geom
			if (BaseSection != nullptr && BaseSection->ProcIndexBuffer.Num() > 0 && BaseSection->ProcVertexBuffer.Num() > 0)
			{
				int32 CurrentSectionSlicedVertCount = 0;
				int32 CurrentSectionOtherHalfVertCount = 0;
				
				// Compare bounding box of section with slicing plane
				int32 BoxCompare = BoxPlaneCompare(BaseSection->SectionLocalBox, SlicePlane);

				// Box totally clipped, clear section
				if (BoxCompare == -1) // 섹션 전체가 OtherHalf로
				{
					// Add entire section to other half
					if (bCreateOtherHalf)
					{
						for (int32 BaseVertIdx = 0; BaseVertIdx < BaseSection->ProcVertexBuffer.Num(); ++BaseVertIdx)
						{
							// Key: OtherHalf 메쉬의 새 글로벌 인덱스, Value: 원본 BaseSection의 로컬 인덱스
							OutOtherSlicedToBaseVertexIndex.Add((GlobalVertexOffsetForOtherHalf + BaseVertIdx), BaseVertIdx);
						}
						OtherSections.Add(*BaseSection);
						OtherMaterials.Add(InProcMesh->GetMaterial(SectionIndex));
						CurrentSectionOtherHalfVertCount = BaseSection->ProcVertexBuffer.Num();
					}

					InProcMesh->ClearMeshSection(SectionIndex);
				}
				// Box totally on one side of plane, leave it alone, do nothing
				else if (BoxCompare == 1) // 섹션 전체가 InProcMesh에 남음
				{
					for (int32 BaseVertIdx = 0; BaseVertIdx < BaseSection->ProcVertexBuffer.Num(); ++BaseVertIdx)
					{
						// Key: InProcMesh의 새 글로벌 인덱스, Value: 원본 BaseSection의 로컬 인덱스
						OutSlicedToBaseVertexIndex.Add((GlobalVertexOffsetForSlicedMesh + BaseVertIdx), BaseVertIdx);
					}
					CurrentSectionSlicedVertCount = BaseSection->ProcVertexBuffer.Num();
				}
				// Box intersects plane, need to clip some polys!
				else // 섹션이 절단됨
				{
					// New section for geometry
					FProcMeshSection NewSection;

					// New section for 'other half' geometry (if desired)
					FProcMeshSection* NewOtherSection = nullptr;
					if (bCreateOtherHalf)
					{
						int32 OtherSectionIndex = OtherSections.Add(FProcMeshSection());
						NewOtherSection = &OtherSections[OtherSectionIndex];

						OtherMaterials.Add(InProcMesh->GetMaterial(SectionIndex)); // Remember material for this section
					}

					// Map of base vert index to sliced vert index
					TMap<int32, int32> BaseToSlicedVertIndex;
					TMap<int32, int32> BaseToOtherSlicedVertIndex;

					const int32 NumBaseVerts = BaseSection->ProcVertexBuffer.Num();

					// Distance of each base vert from slice plane
					TArray<float> VertDistance;
					VertDistance.AddUninitialized(NumBaseVerts);

					// Build vertex buffer 
					for (int32 BaseVertIndex = 0; BaseVertIndex < NumBaseVerts; BaseVertIndex++)
					{
						FProcMeshVertex& BaseVert = BaseSection->ProcVertexBuffer[BaseVertIndex];
						// Calc distance from plane
						VertDistance[BaseVertIndex] = SlicePlane.PlaneDot(BaseVert.Position);

						// See if vert is being kept in this section
						if (VertDistance[BaseVertIndex] > 0.f)
						{
							// Copy to sliced v buffer
							int32 SlicedVertIndex = NewSection.ProcVertexBuffer.Add(BaseVert);
							// Update section bounds
							NewSection.SectionLocalBox += BaseVert.Position;
							// Add to map
							BaseToSlicedVertIndex.Add(BaseVertIndex, SlicedVertIndex);
							OutSlicedToBaseVertexIndex.Add( SlicedVertIndex, BaseVertIndex);
						}
						// Or add to other half if desired
						else if(NewOtherSection != nullptr)
						{
							int32 SlicedVertIndex = NewOtherSection->ProcVertexBuffer.Add(BaseVert);
							NewOtherSection->SectionLocalBox += BaseVert.Position;
							BaseToOtherSlicedVertIndex.Add(BaseVertIndex, SlicedVertIndex);
							OutOtherSlicedToBaseVertexIndex.Add( SlicedVertIndex, BaseVertIndex);
						}
					}


					// Iterate over base triangles (ie 3 indices at a time)
					for (int32 BaseIndex = 0; BaseIndex < BaseSection->ProcIndexBuffer.Num(); BaseIndex += 3)
					{
						int32 BaseV[3]; // Triangle vert indices in original mesh
						int32* SlicedV[3]; // Pointers to vert indices in new v buffer
						int32* SlicedOtherV[3]; // Pointers to vert indices in new 'other half' v buffer

						// For each vertex..
						for (int32 i = 0; i < 3; i++)
						{
							// Get triangle vert index
							BaseV[i] = BaseSection->ProcIndexBuffer[BaseIndex + i];
							// Look up in sliced v buffer
							SlicedV[i] = BaseToSlicedVertIndex.Find(BaseV[i]);
							// Look up in 'other half' v buffer (if desired)
							if (bCreateOtherHalf)
							{
								SlicedOtherV[i] = BaseToOtherSlicedVertIndex.Find(BaseV[i]);
								// Each base vert _must_ exist in either BaseToSlicedVertIndex or BaseToOtherSlicedVertIndex 
								check((SlicedV[i] != nullptr) != (SlicedOtherV[i] != nullptr));
							}
						}

						// If all verts survived plane cull, keep the triangle
						if (SlicedV[0] != nullptr && SlicedV[1] != nullptr && SlicedV[2] != nullptr)
						{
							NewSection.ProcIndexBuffer.Add(*SlicedV[0]);
							NewSection.ProcIndexBuffer.Add(*SlicedV[1]);
							NewSection.ProcIndexBuffer.Add(*SlicedV[2]);
						}
						// If all verts were removed by plane cull
						else if (SlicedV[0] == nullptr && SlicedV[1] == nullptr && SlicedV[2] == nullptr)
						{
							// If creating other half, add all verts to that
							if (NewOtherSection != nullptr)
							{
								NewOtherSection->ProcIndexBuffer.Add(*SlicedOtherV[0]);
								NewOtherSection->ProcIndexBuffer.Add(*SlicedOtherV[1]);
								NewOtherSection->ProcIndexBuffer.Add(*SlicedOtherV[2]);
							}
						}
						// If partially culled, clip to create 1 or 2 new triangles
						else
						{
							int32 FinalVerts[4];
							int32 NumFinalVerts = 0;

							int32 OtherFinalVerts[4];
							int32 NumOtherFinalVerts = 0;

							FUtilEdge3D NewClipEdge;
							int32 ClippedEdges = 0;

							float PlaneDist[3];
							PlaneDist[0] = VertDistance[BaseV[0]];
							PlaneDist[1] = VertDistance[BaseV[1]];
							PlaneDist[2] = VertDistance[BaseV[2]];

							for (int32 EdgeIdx = 0; EdgeIdx < 3; EdgeIdx++)
							{
								int32 ThisVert = EdgeIdx;

								// If start vert is inside, add it.
								if (SlicedV[ThisVert] != nullptr)
								{
									check(NumFinalVerts < 4);
									FinalVerts[NumFinalVerts++] = *SlicedV[ThisVert];
								}
								// If not, add to other side
								else if(bCreateOtherHalf)
								{
									check(NumOtherFinalVerts < 4);
									OtherFinalVerts[NumOtherFinalVerts++] = *SlicedOtherV[ThisVert];
								}

								// If start and next vert are on opposite sides, add intersection
								int32 NextVert = (EdgeIdx + 1) % 3;

								if ((SlicedV[EdgeIdx] == nullptr) != (SlicedV[NextVert] == nullptr))
								{
									// Find distance along edge that plane is
									float Alpha = -PlaneDist[ThisVert] / (PlaneDist[NextVert] - PlaneDist[ThisVert]);
									// Interpolate vertex params to that point
									FProcMeshVertex InterpVert = InterpolateVert(BaseSection->ProcVertexBuffer[BaseV[ThisVert]], BaseSection->ProcVertexBuffer[BaseV[NextVert]], FMath::Clamp(Alpha, 0.0f, 1.0f));

									// Add to vertex buffer
									int32 InterpVertIndex = NewSection.ProcVertexBuffer.Add(InterpVert);
									// Update bounds
									NewSection.SectionLocalBox += InterpVert.Position;

									// Save vert index for this poly
									check(NumFinalVerts < 4);
									FinalVerts[NumFinalVerts++] = InterpVertIndex;

									// If desired, add to the poly for the other half as well
									if (NewOtherSection != nullptr)
									{
										int32 OtherInterpVertIndex = NewOtherSection->ProcVertexBuffer.Add(InterpVert);
										NewOtherSection->SectionLocalBox += InterpVert.Position;
										check(NumOtherFinalVerts < 4);
										OtherFinalVerts[NumOtherFinalVerts++] = OtherInterpVertIndex;
									}

									// When we make a new edge on the surface of the clip plane, save it off.
									check(ClippedEdges < 2);
									if (ClippedEdges == 0)
									{
										NewClipEdge.V0 = (FVector3f)InterpVert.Position;
									}
									else
									{
										NewClipEdge.V1 = (FVector3f)InterpVert.Position;
									}

									ClippedEdges++;
								}
							}

							// Triangulate the clipped polygon.
							for (int32 VertexIndex = 2; VertexIndex < NumFinalVerts; VertexIndex++)
							{
								NewSection.ProcIndexBuffer.Add(FinalVerts[0]);
								NewSection.ProcIndexBuffer.Add(FinalVerts[VertexIndex - 1]);
								NewSection.ProcIndexBuffer.Add(FinalVerts[VertexIndex]);
							}

							// If we are making the other half, triangulate that as well
							if (NewOtherSection != nullptr)
							{
								for (int32 VertexIndex = 2; VertexIndex < NumOtherFinalVerts; VertexIndex++)
								{
									NewOtherSection->ProcIndexBuffer.Add(OtherFinalVerts[0]);
									NewOtherSection->ProcIndexBuffer.Add(OtherFinalVerts[VertexIndex - 1]);
									NewOtherSection->ProcIndexBuffer.Add(OtherFinalVerts[VertexIndex]);
								}
							}

							check(ClippedEdges != 1); // Should never clip just one edge of the triangle

							// If we created a new edge, save that off here as well
							if (ClippedEdges == 2)
							{
								ClipEdges.Add(NewClipEdge);
							}
						}
					}

					// Remove 'other' section from array if no valid geometry for it
					if (NewOtherSection != nullptr && (NewOtherSection->ProcIndexBuffer.Num() == 0 || NewOtherSection->ProcVertexBuffer.Num() == 0))
					{
						OtherSections.RemoveAt(OtherSections.Num() - 1);
					}

					// If we have some valid geometry, update section
					if (NewSection.ProcIndexBuffer.Num() > 0 && NewSection.ProcVertexBuffer.Num() > 0)
					{
						// Assign new geom to this section
						InProcMesh->SetProcMeshSection(SectionIndex, NewSection);
					}
					// If we don't, remove this section
					else
					{
						InProcMesh->ClearMeshSection(SectionIndex);
					}
				}
			}
		}

		// Create cap geometry (if some edges to create it from)
		if (CapOption != EProcMeshSliceCapOption::NoCap && ClipEdges.Num() > 0)
		{
			FProcMeshSection CapSection;
			int32 CapSectionIndex = INDEX_NONE;

			// If using an existing section, copy that info first
			if (CapOption == EProcMeshSliceCapOption::UseLastSectionForCap)
			{
				CapSectionIndex = InProcMesh->GetNumSections() - 1;
				CapSection = *InProcMesh->GetProcMeshSection(CapSectionIndex);
			}
			// Adding new section for cap
			else
			{
				CapSectionIndex = InProcMesh->GetNumSections();
			}

			// Project 3D edges onto slice plane to form 2D edges
			TArray<FUtilEdge2D> Edges2D;
			FUtilPoly2DSet PolySet;
			FGeomTools::ProjectEdges(Edges2D, PolySet.PolyToWorld, ClipEdges, SlicePlane);

			// Find 2D closed polygons from this edge soup
			FGeomTools::Buid2DPolysFromEdges(PolySet.Polys, Edges2D, FColor(255, 255, 255, 255));

			// Remember start point for vert and index buffer before adding and cap geom
			int32 CapVertBase = CapSection.ProcVertexBuffer.Num();
			int32 CapIndexBase = CapSection.ProcIndexBuffer.Num();

			// Triangulate each poly
			for (int32 PolyIdx = 0; PolyIdx < PolySet.Polys.Num(); PolyIdx++)
			{
				// Generate UVs for the 2D polygon.
				FGeomTools::GeneratePlanarTilingPolyUVs(PolySet.Polys[PolyIdx], 64.f);

				// Remember start of vert buffer before adding triangles for this poly
				int32 PolyVertBase = CapSection.ProcVertexBuffer.Num();

				// Transform from 2D poly verts to 3D
				Transform2DPolygonTo3D(PolySet.Polys[PolyIdx], PolySet.PolyToWorld, CapSection.ProcVertexBuffer, CapSection.SectionLocalBox);

				// Triangulate this polygon
				TriangulatePoly(CapSection.ProcIndexBuffer, CapSection.ProcVertexBuffer, PolyVertBase, (FVector3f)LocalPlaneNormal);
			}

			// Set geom for cap section
			InProcMesh->SetProcMeshSection(CapSectionIndex, CapSection);

			// If creating new section for cap, assign cap material to it
			if (CapOption == EProcMeshSliceCapOption::CreateNewSectionForCap)
			{
				InProcMesh->SetMaterial(CapSectionIndex, CapMaterial);
			}

			// If creating the other half, copy cap geom into other half sections
			if (bCreateOtherHalf)
			{
				// Find section we want to use for the cap on the 'other half'
				FProcMeshSection* OtherCapSection;
				if (CapOption == EProcMeshSliceCapOption::CreateNewSectionForCap)
				{
					OtherSections.Add(FProcMeshSection());
					OtherMaterials.Add(CapMaterial);
				}
				OtherCapSection = &OtherSections.Last();

				// Remember current base index for verts in 'other cap section'
				int32 OtherCapVertBase = OtherCapSection->ProcVertexBuffer.Num();

				// Copy verts from cap section into other cap section
				for (int32 VertIdx = CapVertBase; VertIdx < CapSection.ProcVertexBuffer.Num(); VertIdx++)
				{
					FProcMeshVertex OtherCapVert = CapSection.ProcVertexBuffer[VertIdx];

					// Flip normal and tangent TODO: FlipY?
					OtherCapVert.Normal *= -1.f;
					OtherCapVert.Tangent.TangentX *= -1.f;

					// Add to other cap v buffer
					OtherCapSection->ProcVertexBuffer.Add(OtherCapVert);
					// And update bounding box
					OtherCapSection->SectionLocalBox += OtherCapVert.Position;
				}

				// Find offset between main cap verts and other cap verts
				int32 VertOffset = OtherCapVertBase - CapVertBase;

				// Copy indices over as well
				for (int32 IndexIdx = CapIndexBase; IndexIdx < CapSection.ProcIndexBuffer.Num(); IndexIdx += 3)
				{
					// Need to offset and change winding
					OtherCapSection->ProcIndexBuffer.Add(CapSection.ProcIndexBuffer[IndexIdx + 0] + VertOffset);
					OtherCapSection->ProcIndexBuffer.Add(CapSection.ProcIndexBuffer[IndexIdx + 2] + VertOffset);
					OtherCapSection->ProcIndexBuffer.Add(CapSection.ProcIndexBuffer[IndexIdx + 1] + VertOffset);
				}
			}
		}

		// Array of sliced collision shapes
		TArray< TArray<FVector> > SlicedCollision;
		TArray< TArray<FVector> > OtherSlicedCollision;

		UBodySetup* ProcMeshBodySetup = InProcMesh->GetBodySetup();

		for (int32 ConvexIndex = 0; ConvexIndex < ProcMeshBodySetup->AggGeom.ConvexElems.Num(); ConvexIndex++)
		{
			FKConvexElem& BaseConvex = ProcMeshBodySetup->AggGeom.ConvexElems[ConvexIndex];

			int32 BoxCompare = BoxPlaneCompare(BaseConvex.ElemBox, SlicePlane);

			// If box totally clipped, add to other half (if desired)
			if (BoxCompare == -1)
			{
				if (bCreateOtherHalf)
				{
					OtherSlicedCollision.Add(BaseConvex.VertexData);
				}
			}
			// If box totally valid, just keep mesh as is
			else if (BoxCompare == 1)
			{
				SlicedCollision.Add(BaseConvex.VertexData);				// LWC_TODO: Perf pessimization
			}
			// Need to actually slice the convex shape
			else
			{
				TArray<FVector> SlicedConvexVerts;
				SliceConvexElem(BaseConvex, SlicePlane, SlicedConvexVerts);
				// If we got something valid, add it
				if (SlicedConvexVerts.Num() >= 4)
				{
					SlicedCollision.Add(SlicedConvexVerts);
				}

				// Slice again to get the other half of the collision, if desired
				if (bCreateOtherHalf)
				{
					TArray<FVector> OtherSlicedConvexVerts;
					SliceConvexElem(BaseConvex, SlicePlane.Flip(), OtherSlicedConvexVerts);
					if (OtherSlicedConvexVerts.Num() >= 4)
					{
						OtherSlicedCollision.Add(OtherSlicedConvexVerts);
					}
				}
			}
		}

		// Update collision of proc mesh
		InProcMesh->SetCollisionConvexMeshes(SlicedCollision);

		// If creating other half, create component now
		if (bCreateOtherHalf)
		{
			// Create new component with the same outer as the proc mesh passed in
			OutOtherHalfProcMesh = NewObject<UProceduralMeshComponent>(InProcMesh->GetOuter());

			// Set transform to match source component
			OutOtherHalfProcMesh->SetWorldTransform(InProcMesh->GetComponentTransform());

			// Add each section of geometry
			for (int32 SectionIndex = 0; SectionIndex < OtherSections.Num(); SectionIndex++)
			{
				OutOtherHalfProcMesh->SetProcMeshSection(SectionIndex, OtherSections[SectionIndex]);
				OutOtherHalfProcMesh->SetMaterial(SectionIndex, OtherMaterials[SectionIndex]);
			}

			// Copy collision settings from input mesh
			OutOtherHalfProcMesh->SetCollisionProfileName(InProcMesh->GetCollisionProfileName());
			OutOtherHalfProcMesh->SetCollisionEnabled(InProcMesh->GetCollisionEnabled());
			OutOtherHalfProcMesh->bUseComplexAsSimpleCollision = InProcMesh->bUseComplexAsSimpleCollision;

			// Assign sliced collision
			OutOtherHalfProcMesh->SetCollisionConvexMeshes(OtherSlicedCollision);

			// Finally register
			OutOtherHalfProcMesh->RegisterComponent();
		}
	}
}

float USlicingSkeletalMeshLibrary::GetBoneWeightForVertex(int32 VertexIndex, int32 TargetBoneIndex,
	const FSkelMeshRenderSection* SkelMeshRenderSection, const FSkeletalMeshLODRenderData* LODRenderData,
	const FSkinWeightVertexBuffer* SkinWeightBuffer)
{
	if (!SkinWeightBuffer || SkinWeightBuffer->GetNumVertices() == 0) return -1;
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
	return -1;
}

FBoneWeightsInfo USlicingSkeletalMeshLibrary::GetBoneWeightsForVertex(const USkeletalMeshComponent* SkelComp, int32 VertexIndex,
	const FSkelMeshRenderSection* SkelMeshRenderSection, const FSkeletalMeshLODRenderData* LODRenderData,
	const FSkinWeightVertexBuffer* SkinWeightBuffer)
{
	FBoneWeightsInfo BoneWeightsInfo;
	
	if (!SkinWeightBuffer || SkinWeightBuffer->GetNumVertices() == 0) return BoneWeightsInfo;
	const int32 MaxInfluences = SkinWeightBuffer->GetMaxBoneInfluences();
	const TArray<FBoneIndexType>& BoneMap = SkelMeshRenderSection->BoneMap;
	for (int32 InfluenceIdx = 0; InfluenceIdx < MaxInfluences; InfluenceIdx++)
	{
		int32 LocalBoneIndex = SkinWeightBuffer->GetBoneIndex(VertexIndex, InfluenceIdx);
		if (LocalBoneIndex >= BoneMap.Num()) continue; // Ensure we are within bounds
        
		int32 GlobalBoneIndex = BoneMap[LocalBoneIndex];
		float BoneWeight = SkinWeightBuffer->GetBoneWeight(VertexIndex, InfluenceIdx) / 65535.0f;
		BoneWeightsInfo.BoneWeights.Add(BoneWeight);
		BoneWeightsInfo.InfluencingBoneIndices.Add(GlobalBoneIndex);
		BoneWeightsInfo.BoneNames.Add(SkelComp->GetSkeletalMeshAsset()->GetRefSkeleton().GetBoneName(GlobalBoneIndex));
	}
	return BoneWeightsInfo;
}

TMap<uint32, float> USlicingSkeletalMeshLibrary::GetBoneWeightMapForProceduralVertices(const int32 TargetLODIndex,
	const USkeletalMeshComponent* InSkelComp, int32 TargetGlobalBoneIndex,
	const TMap<uint32, uint32>& SlicePmcToOriginalPmc, const TMap<uint32, uint32>& OriginalPmcToSkeletal)
{
	TMap<uint32, float> BoneWeightsMap;

    if (!InSkelComp || !InSkelComp->GetSkeletalMeshAsset() || TargetGlobalBoneIndex == INDEX_NONE)
    {
        UE_LOG(LogTemp, Error, TEXT("GetBoneWeightMapForProceduralVertices: Invalid input SkelComp or TargetGlobalBoneIndex."));
        return BoneWeightsMap;
    }

    const FSkeletalMeshRenderData* SkelMeshRenderData = InSkelComp->GetSkeletalMeshRenderData();
    if (!SkelMeshRenderData || SkelMeshRenderData->LODRenderData.Num() == 0)
    {
        UE_LOG(LogTemp, Error, TEXT("GetBoneWeightMapForProceduralVertices: No SkeletalMeshRenderData or LODRenderData found."));
        return BoneWeightsMap;
    }

    if (!SkelMeshRenderData->LODRenderData.IsValidIndex(TargetLODIndex)) // TargetLODIndex는 멤버 변수로 가정
    {
        UE_LOG(LogTemp, Error, TEXT("GetBoneWeightMapForProceduralVertices: TargetLODIndex %d is invalid."), TargetLODIndex);
        return BoneWeightsMap;
    }
    const FSkeletalMeshLODRenderData& LODRenderData = SkelMeshRenderData->LODRenderData[TargetLODIndex];
    const FSkinWeightVertexBuffer* SkinWeightBuffer = LODRenderData.GetSkinWeightVertexBuffer();
    if (!SkinWeightBuffer)
    {
        UE_LOG(LogTemp, Error, TEXT("GetBoneWeightMapForProceduralVertices: SkinWeightBuffer is null for LOD %d."), TargetLODIndex);
        return BoneWeightsMap;
    }

    // SlicePmcToOriginalPmc (Key: NewPMCVertIdx, Value: PreSlicePMCLocalIdx)을 순회
    for (const auto& NewToOldPair : SlicePmcToOriginalPmc)
    {
        int32 NewPMCVertIdx = NewToOldPair.Key;
        int32 PreSlicePMCLocalIdx = NewToOldPair.Value; // 슬라이스 전 PMC의 로컬 인덱스

        if (PreSlicePMCLocalIdx == -1) // 캡 버텍스 또는 특별히 -1로 매핑된 경우
        {
            BoneWeightsMap.Add(NewPMCVertIdx, 0.f); // 스키닝 가중치 0으로 처리
            continue;
        }

        // OriginalPmcToSkeletal 맵을 사용하여 PreSlicePMCLocalIdx로부터 원본 스켈레탈 메쉬의 글로벌 인덱스를 찾습니다.
        // OriginalPmcToSkeletal의 Key 타입이 uint32이므로 캐스팅합니다.
        const uint32* OriginalSkelGlobalVertexIndexPtr = OriginalPmcToSkeletal.Find(static_cast<uint32>(PreSlicePMCLocalIdx));

        if (OriginalSkelGlobalVertexIndexPtr)
        {
            uint32 OriginalSkelGlobalVertexIndex = *OriginalSkelGlobalVertexIndexPtr;
            float BoneWeight = 0.f; // 기본값

            // 원본 스켈레탈 메쉬의 어떤 섹션에 이 버텍스가 속했는지 찾아야 GetBoneWeightForVertex를 호출 가능.
            bool bFoundSection = false;
            for (const FSkelMeshRenderSection& Section : LODRenderData.RenderSections)
            {
                if (OriginalSkelGlobalVertexIndex >= Section.BaseVertexIndex &&
                    OriginalSkelGlobalVertexIndex < (Section.BaseVertexIndex + Section.NumVertices))
                {
                    BoneWeight = GetBoneWeightForVertex(OriginalSkelGlobalVertexIndex, TargetGlobalBoneIndex, &Section, &LODRenderData, SkinWeightBuffer);
                    bFoundSection = true;
                    break; 
                }
            }
            
            if (!bFoundSection) {
                 UE_LOG(LogTemp, Warning, TEXT("GetBoneWeightMapForProceduralVertices: Could not find render section for original skeletal vertex index: %u (from PreSlicePMCLocalIdx: %d)"), OriginalSkelGlobalVertexIndex, PreSlicePMCLocalIdx);
            }
            BoneWeightsMap.Add(NewPMCVertIdx, BoneWeight); // GetBoneWeightForVertex가 0.f를 반환하면 그대로 사용
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("GetBoneWeightMapForProceduralVertices: Could not find original skeletal mesh vertex in OriginalPmcToSkeletal map for PreSlicePMCLocalIdx: %d. New PMC Vert Idx: %d"), PreSlicePMCLocalIdx, NewPMCVertIdx);
            BoneWeightsMap.Add(NewPMCVertIdx, 0.f);
        }
    }

    return BoneWeightsMap;
}



TMap<uint32, FBoneWeightsInfo> USlicingSkeletalMeshLibrary::GetBoneWeightsInfoMapForSlicedProcMeshVertices(const int32 TargetLODIndex,
	const USkeletalMeshComponent* InSkelComp,
	const TMap<uint32, uint32>& SlicePmcToSkelMap)
{
	TMap<uint32, FBoneWeightsInfo> Results;
    if (!InSkelComp || !InSkelComp->GetSkeletalMeshAsset())
    {
        UE_LOG(LogTemp, Error, TEXT("GetBoneWeightMapForProceduralVertices: Invalid input SkelComp or TargetGlobalBoneIndex."));
        return Results;
    }
    const FSkeletalMeshRenderData* SkelMeshRenderData = InSkelComp->GetSkeletalMeshRenderData();
    if (!SkelMeshRenderData || SkelMeshRenderData->LODRenderData.Num() == 0)
    {
        UE_LOG(LogTemp, Error, TEXT("GetBoneWeightMapForProceduralVertices: No SkeletalMeshRenderData or LODRenderData found."));
        return Results;
    }
    if (!SkelMeshRenderData->LODRenderData.IsValidIndex(TargetLODIndex)) // TargetLODIndex는 멤버 변수로 가정
    {
        UE_LOG(LogTemp, Error, TEXT("GetBoneWeightMapForProceduralVertices: TargetLODIndex %d is invalid."), TargetLODIndex);
        return Results;
    }
    const FSkeletalMeshLODRenderData& LODRenderData = SkelMeshRenderData->LODRenderData[TargetLODIndex];
    const FSkinWeightVertexBuffer* SkinWeightBuffer = LODRenderData.GetSkinWeightVertexBuffer();
    if (!SkinWeightBuffer)
    {
        UE_LOG(LogTemp, Error, TEXT("GetBoneWeightMapForProceduralVertices: SkinWeightBuffer is null for LOD %d."), TargetLODIndex);
        return Results;
    }

    // SlicePmcToOriginalPmc (Key: NewPMCVertIdx, Value: PreSlicePMCLocalIdx)을 순회
    for (const TPair<uint32, uint32>& NewToOldPair : SlicePmcToSkelMap)
    {
        uint32 ProcVertexIndex = NewToOldPair.Key;
        uint32 SkelVertexIndex = NewToOldPair.Value; 
    	FBoneWeightsInfo BoneWeightsInfo;
    	
        if (SkelVertexIndex >= 0)
        {
            TArray<float> BoneWeights; // 기본값

            // 원본 스켈레탈 메쉬의 어떤 섹션에 이 버텍스가 속했는지 찾아야 GetBoneWeightForVertex를 호출 가능.
            bool bFoundSection = false;
            for (const FSkelMeshRenderSection& Section : LODRenderData.RenderSections)
            {
                if (SkelVertexIndex >= Section.BaseVertexIndex &&
                    SkelVertexIndex < (Section.BaseVertexIndex + Section.NumVertices))
                {
                	BoneWeightsInfo = GetBoneWeightsForVertex(InSkelComp, SkelVertexIndex, &Section, &LODRenderData, SkinWeightBuffer);
                    bFoundSection = true;
                    break; 
                }
            }
            if (!bFoundSection) {
                 UE_LOG(LogTemp, Warning, TEXT("GetBoneWeightMapForProceduralVertices: Could not find render section for original skeletal vertex index: %u (from SlicedProcMesh Index: %d)"), SkelVertexIndex, ProcVertexIndex);
            }
            Results.Add(ProcVertexIndex, BoneWeightsInfo); // GetBoneWeightForVertex가 0.f를 반환하면 그대로 사용
        }
    }
	return Results;
}

