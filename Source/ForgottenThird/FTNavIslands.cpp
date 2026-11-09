#include "FTNavIslands.h"

#include "NavMesh/RecastNavMesh.h"

void FTGatherNavIslands(const ARecastNavMesh& NavMesh, TFunctionRef<ftcore::MassingVec(const FVector&)> CornerOf, double SeaLevel,
	FFTNavIslands& Out)
{
	Out = FFTNavIslands();
	TArray<FNavTileRef> Tiles;
	NavMesh.GetAllNavMeshTiles(Tiles);
	Out.Tiles = Tiles.Num();
	std::vector<ftcore::NavPolyRef> AllPolys;
	TArray<NavNodeRef> AllRefs;
	TMap<NavNodeRef, uint32> AllIndex;
	TArray<FNavPoly> TilePolys;
	TArray<FVector> Verts;
	for (const FNavTileRef& Tile : Tiles)
	{
		TilePolys.Reset();
		if (!NavMesh.GetPolysInTile(Tile, TilePolys))
		{
			continue;
		}
		for (const FNavPoly& Poly : TilePolys)
		{
			Verts.Reset();
			if (!NavMesh.GetPolyVerts(Poly.Ref, Verts) || Verts.IsEmpty())
			{
				continue;
			}
			ftcore::NavPolyRef Ref;
			Ref.first = static_cast<uint32>(Out.Corners.size());
			Ref.count = static_cast<uint32>(Verts.Num());
			for (const FVector& V : Verts)
			{
				Out.Corners.push_back(CornerOf(V));
			}
			AllIndex.Add(Poly.Ref, static_cast<uint32>(AllPolys.size()));
			AllRefs.Add(Poly.Ref);
			AllPolys.push_back(Ref);
		}
	}
	Out.Gathered = static_cast<int32>(AllPolys.size());
	// Without a sea, nothing is left out.
	std::vector<std::uint32_t> Keep;
	ftcore::navPolysAboveSea(Out.Corners, AllPolys, SeaLevel, ftcore::kNavSeaMargin, Keep, Out.UnderSea);
	Out.Polys.reserve(Keep.size());
	Out.Refs.Reserve(static_cast<int32>(Keep.size()));
	Out.IndexOf.Reserve(static_cast<int32>(Keep.size()));
	for (const std::uint32_t K : Keep)
	{
		Out.IndexOf.Add(AllRefs[static_cast<int32>(K)], static_cast<uint32>(Out.Polys.size()));
		Out.Refs.Add(AllRefs[static_cast<int32>(K)]);
		Out.Polys.push_back(AllPolys[K]);
	}
	std::vector<ftcore::NavLink> Links;
	TArray<NavNodeRef> Near;
	TArray<NavNodeRef> Through;
	for (int32 I = 0; I < Out.Refs.Num(); ++I)
	{
		Near.Reset();
		if (!NavMesh.GetPolyNeighbors(Out.Refs[I], Near))
		{
			continue;
		}
		for (const NavNodeRef N : Near)
		{
			ftcore::NavLink Link;
			Link.a = static_cast<uint32>(I);
			if (const uint32* J = Out.IndexOf.Find(N))
			{
				Link.b = *J;
				Links.push_back(Link);
				continue;
			}
			// A ground poly left out under the sea joins nothing.
			if (AllIndex.Contains(N))
			{
				continue;
			}
			// Not a ground poly: an off-mesh link, joined to the ground polys at its ends.
			Through.Reset();
			if (NavMesh.GetPolyNeighbors(N, Through))
			{
				for (const NavNodeRef M : Through)
				{
					if (const uint32* J = Out.IndexOf.Find(M))
					{
						Link.b = *J;
						Links.push_back(Link);
					}
				}
			}
		}
	}
	ftcore::navIslands(Out.Corners, Out.Polys, Links, Out.Set);
}
