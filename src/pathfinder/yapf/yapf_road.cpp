/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file yapf_road.cpp The road pathfinding. */

#include "../../stdafx.h"
#include "yapf.hpp"
#include "yapf_node_road.hpp"
#include "../../roadstop_base.h"


template <class Types>
class CYapfCostRoadT
{
public:
	typedef typename Types::Tpf Tpf; ///< pathfinder (derived from THIS class)
	typedef typename Types::TrackFollower TrackFollower; ///< track follower helper
	typedef typename Types::Astar::Node Node; ///< this will be our node type
	typedef typename Node::Key Key;    ///< key to hash tables

protected:
	/** to access inherited path finder */
	Tpf& Yapf()
	{
		return *static_cast<Tpf*>(this);
	}

	int SlopeCost(const PathPos &pos, TileIndex next_tile)
	{
		/* height of the center of the current tile */
		int x1 = TileX(pos.tile) * TILE_SIZE;
		int y1 = TileY(pos.tile) * TILE_SIZE;
		int z1 = GetSlopePixelZ(x1 + TILE_SIZE / 2, y1 + TILE_SIZE / 2);

		/* height of the center of the next tile */
		int x2 = TileX(next_tile) * TILE_SIZE;
		int y2 = TileY(next_tile) * TILE_SIZE;
		int z2 = GetSlopePixelZ(x2 + TILE_SIZE / 2, y2 + TILE_SIZE / 2);

		if (z2 - z1 > 1) {
			/* Slope up */
			return Yapf().PfGetSettings().road_slope_penalty;
		}
		return 0;
	}

	/** return one tile cost */
	inline int OneTileCost(const PathPos &pos)
	{
		int cost = 0;
		/* set base cost */
		if (IsDiagonalTrackdir(pos.td)) {
			cost += YAPF_TILE_LENGTH;
			switch (GetTileType(pos.tile)) {
				case TT_MISC:
					/* Increase the cost for level crossings */
					if (IsLevelCrossingTile(pos.tile)) {
						cost += Yapf().PfGetSettings().road_crossing_penalty;
					}
					break;

				case TT_STATION: {
					const RoadStop *rs = RoadStop::GetByTile(pos.tile, GetRoadStopType(pos.tile));
					if (IsDriveThroughStopTile(pos.tile)) {
						/* Increase the cost for drive-through road stops */
						cost += Yapf().PfGetSettings().road_stop_penalty;
						DiagDirection dir = TrackdirToExitdir(pos.td);
						if (!RoadStop::IsDriveThroughRoadStopContinuation(pos.tile, pos.tile - TileOffsByDiagDir(dir))) {
							/* When we're the first road stop in a 'queue' of them we increase
							 * cost based on the fill percentage of the whole queue. */
							const RoadStop::Entry *entry = rs->GetEntry(dir);
							cost += entry->GetOccupied() * Yapf().PfGetSettings().road_stop_occupied_penalty / entry->GetLength();
						}
					} else {
						/* Increase cost for filled road stops */
						cost += Yapf().PfGetSettings().road_stop_bay_occupied_penalty * (!rs->IsFreeBay(0) + !rs->IsFreeBay(1)) / 2;
					}
					break;
				}

				default:
					break;
			}
		} else {
			/* non-diagonal trackdir */
			cost = YAPF_TILE_CORNER_LENGTH + Yapf().PfGetSettings().road_curve_penalty;
		}
		return cost;
	}

public:
	/**
	 * Called by YAPF to calculate the cost from the origin to the given node.
	 *  Calculates only the cost of given node, adds it to the parent node cost
	 *  and stores the result into Node::m_cost member
	 */
	inline bool PfCalcCost(Node& n, const TrackFollower *tf)
	{
		int segment_cost = 0;
		uint tiles = 0;
		/* start at n and walk to the end of segment */
		PathPos pos = n.GetPos();

		if (IsRoadBridgeTile(pos.tile) && TrackdirToExitdir(ReverseTrackdir(pos.td)) == GetTunnelBridgeDirection(pos.tile)) {
			segment_cost = GetTunnelBridgeLength(pos.tile, GetOtherBridgeEnd(pos.tile)) * YAPF_TILE_LENGTH;
		}

		for (;;) {
			/* base tile cost depending on distance between edges */
			segment_cost += OneTileCost(pos);

			const RoadVehicle *v = Yapf().GetVehicle();
			/* we have reached the vehicle's destination - segment should end here to avoid target skipping */
			if (Yapf().PfDetectDestinationTile(pos)) break;

			/* stop if we have just entered the depot */
			if (IsRoadDepotTile(pos.tile) && pos.td == DiagDirToDiagTrackdir(ReverseDiagDir(GetGroundDepotDirection(pos.tile)))) {
				/* next time we will reverse and leave the depot */
				break;
			}

			/* if there are no reachable trackdirs on new tile, we have end of road */
			TrackFollower F(Yapf().GetVehicle());
			if (!F.Follow(pos)) break;

			/* if there are more trackdirs available & reachable, we are at the end of segment */
			if (!F.m_new.is_single()) break;

			/* stop if RV is on simple loop with no junctions */
			if (F.m_new == n.GetPos()) return false;

			/* if we skipped some tunnel tiles, add their cost */
			segment_cost += F.m_tiles_skipped * YAPF_TILE_LENGTH;
			tiles += F.m_tiles_skipped + 1;

			/* add hilly terrain penalty */
			if (!F.m_new.in_wormhole()) segment_cost += SlopeCost(pos, F.m_new.tile);

			/* add min/max speed penalties */
			int min_speed = 0;
			int max_veh_speed = v->GetDisplayMaxSpeed();
			int max_speed = F.GetSpeedLimit(&min_speed);
			if (max_speed < max_veh_speed) segment_cost += 1 * (max_veh_speed - max_speed);
			if (min_speed > max_veh_speed) segment_cost += 10 * (min_speed - max_veh_speed);

			/* move to the next tile */
			pos = F.m_new;
			if (tiles > MAX_MAP_SIZE) break;
		}

		/* save end of segment back to the node */
		n.m_segment_last = pos;

		/* save also tile cost */
		int parent_cost = (n.m_parent != NULL) ? n.m_parent->m_cost : 0;
		n.m_cost = parent_cost + segment_cost;
		return true;
	}
};


template <class Types>
class CYapfDestinationAnyDepotRoadT
{
public:
	typedef typename Types::Tpf Tpf;                     ///< the pathfinder class (derived from THIS class)
	typedef typename Types::TrackFollower TrackFollower;
	typedef typename Types::Astar::Node Node;            ///< this will be our node type
	typedef typename Node::Key Key;                      ///< key to hash tables

	/** to access inherited path finder */
	Tpf& Yapf()
	{
		return *static_cast<Tpf*>(this);
	}

	/** Called by YAPF to detect if node ends in the desired destination */
	inline bool PfDetectDestination(Node& n)
	{
		return IsRoadDepotTile(n.m_segment_last.tile);
	}

	inline bool PfDetectDestinationTile(const PathPos &pos)
	{
		return IsRoadDepotTile(pos.tile);
	}

	/**
	 * Called by YAPF to calculate cost estimate. Calculates distance to the destination
	 *  adds it to the actual cost from origin and stores the sum to the Node::m_estimate
	 */
	inline bool PfCalcEstimate(Node& n)
	{
		n.m_estimate = n.m_cost;
		return true;
	}
};


template <class Types>
class CYapfDestinationTileRoadT
{
public:
	typedef typename Types::Tpf Tpf;                     ///< the pathfinder class (derived from THIS class)
	typedef typename Types::TrackFollower TrackFollower;
	typedef typename Types::Astar::Node Node;            ///< this will be our node type
	typedef typename Node::Key Key;                      ///< key to hash tables

protected:
	TileIndex    m_destTile;
	StationID    m_dest_station;
	bool         m_bus;
	bool         m_non_artic;

public:
	void SetDestination(const RoadVehicle *v)
	{
		if (v->current_order.IsType(OT_GOTO_STATION)) {
			m_dest_station  = v->current_order.GetDestination();
			m_bus           = v->IsBus();
			m_destTile      = Station::Get(m_dest_station)->GetClosestTile(v->tile, m_bus ? STATION_BUS : STATION_TRUCK);
			m_non_artic     = !v->HasArticulatedPart();
		} else {
			m_dest_station  = INVALID_STATION;
			m_destTile      = v->dest_tile;
		}
	}

protected:
	/** to access inherited path finder */
	Tpf& Yapf()
	{
		return *static_cast<Tpf*>(this);
	}

public:
	/** Called by YAPF to detect if node ends in the desired destination */
	inline bool PfDetectDestination(Node& n)
	{
		return PfDetectDestinationTile(n.m_segment_last);
	}

	inline bool PfDetectDestinationTile(const PathPos &pos)
	{
		if (m_dest_station != INVALID_STATION) {
			return IsStationTile(pos.tile) &&
				GetStationIndex(pos.tile) == m_dest_station &&
				(m_bus ? IsBusStop(pos.tile) : IsTruckStop(pos.tile)) &&
				(m_non_artic || IsDriveThroughStopTile(pos.tile));
		}

		return pos.tile == m_destTile;
	}

	/**
	 * Called by YAPF to calculate cost estimate. Calculates distance to the destination
	 *  adds it to the actual cost from origin and stores the sum to the Node::m_estimate
	 */
	inline bool PfCalcEstimate(Node& n)
	{
		static const int dg_dir_to_x_offs[] = {-1, 0, 1, 0};
		static const int dg_dir_to_y_offs[] = {0, 1, 0, -1};
		if (PfDetectDestination(n)) {
			n.m_estimate = n.m_cost;
			return true;
		}

		TileIndex tile = n.m_segment_last.tile;
		DiagDirection exitdir = TrackdirToExitdir(n.m_segment_last.td);
		int x1 = 2 * TileX(tile) + dg_dir_to_x_offs[(int)exitdir];
		int y1 = 2 * TileY(tile) + dg_dir_to_y_offs[(int)exitdir];
		int x2 = 2 * TileX(m_destTile);
		int y2 = 2 * TileY(m_destTile);
		int dx = abs(x1 - x2);
		int dy = abs(y1 - y2);
		int dmin = min(dx, dy);
		int dxy = abs(dx - dy);
		int d = dmin * YAPF_TILE_CORNER_LENGTH + (dxy - 1) * (YAPF_TILE_LENGTH / 2);
		n.m_estimate = n.m_cost + d;
		assert(n.m_estimate >= n.m_parent->m_estimate);
		return true;
	}
};



template <class Types>
class CYapfFollowRoadT
{
public:
	typedef typename Types::Tpf Tpf;                     ///< the pathfinder class (derived from THIS class)
	typedef typename Types::TrackFollower TrackFollower;
	typedef typename Types::Astar::Node Node;            ///< this will be our node type
	typedef typename Node::Key Key;                      ///< key to hash tables

protected:
	/** to access inherited path finder */
	inline Tpf& Yapf()
	{
		return *static_cast<Tpf*>(this);
	}

public:

	/**
	 * Called by YAPF to move from the given node to the next tile. For each
	 *  reachable trackdir on the new tile creates new node, initializes it
	 *  and adds it to the open list by calling Yapf().AddNewNode(n)
	 */
	inline void PfFollowNode(Node& old_node)
	{
		TrackFollower F(Yapf().GetVehicle());
		if (F.Follow(old_node.m_segment_last)) {
			Yapf().AddMultipleNodes(&old_node, F);
		}
	}

	/** return debug report character to identify the transportation type */
	inline char TransportTypeChar() const
	{
		return 'r';
	}

	static TileIndex stFindNearestDepot(const RoadVehicle *v, const PathPos &pos, int max_distance)
	{
		Tpf pf;
		return pf.FindNearestDepot(v, pos, max_distance);
	}

	inline TileIndex FindNearestDepot(const RoadVehicle *v, const PathPos &pos, int max_distance)
	{
		/* set origin and destination nodes */
		Yapf().SetOrigin(pos);

		/* find the best path */
		if (!Yapf().FindPath(v)) return INVALID_TILE;

		/* some path found
		 * get found depot tile */
		Node *n = Yapf().GetBestNode();

		if (max_distance > 0 && n->m_cost > max_distance * YAPF_TILE_LENGTH) return INVALID_TILE;

		return n->m_segment_last.tile;
	}
};

template <class Tpf_, class TAstar>
struct CYapfRoad_TypesT
{
	typedef CYapfRoad_TypesT<Tpf_, TAstar> Types;

	typedef Tpf_                              Tpf;
	typedef CFollowTrackRoad                  TrackFollower;
	typedef TAstar                            Astar;
	typedef RoadVehicle                       VehicleType;
};

struct CYapfRoad1
	: CYapfBaseT<CYapfRoad_TypesT<CYapfRoad1, AstarRoadTrackDir> >
	, CYapfCostRoadT<CYapfRoad_TypesT<CYapfRoad1, AstarRoadTrackDir> >
	, CYapfSegmentCostCacheNoneT<CYapfRoad_TypesT<CYapfRoad1, AstarRoadTrackDir> >
	, CYapfOriginTileT<CYapfRoad1>
	, CYapfDestinationTileRoadT<CYapfRoad_TypesT<CYapfRoad1, AstarRoadTrackDir> >
	, CYapfFollowRoadT<CYapfRoad_TypesT<CYapfRoad1, AstarRoadTrackDir> >
{
};

struct CYapfRoad2
	: CYapfBaseT<CYapfRoad_TypesT<CYapfRoad2, AstarRoadExitDir> >
	, CYapfCostRoadT<CYapfRoad_TypesT<CYapfRoad2, AstarRoadExitDir> >
	, CYapfSegmentCostCacheNoneT<CYapfRoad_TypesT<CYapfRoad2, AstarRoadExitDir> >
	, CYapfOriginTileT<CYapfRoad2>
	, CYapfDestinationTileRoadT<CYapfRoad_TypesT<CYapfRoad2, AstarRoadExitDir> >
	, CYapfFollowRoadT<CYapfRoad_TypesT<CYapfRoad2, AstarRoadExitDir> >
{
};

struct CYapfRoadAnyDepot1
	: CYapfBaseT<CYapfRoad_TypesT<CYapfRoadAnyDepot1, AstarRoadTrackDir> >
	, CYapfCostRoadT<CYapfRoad_TypesT<CYapfRoadAnyDepot1, AstarRoadTrackDir> >
	, CYapfSegmentCostCacheNoneT<CYapfRoad_TypesT<CYapfRoadAnyDepot1, AstarRoadTrackDir> >
	, CYapfOriginTileT<CYapfRoadAnyDepot1>
	, CYapfDestinationAnyDepotRoadT<CYapfRoad_TypesT<CYapfRoadAnyDepot1, AstarRoadTrackDir> >
	, CYapfFollowRoadT<CYapfRoad_TypesT<CYapfRoadAnyDepot1, AstarRoadTrackDir> >
{
};

struct CYapfRoadAnyDepot2
	: CYapfBaseT<CYapfRoad_TypesT<CYapfRoadAnyDepot2, AstarRoadExitDir> >
	, CYapfCostRoadT<CYapfRoad_TypesT<CYapfRoadAnyDepot2, AstarRoadExitDir> >
	, CYapfSegmentCostCacheNoneT<CYapfRoad_TypesT<CYapfRoadAnyDepot2, AstarRoadExitDir> >
	, CYapfOriginTileT<CYapfRoadAnyDepot2>
	, CYapfDestinationAnyDepotRoadT<CYapfRoad_TypesT<CYapfRoadAnyDepot2, AstarRoadExitDir> >
	, CYapfFollowRoadT<CYapfRoad_TypesT<CYapfRoadAnyDepot2, AstarRoadExitDir> >
{
};


template <class Tpf>
static Trackdir ChooseRoadTrack(const RoadVehicle *v, TileIndex tile, DiagDirection enterdir, bool &path_found)
{
	/* Handle special case - when next tile is destination tile.
	 * However, when going to a station the (initial) destination
	 * tile might not be a station, but a junction, in which case
	 * this method forces the vehicle to jump in circles. */
	if (tile == v->dest_tile && !v->current_order.IsType(OT_GOTO_STATION)) {
		/* choose diagonal trackdir reachable from enterdir */
		return DiagDirToDiagTrackdir(enterdir);
	}

	Tpf pf;

	/* set origin and destination nodes */
	pf.SetOrigin(tile, TrackStatusToTrackdirBits(GetTileRoadStatus(tile, v->compatible_roadtypes)) & DiagdirReachesTrackdirs(enterdir));
	pf.SetDestination(v);

	/* find the best path */
	path_found = pf.FindPath(v);

	/* if path not found - return INVALID_TRACKDIR */
	typename Tpf::Astar::Node *n = pf.GetBestNode();
	if (n == NULL) return INVALID_TRACKDIR;

	/* path was found or at least suggested
	 * walk through the path back to its origin */
	while (n->m_parent != NULL) {
		n = n->m_parent;
	}

	/* return trackdir from the best origin node (one of start nodes) */
	assert(n->GetPos().tile == tile);
	return n->GetPos().td;
}

Trackdir YapfRoadVehicleChooseTrack(const RoadVehicle *v, TileIndex tile, DiagDirection enterdir, TrackdirBits trackdirs, bool &path_found)
{
	/* default is YAPF type 2 */
	typedef Trackdir (*PfnChooseRoadTrack)(const RoadVehicle*, TileIndex, DiagDirection, bool &path_found);
	PfnChooseRoadTrack pfnChooseRoadTrack = &ChooseRoadTrack<CYapfRoad2>; // default: ExitDir, allow 90-deg

	/* check if non-default YAPF type should be used */
	if (_settings_game.pf.yapf.disable_node_optimization) {
		pfnChooseRoadTrack = &ChooseRoadTrack<CYapfRoad1>; // Trackdir, allow 90-deg
	}

	Trackdir td_ret = pfnChooseRoadTrack(v, tile, enterdir, path_found);
	return (td_ret != INVALID_TRACKDIR) ? td_ret : (Trackdir)FindFirstBit2x64(trackdirs);
}

TileIndex YapfRoadVehicleFindNearestDepot(const RoadVehicle *v, uint max_distance)
{
	PathPos pos = v->GetPos();
	if ((TrackStatusToTrackdirBits(GetTileRoadStatus(pos.tile, v->compatible_roadtypes)) & TrackdirToTrackdirBits(pos.td)) == 0) {
		return false;
	}

	/* default is YAPF type 2 */
	typedef TileIndex (*PfnFindNearestDepot)(const RoadVehicle*, const PathPos&, int);
	PfnFindNearestDepot pfnFindNearestDepot = &CYapfRoadAnyDepot2::stFindNearestDepot;

	/* check if non-default YAPF type should be used */
	if (_settings_game.pf.yapf.disable_node_optimization) {
		pfnFindNearestDepot = &CYapfRoadAnyDepot1::stFindNearestDepot; // Trackdir, allow 90-deg
	}

	return pfnFindNearestDepot(v, pos, max_distance);
}
