// Round-trip test for NeighborSnapshot and InstanceStatus. Game-free, Windows-free.
//   make test   (or: g++ -std=c++20 -I src tests/snapshot_roundtrip.cpp src/NeighborSnapshot.cpp src/InstanceStatus.cpp -o snap && ./snap <scratch dir>)
#include "NeighborSnapshot.h"
#include "InstanceStatus.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace regionsync;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(int argc, char** argv)
{
	NeighborSnapshot a;
	a.regionX = 3; a.regionZ = 7; a.sizeX = 2; a.sizeZ = 4;
	a.citySerial = 0xDEADBEEF; a.cityName = "New Worth = Town"; a.established = true;
	a.writtenUnixTime = 1760000000123LL; a.simDateNumber = -42;
	a.simYear = 2031; a.simMonth = 11; a.simDay = 30;
	a.writer = { "GAMEBOX-01", 0xFFFFFFF0u, 134000000000000000ULL };
	a.cityLoadedAt = 134000000001234567ULL;
	a.population = 123456; a.commercialJobs = 2345; a.industrialJobs = 678;
	a.workforcePercentage = 0.625f; a.mayorRating = -17; a.budget = -12345.5f; a.income = 987.25f;
	for (size_t i = 0; i < kCommoditySlots; i++)
	{
		a.commodities[i] = { 1.5f * i, 2.5f * i, 3.5f * i, 4.5f * i, 0.125f * i };
	}
	a.rciGroupCount = 3;
	a.rciGroups[0] = { 0x1010, 100 }; a.rciGroups[1] = { 0x3110, 200 }; a.rciGroups[2] = { 0x4400, -1 };
	a.connectionsKnown = true;
	a.connections = { { 2, 0, 17, -1, 17, true, 2, 7 }, { 4, 127, 0, 127, -1, true, 3, 6 }, { 5, 3, 0, 0, 0, false, 3, 6 } };
	a.connectionsComplete = true;
	a.dealsKnown = true;
	a.dealsComplete = false;
	a.deals = { { 0x4773FFE4, 2, 7, 1500, 0x2, 365 }, { 0x4773FFE1, 4, 7, 0, 0x101, 0 } };

	// Scratch dir under the build tree, not the shared system temp dir.
	const std::filesystem::path dir = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("build/snapshot-test");
	const std::filesystem::path path = dir / SnapshotFileName(a.regionX, a.regionZ);
	std::string error;
	CHECK(SnapshotFileName(3, 7) == "3_7.snapshot");
	CHECK(WriteSnapshotFile(path, a, error));
	CHECK(error.empty());
	CHECK(!std::filesystem::exists(path.string() + ".tmp"));

	NeighborSnapshot b;
	CHECK(ReadSnapshotFile(path, b, error));
	CHECK(b.regionX == a.regionX && b.regionZ == a.regionZ && b.sizeX == a.sizeX && b.sizeZ == a.sizeZ);
	CHECK(b.citySerial == a.citySerial);
	CHECK(b.cityName == a.cityName);
	CHECK(b.established == a.established);
	CHECK(b.writtenUnixTime == a.writtenUnixTime);
	CHECK(b.simDateNumber == a.simDateNumber);
	CHECK(b.simYear == a.simYear && b.simMonth == a.simMonth && b.simDay == a.simDay);
	CHECK(b.population == a.population && b.commercialJobs == a.commercialJobs && b.industrialJobs == a.industrialJobs);
	CHECK(std::fabs(b.workforcePercentage - a.workforcePercentage) < 1e-5f);
	CHECK(b.mayorRating == a.mayorRating);
	CHECK(std::fabs(b.budget - a.budget) < 1e-2f);
	CHECK(std::fabs(b.income - a.income) < 1e-3f);
	for (size_t i = 0; i < kCommoditySlots; i++)
	{
		CHECK(std::fabs(b.commodities[i].exported - a.commodities[i].exported) < 1e-3f);
		CHECK(std::fabs(b.commodities[i].imported - a.commodities[i].imported) < 1e-3f);
		CHECK(std::fabs(b.commodities[i].produced - a.commodities[i].produced) < 1e-3f);
		CHECK(std::fabs(b.commodities[i].demanded - a.commodities[i].demanded) < 1e-3f);
		CHECK(std::fabs(b.commodities[i].costPerUnit - a.commodities[i].costPerUnit) < 1e-4f);
	}
	CHECK(b.rciGroupCount == a.rciGroupCount);
	for (size_t i = 0; i < a.rciGroupCount; i++)
	{
		CHECK(b.rciGroups[i].groupId == a.rciGroups[i].groupId);
		CHECK(b.rciGroups[i].population == a.rciGroups[i].population);
	}

	CHECK(b.schemaVersion == 3);
	CHECK(b.writer.host == a.writer.host && b.writer.pid == a.writer.pid && b.writer.start == a.writer.start);
	CHECK(b.cityLoadedAt == a.cityLoadedAt);
	CHECK(b.connectionsKnown && b.connections.size() == a.connections.size());
	for (size_t i = 0; i < a.connections.size() && i < b.connections.size(); i++)
	{
		const ConnectionRecord& x = a.connections[i];
		const ConnectionRecord& y = b.connections[i];
		CHECK(x.type == y.type && x.innerX == y.innerX && x.innerZ == y.innerZ && x.otherTileX == y.otherTileX && x.otherTileZ == y.otherTileZ);
		CHECK(x.cellKnown == y.cellKnown && (!x.cellKnown || (x.cellX == y.cellX && x.cellZ == y.cellZ)));
	}
	CHECK(b.dealsKnown && b.deals.size() == a.deals.size());
	CHECK(b.connectionsComplete && !b.dealsComplete);
	for (size_t i = 0; i < a.deals.size() && i < b.deals.size(); i++)
	{
		const DealRecord& x = a.deals[i];
		const DealRecord& y = b.deals[i];
		CHECK(x.type == y.type && x.otherTileX == y.otherTileX && x.otherTileZ == y.otherTileZ);
		CHECK(x.amount == y.amount && x.flags == y.flags && x.expiresIn == y.expiresIn);
	}

	// Known and empty round-trips as known; unknown stays unknown.
	NeighborSnapshot empty;
	empty.connectionsKnown = true;
	CHECK(WriteSnapshotFile(path, empty, error));
	NeighborSnapshot e2;
	e2.dealsKnown = true; e2.deals.resize(3);  // stale contents must be cleared
	CHECK(ReadSnapshotFile(path, e2, error));
	CHECK(e2.connectionsKnown && e2.connections.empty());
	CHECK(!e2.dealsKnown && e2.deals.empty());
	CHECK(!e2.connectionsComplete && !e2.dealsComplete);   // absent reads false
	CHECK(!e2.writer.Known());

	// Writer identity: missing or bad values read as unknown, and an invalid
	// host is not written.
	{
		std::ofstream f(path, std::ios::trunc);
		f << "schemaVersion=2\nregionX=1\n";
	}
	NeighborSnapshot w;
	w.writer = { "STALE", 1, 1 };  // must be cleared
	CHECK(ReadSnapshotFile(path, w, error) && w.schemaVersion == 2 && !w.writer.Known() && w.writer.host.empty());
	const char* badWriters[] = {
		"writerHost=H\nwriterPid=1\n",                                   // start missing
		"writerPid=1\nwriterStart=5\n",                                  // host missing
		"writerHost=\nwriterPid=1\nwriterStart=5\n",                     // host empty
		"writerHost=H\nwriterPid=-1\nwriterStart=5\n",                   // pid negative
		"writerHost=H\nwriterPid=4294967296\nwriterStart=5\n",           // pid over u32
		"writerHost=H\nwriterPid=0\nwriterStart=5\n",                    // pid zero
		"writerHost=H\nwriterPid=1\nwriterStart=-5\n",                   // start negative
		"writerHost=H\nwriterPid=1\nwriterStart=18446744073709551616\n", // start over u64
		"writerHost=H\nwriterPid=1\nwriterStart=x\n",                    // start not a number
		"writerHost=H\twith tab\nwriterPid=1\nwriterStart=5\n",         // control character
	};
	for (const char* text : badWriters)
	{
		{
			std::ofstream f(path, std::ios::trunc);
			f << text;
		}
		NeighborSnapshot bw;
		CHECK(ReadSnapshotFile(path, bw, error) && !bw.writer.Known());
	}
	{
		std::ofstream f(path, std::ios::trunc);
		f << "writerHost=H\nwriterPid=4294967295\nwriterStart=18446744073709551615\n";
	}
	NeighborSnapshot mx;
	CHECK(ReadSnapshotFile(path, mx, error) && mx.writer.Known() && mx.writer.pid == 0xFFFFFFFFu && mx.writer.start == 0xFFFFFFFFFFFFFFFFULL);
	CHECK(!IsValidWriterHost(std::string(kMaxWriterHostLength + 1, 'A')) && IsValidWriterHost(std::string(kMaxWriterHostLength, 'A')));
	CHECK(!IsValidWriterHost("a\nb") && !IsValidWriterHost("a\rb") && IsValidWriterHost("PC-\xC3\xA9"));
	NeighborSnapshot badHost = a;
	badHost.writer.host = "evil\ncityName=x";
	CHECK(WriteSnapshotFile(path, badHost, error));
	NeighborSnapshot bh;
	CHECK(ReadSnapshotFile(path, bh, error) && !bh.writer.Known() && bh.cityName == a.cityName);

	// Frame conversion. A at region (2,0), B at (0,0), both medium (128 cells).
	int32_t bx = 0, bz = 0;
	CHECK(CityCells(0) == 64 && CityCells(1) == 128 && CityCells(2) == 256);
	CHECK(ToSubscriberFrame(2, 0, 0, 17, 0, 0, 128, bx, bz) && bx == 128 && bz == 17);       // x = cells
	CHECK(ToSubscriberFrame(0, 0, 127, 5, 2, 0, 128, bx, bz) && bx == -1 && bz == 5);        // x = -1
	CHECK(ToSubscriberFrame(0, 2, 40, 0, 0, 0, 128, bx, bz) && bx == 40 && bz == 128);       // z = cells
	CHECK(ToSubscriberFrame(0, 0, 0, 127, 0, 2, 128, bx, bz) && bx == 0 && bz == -1);        // z = -1
	CHECK(ToSubscriberFrame(2, 0, 0, 127, 0, 0, 128, bx, bz) && bx == 128 && bz == 127);     // last cell on the edge
	CHECK(!ToSubscriberFrame(2, 2, 0, 0, 0, 0, 128, bx, bz) && bx == 128 && bz == 128);      // corner
	CHECK(!ToSubscriberFrame(0, 0, 0, 0, 2, 2, 128, bx, bz) && bx == -128 && bz == -128);    // far corner
	CHECK(!ToSubscriberFrame(1, 0, 10, 10, 0, 0, 128, bx, bz) && bx == 74);                  // interior
	CHECK(!ToSubscriberFrame(4, 0, 0, 10, 0, 0, 128, bx, bz));                                // not adjacent
	CHECK(!ToSubscriberFrame(2, 0, 0, 128, 0, 0, 128, bx, bz));                               // other axis out of range
	CHECK(ToSubscriberFrame(1, 0, 0, 63, 0, 0, 64, bx, bz) && bx == 64 && bz == 63);          // small B
	CHECK(!ToSubscriberFrame(0x7FFFFFFF, 0, 0, 10, 0, 0, 128, bx, bz) && bx == 0 && bz == 0); // region out of range
	CHECK(!ToSubscriberFrame(2, 0, 257, 10, 0, 0, 128, bx, bz));                              // inner out of range
	CHECK(!ToSubscriberFrame(2, 0, 0, 10, -1, 0, 128, bx, bz));                               // own region negative

	// Overflowing numbers fail the parse; bad or oversized counts are reported.
	{
		std::ofstream f(path, std::ios::trunc);
		f << "regionX=4294967296\npopulation=99999999999\ncitySerial=-1\nconnectionsComplete=1\nconnectionCount=x\ndealsComplete=1\ndealCount=999999\n";
	}
	NeighborSnapshot o;
	CHECK(ReadSnapshotFile(path, o, error));
	CHECK(o.regionX == 0 && o.population == 0 && o.citySerial == 0);
	CHECK(o.connectionsKnown && o.connections.empty());
	CHECK(o.dealsKnown && o.deals.size() == kMaxListEntries);
	CHECK(error.find("connectionCount") != std::string::npos && error.find("dealCount") != std::string::npos);
	CHECK(!o.connectionsComplete && !o.dealsComplete);   // any parse warning clears both

	// A missing per-entry key also clears both; a clean file keeps them.
	{
		std::ofstream f(path, std::ios::trunc);
		f << "connectionsComplete=1\nconnectionCount=1\nconnection.0.type=2\nconnection.0.innerX=0\nconnection.0.innerZ=5\n"
			"connection.0.otherTileX=1\nconnection.0.otherTileZ=0\ndealsComplete=1\ndealCount=1\ndeal.0.type=1198784480\n";
	}
	NeighborSnapshot m;
	CHECK(ReadSnapshotFile(path, m, error));
	CHECK(error.find("deal.0.") != std::string::npos && !m.connectionsComplete && !m.dealsComplete);
	{
		std::ofstream f(path, std::ios::trunc);
		f << "connectionsComplete=1\nconnectionCount=1\nconnection.0.type=2\nconnection.0.innerX=0\nconnection.0.innerZ=5\n"
			"connection.0.otherTileX=1\nconnection.0.otherTileZ=0\ndealsComplete=1\ndealCount=0\n";
	}
	CHECK(ReadSnapshotFile(path, m, error) && error.empty() && m.connectionsComplete && m.dealsComplete);

	// Validation helpers.
	// Connection cell: west, east, north, south of a 64-cell tile accepted.
	CHECK(IsConnectionCellAdjacent(0, 55, -1, 55, 64) && IsConnectionCellAdjacent(63, 10, 64, 10, 64));
	CHECK(IsConnectionCellAdjacent(5, 0, 5, -1, 64) && IsConnectionCellAdjacent(5, 63, 5, 64, 64) && IsConnectionCellAdjacent(127, 3, 128, 3, 128));
	// Interior pair, same cell, off-axis, two steps, 256 on a small tile, other axis outside.
	CHECK(!IsConnectionCellAdjacent(10, 10, 11, 10, 64) && !IsConnectionCellAdjacent(0, 55, 0, 55, 64) && !IsConnectionCellAdjacent(0, 55, -1, 54, 64));
	CHECK(!IsConnectionCellAdjacent(0, 55, 2, 55, 64) && !IsConnectionCellAdjacent(255, 0, 256, 0, 64) && !IsConnectionCellAdjacent(0, 64, -1, 64, 64));
	CHECK(!IsConnectionCellAdjacent(0, 5, -1, 5, 0));
	CHECK(IsMirrorableConnectionType(1) && IsMirrorableConnectionType(11) && IsMirrorableConnectionType(10));
	CHECK(IsMirrorableConnectionType(3) && IsMirrorableConnectionType(8) && IsMirrorableConnectionType(12));
	CHECK(!IsMirrorableConnectionType(0) && !IsMirrorableConnectionType(9) && !IsMirrorableConnectionType(13));
	CHECK(IsDualConnectionType(3) && IsDualConnectionType(8) && IsDualConnectionType(12));
	CHECK(!IsDualConnectionType(2) && !IsDualConnectionType(9) && !IsDualConnectionType(0));
	CHECK(IsDealType(0x4773FFE0) && IsDealType(0x4773FFE5) && !IsDealType(0x4773FFE6) && !IsDealType(0x4773FFDF));
	CHECK(OtherSideDealType(0x4773FFE4) == 0x4773FFE5 && OtherSideDealType(0x4773FFE1) == 0x4773FFE0);
	CHECK(DealCommodity(0x4773FFE0) == 0 && DealCommodity(0x4773FFE3) == 1 && DealCommodity(0x4773FFE5) == 2);
	CHECK(IsValidDealAmount(0) && IsValidDealAmount(10000000) && !IsValidDealAmount(-1) && !IsValidDealAmount(10000001));
	CHECK(IsValidExpiresIn(0) && IsValidExpiresIn(36500) && !IsValidExpiresIn(-1) && !IsValidExpiresIn(36501));
	CHECK(MaskDealFlags(0xFFFFFFFF) == 0x37F && MaskDealFlags(0x81) == 0x1);

	// Edge network: type map and stub geometry.
	CHECK(ConnectionToNetworkType(1) == 1 && ConnectionToNetworkType(2) == 0 && ConnectionToNetworkType(4) == 5 && ConnectionToNetworkType(5) == 4);
	CHECK(ConnectionToNetworkType(6) == 7 && ConnectionToNetworkType(7) == 8 && ConnectionToNetworkType(10) == 11 && ConnectionToNetworkType(11) == 9);
	CHECK(ConnectionToNetworkType(3) == 2 && ConnectionToNetworkType(8) == 6 && ConnectionToNetworkType(12) == 12);
	CHECK(ConnectionToNetworkType(0) == -1 && ConnectionToNetworkType(9) == -1 && ConnectionToNetworkType(0xFFFFFFFF) == -1);
	const auto stubIs = [](std::optional<EdgeStub> s, int32_t sx, int32_t sz, int32_t ex, int32_t ez, uint32_t side) {
		return s && s->sx == sx && s->sz == sz && s->ex == ex && s->ez == ez && s->side == side;
	};
	CHECK(stubIs(EdgeStubGeometry(-1, 17, 128), 0, 17, 2, 17, 0));       // west
	CHECK(stubIs(EdgeStubGeometry(40, -1, 128), 40, 0, 40, 2, 1));       // north
	CHECK(stubIs(EdgeStubGeometry(128, 5, 128), 127, 5, 125, 5, 2));    // east
	CHECK(stubIs(EdgeStubGeometry(0, 64, 64), 0, 63, 0, 61, 3));         // south, small tile
	CHECK(stubIs(EdgeStubGeometry(-1, 255, 256), 0, 255, 2, 255, 0));    // last cell on the edge
	CHECK(!EdgeStubGeometry(-1, -1, 128) && !EdgeStubGeometry(128, 128, 128) && !EdgeStubGeometry(-1, 128, 128)); // corners
	CHECK(!EdgeStubGeometry(5, 5, 128) && !EdgeStubGeometry(-2, 5, 128) && !EdgeStubGeometry(129, 5, 128));       // interior, too far
	CHECK(!EdgeStubGeometry(-1, 5, 0) && !EdgeStubGeometry(-1, 0, 2));                                            // degenerate size

	// Dual lanes. Publisher 128 cells; W/E lanes vary z, N/S lanes vary x.
	{
		using Pairs = std::vector<std::pair<size_t, size_t>>;
		const auto W = [](uint32_t t, int32_t z) { return ConnectionRecord{ t, 0, z, -1, z, true, 0, 0 }; };
		const auto E = [](uint32_t t, int32_t z) { return ConnectionRecord{ t, 127, z, 128, z, true, 0, 0 }; };
		const auto N = [](uint32_t t, int32_t x) { return ConnectionRecord{ t, x, 0, x, -1, true, 0, 0 }; };
		const auto S = [](uint32_t t, int32_t x) { return ConnectionRecord{ t, x, 127, x, 128, true, 0, 0 }; };
		CHECK(PairDualLanes({ W(8, 11), W(8, 10) }, 128) == Pairs({ { 1, 0 } }));                        // hi listed first
		CHECK(PairDualLanes({ W(8, 5), W(8, 6), N(3, 5), N(3, 6), E(12, 40), E(12, 41), S(8, 99), S(8, 100) }, 128)
			== Pairs({ { 2, 3 }, { 0, 1 }, { 6, 7 }, { 4, 5 } }));                                          // all four edges
		CHECK(PairDualLanes({ W(8, 10), W(8, 11), W(8, 12), W(8, 13) }, 128) == Pairs({ { 0, 1 }, { 2, 3 } }));
		CHECK(PairDualLanes({ W(8, 10), W(8, 11), W(8, 12) }, 128) == Pairs({ { 0, 1 } }));             // third unpaired
		CHECK(PairDualLanes({ W(8, 10), W(8, 12) }, 128).empty());                                        // gap
		CHECK(PairDualLanes({ W(8, 10), W(3, 11) }, 128).empty());                                        // types differ
		CHECK(PairDualLanes({ W(8, 10), W(8, 10), W(8, 11) }, 128) == Pairs({ { 0, 2 } }));             // duplicate
		CHECK(PairDualLanes({ N(8, 0), W(8, 1) }, 128).empty());                                          // across the corner
		ConnectionRecord c0 = W(8, 0), c1 = W(8, 1), d0 = W(8, 5), d1 = W(8, 6);
		c0.cellKnown = c1.cellKnown = d0.cellKnown = d1.cellKnown = false;
		CHECK(PairDualLanes({ c0, c1, d0, d1 }, 128) == Pairs({ { 2, 3 } }));                           // corner cell, side unknown
		ConnectionRecord e0 = E(8, 20), e1 = E(8, 21), n0 = N(8, 30), n1 = N(8, 31), s0 = S(8, 40), s1 = S(8, 41);
		for (ConnectionRecord* r : { &e0, &e1, &n0, &n1, &s0, &s1 }) r->cellKnown = false;
		CHECK(PairDualLanes({ e1, e0, n0, n1, s1, s0 }, 128) == Pairs({ { 2, 3 }, { 1, 0 }, { 5, 4 } })); // side from the inner cell
		CHECK(PairDualLanes({ E(8, 99), S(8, 100) }, 128).empty());                                       // different edges, adjacent coordinates
		CHECK(PairDualLanes({ W(2, 10), W(2, 11), W(8, 12), W(2, 13) }, 128).empty());                   // singles ignored
		CHECK(PairDualLanes({ W(8, 10), W(2, 10), W(8, 11) }, 128) == Pairs({ { 0, 2 } }));
		ConnectionRecord s63 = E(8, 63), s64 = E(8, 64), s65 = E(8, 65);
		s63.otherTileZ = 0; s64.otherTileZ = 1; s65.otherTileZ = 1;
		CHECK(PairDualLanes({ s64, s65, s63 }, 128) == Pairs({ { 2, 0 } }));                             // straddler
		CHECK(PairDualLanes({ W(8, 10), W(8, 11) }, 0).empty());                                          // publisher size unknown
		CHECK(PairDualLanes({ ConnectionRecord{ 8, 0, 10, 1, 10, true, 0, 0 }, W(8, 11) }, 128).empty()); // bad cell

		// Research table: drive right, lo is 1 on N/E and 0 on W/S, hi the inverse; drive left inverts both.
		for (uint32_t side = 0; side < 4; side++)
		{
			const bool ne = side == 1 || side == 2;
			CHECK(DualLaneDir(side, false, true) == ne && DualLaneDir(side, true, true) == !ne);
			CHECK(DualLaneDir(side, false, false) == !ne && DualLaneDir(side, true, false) == ne);
		}
		CHECK(!DualLaneDir(0, false, true) && DualLaneDir(1, false, true) && DualLaneDir(2, false, true) && !DualLaneDir(3, false, true));
	}

	// Removal helpers.
	CHECK(IsProposedOnlyDeal(0x1) && IsProposedOnlyDeal(0x81) && IsProposedOnlyDeal(0x101));
	CHECK(!IsProposedOnlyDeal(0x0) && !IsProposedOnlyDeal(0x2) && !IsProposedOnlyDeal(0x3) && !IsProposedOnlyDeal(0x4));
	int32_t pcx = 0, pcz = 0;
	CHECK(PublisherConnectionCell(0, 17, 128, 17, 128, pcx, pcz) && pcx == -1 && pcz == 17);     // publisher east of us
	CHECK(PublisherConnectionCell(127, 5, -1, 5, 128, pcx, pcz) && pcx == 128 && pcz == 5);      // publisher west
	CHECK(PublisherConnectionCell(40, 0, 40, 128, 128, pcx, pcz) && pcx == 40 && pcz == -1);     // publisher south
	CHECK(PublisherConnectionCell(0, 127, 0, -1, 128, pcx, pcz) && pcx == 0 && pcz == 128);      // publisher north
	CHECK(IsConnectionCellAdjacent(0, 17, -1, 17, 128) && IsConnectionCellAdjacent(127, 5, 128, 5, 128));
	CHECK(!PublisherConnectionCell(0, 0, -1, -1, 128, pcx, pcz) && !PublisherConnectionCell(0, 0, 5, 5, 128, pcx, pcz)); // corner, interior

	// Missing file reports an error.
	NeighborSnapshot c;
	CHECK(!ReadSnapshotFile(dir / "missing.snapshot", c, error));
	CHECK(!error.empty());

	// Instance status: every field, then the region state drops the city keys.
	{
		InstanceStatus st;
		st.writer = { "GAMEBOX", 29420, 134368212816162474ULL };
		st.userDir = "C:\\Users\\player\\Documents\\SimCity 4 Instances\\Instance1";
		st.state = "city";
		st.region = "New York";
		st.regionX = 4; st.regionZ = 6;
		st.cityName = "Bumper=Ville";
		st.conflict = true;
		st.writtenUnixTime = 1792345678;
		CHECK(InstanceStatusFileName(29420, 134368212816162474ULL) == "29420-134368212816162474.status");
		const std::filesystem::path sp = dir / InstanceStatusFileName(st.writer.pid, st.writer.start);
		CHECK(WriteInstanceStatus(sp, st, error));
		CHECK(!std::filesystem::exists(sp.string() + ".tmp"));
		InstanceStatus rt;
		CHECK(ReadInstanceStatus(sp, rt, error));
		CHECK(error.empty());
		CHECK(rt.schemaVersion == 1);
		CHECK(rt.writer.host == st.writer.host && rt.writer.pid == st.writer.pid && rt.writer.start == st.writer.start);
		CHECK(rt.userDir == st.userDir && rt.state == "city" && rt.region == "New York");
		CHECK(rt.regionX == 4 && rt.regionZ == 6 && rt.cityName == "Bumper=Ville" && rt.conflict);
		CHECK(rt.writtenUnixTime == st.writtenUnixTime);

		st.state = "region";
		st.userDir = "";
		CHECK(WriteInstanceStatus(sp, st, error));
		CHECK(ReadInstanceStatus(sp, rt, error));
		CHECK(rt.state == "region" && rt.region == "New York" && rt.userDir.empty());
		CHECK(rt.regionX == -1 && rt.regionZ == -1 && rt.cityName.empty() && !rt.conflict);

		st.writer = { "", 1, 1 };   // unknown writer: no identity keys; newlines flattened
		st.state = "city";
		st.cityName = "a\nb";
		CHECK(WriteInstanceStatus(sp, st, error));
		CHECK(ReadInstanceStatus(sp, rt, error) && !rt.writer.Known() && rt.cityName == "a b");

		// Reaper rule.
		InstanceStatus live;
		live.writer = { "PC1", 29420, 134368212816162474ULL };
		const std::string own = "29420-134368212816162474.status";
		CHECK(IsReapable(own, live, "PC1"));
		CHECK(IsReapable(own + ".tmp", live, "PC1"));
		CHECK(!IsReapable(own, live, "PC2"));                                  // foreign host
		CHECK(!IsReapable(own, live, ""));
		CHECK(!IsReapable("29420-134368212816162475.status", live, "PC1"));   // start mismatch
		CHECK(!IsReapable("29421-134368212816162474.status", live, "PC1"));   // pid mismatch
		CHECK(!IsReapable("junk.status", live, "PC1") && !IsReapable("29420-x.status", live, "PC1"));
		InstanceStatus truncated;
		truncated.writer = { "PC1", 29, 0 };                                  // half-written pid, no start
		CHECK(!IsReapable(own + ".tmp", truncated, "PC1"));
		{
			std::ofstream(dir / "half.status") << "host=PC1\npid=294";
			InstanceStatus half;
			CHECK(ReadInstanceStatus(dir / "half.status", half, error));
			CHECK(!IsReapable(own, half, "PC1"));
		}

		CHECK(!ReadInstanceStatus(dir / "missing.status", rt, error));
		CHECK(!error.empty());
	}

	// P1: key=value text form.
	{
		KeyValues kv;
		ParseKeyValues("; comment\r\n# also\r\n[Sync]\r\na=1\r\nb=x=y\r\n\r\nnoequals\r\nc=\r\nd=last", kv);
		CHECK(kv.size() == 4);
		CHECK(kv["a"] == "1" && kv["b"] == "x=y" && kv["c"].empty() && kv["d"] == "last");
		CHECK(kv.find("[Sync]") == kv.end() && kv.find("; comment") == kv.end());
	}

	// P1/P2: status text; saveCountFailed is optional and schema stays 1.
	{
		InstanceStatus st;
		ParseInstanceStatus("schemaVersion=1\r\nhost=PC1\r\npid=7\r\nstart=9\r\nstate=city\r\nregion=R\r\nregionX=1\r\nregionZ=2\r\nfutureKey=5\r\n", st);
		CHECK(st.schemaVersion == 1 && st.writer.host == "PC1" && st.writer.pid == 7 && st.writer.start == 9);
		CHECK(st.state == "city" && st.region == "R" && st.regionX == 1 && st.regionZ == 2 && st.saveCountFailedTiles.empty());
		ParseInstanceStatus("schemaVersion=1\nsaveCountFailed=6_6,-1_x,3_-2,junk,4_5_6\n", st);
		CHECK(!st.writer.Known());
		CHECK((st.saveCountFailedTiles == std::vector<std::pair<int32_t, int32_t>>{ { 6, 6 }, { 3, -2 } }));

		InstanceStatus w;
		w.writer = { "PC1", 7, 9 };
		w.state = "region";
		const std::filesystem::path sp = dir / "flag.status";
		CHECK(WriteInstanceStatus(sp, w, error));
		{
			std::ifstream in(sp);
			std::stringstream text;
			text << in.rdbuf();
			CHECK(text.str().find("saveCountFailed") == std::string::npos);
		}
		w.saveCountFailedTiles = { { 6, 6 }, { 0, 12 } };
		CHECK(WriteInstanceStatus(sp, w, error));
		{
			std::ifstream in(sp);
			std::stringstream text;
			text << in.rdbuf();
			CHECK(text.str().find("saveCountFailed=6_6,0_12\n") != std::string::npos);
		}
		InstanceStatus r;
		CHECK(ReadInstanceStatus(sp, r, error) && r.saveCountFailedTiles == w.saveCountFailedTiles && r.schemaVersion == 1 && r.state == "region");
	}

	// P4: identity.
	{
		CHECK(SameHost("GameBox", "GAMEBOX") && SameHost("pc-1", "PC-1") && SameHost("", ""));
		CHECK(!SameHost("PC1", "PC2") && !SameHost("PC1", "PC10") && !SameHost("\xC4", "\xE4"));
		const WriterIdentity a{ "GameBox", 10, 20 };
		CHECK(SameWriter(a, { "GAMEBOX", 10, 20 }));
		CHECK(!SameWriter(a, { "GAMEBOX", 11, 20 }) && !SameWriter(a, { "GAMEBOX", 10, 21 }) && !SameWriter(a, { "OTHER", 10, 20 }));
		CHECK(!SameWriter({ "", 10, 20 }, { "", 10, 20 }));
		CHECK(IsReapable("29420-134368212816162474.status", InstanceStatus{ 1, { "pc1", 29420, 134368212816162474ULL } }, "PC1"));
	}

	// P2: save counter text.
	{
		CHECK(SaveCounterFileName(6, -1) == "6_-1.saves");
		SaveCounter c;
		c.count = 42;
		c.file = "City - Tuggers Ville.sc4";
		c.prevFile = "City - Old.sc4";
		c.lastSaveUnix = 1792345678;
		c.simDateNumber = 731000;
		c.population = 12345;
		c.cityName = "Tuggers\nVille";
		c.writer = { "PC1", 7, 9 };
		SaveCounter r;
		ParseSaveCounter(FormatSaveCounter(c), r);
		CHECK(r.count == 42 && r.file == c.file && r.prevFile == c.prevFile && r.lastSaveUnix == c.lastSaveUnix);
		CHECK(r.simDateNumber == c.simDateNumber && r.population == c.population && r.cityName == "Tuggers Ville");
		CHECK(SameWriter(r.writer, c.writer) && r.lastOk == -1);
		CHECK(FormatSaveCounter(c).find("lastOk") == std::string::npos);
		c.lastOk = 0;
		c.prevFile = c.file;
		ParseSaveCounter(FormatSaveCounter(c), r);
		CHECK(r.lastOk == 0 && r.prevFile.empty());
		c.lastOk = 1;
		ParseSaveCounter(FormatSaveCounter(c), r);
		CHECK(r.lastOk == 1);
		ParseSaveCounter("count=x\r\nfile=a.sc4\r\n", r);
		CHECK(r.count == 0 && r.file == "a.sc4" && !r.writer.Known());

		const std::filesystem::path cp = dir / SaveCounterFileName(6, 6);
		r.count = 99;
		CHECK(ReadSaveCounter(cp, r, error) && r.count == 0 && error.empty());   // missing: count 0
		c.lastOk = 1;
		CHECK(WriteFileAtomic(cp, FormatSaveCounter(c), error));
		CHECK(ReadSaveCounter(cp, r, error) && r.count == 42 && r.lastOk == 1 && r.file == c.file);
		std::ofstream(cp) << "count=abc\nfile=a.sc4\n";
		CHECK(!ReadSaveCounter(cp, r, error) && !error.empty());                 // unparsable count
		std::ofstream(cp) << "file=a.sc4\n";
		CHECK(!ReadSaveCounter(cp, r, error));                                    // absent count
	}

	// P6: a failed write leaves the target and no .tmp.
	{
		const std::filesystem::path target = dir / "atomic.txt";
		CHECK(WriteFileAtomic(target, "good\n", error));
		std::error_code ec;
		if (std::filesystem::exists("/dev/full", ec))
		{
			const std::filesystem::path tmp = target.string() + ".tmp";
			std::filesystem::create_symlink("/dev/full", tmp, ec);
			CHECK(!ec);
			CHECK(!WriteFileAtomic(target, std::string(1 << 16, 'x'), error) && !error.empty());
			CHECK(!std::filesystem::exists(tmp) && !std::filesystem::is_symlink(tmp));
			std::filesystem::create_symlink("/dev/full", tmp, ec);   // small: fails only at flush or close
			CHECK(!ec);
			CHECK(!WriteFileAtomic(target, "abc", error) && !error.empty());
			CHECK(!std::filesystem::exists(tmp) && !std::filesystem::is_symlink(tmp));
			CHECK(std::filesystem::is_regular_file(std::filesystem::symlink_status(target)) && std::filesystem::file_size(target) == 5);
		}
		std::ofstream(dir / "notadir") << "x";
		CHECK(!WriteFileAtomic(dir / "notadir" / "f.txt", "y", error));
	}

	std::filesystem::remove_all(dir);
	std::printf(failures ? "FAILED (%d)\n" : "OK\n", failures);
	return failures ? 1 : 0;
}
