// Round-trip test for NeighborSnapshot. Game-free, Windows-free.
//   make test   (or: g++ -std=c++20 -I src tests/snapshot_roundtrip.cpp src/NeighborSnapshot.cpp -o snap && ./snap <scratch dir>)
#include "NeighborSnapshot.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>

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
	a.dealsKnown = true;
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
	CHECK(b.connectionsKnown && b.connections.size() == a.connections.size());
	for (size_t i = 0; i < a.connections.size() && i < b.connections.size(); i++)
	{
		const ConnectionRecord& x = a.connections[i];
		const ConnectionRecord& y = b.connections[i];
		CHECK(x.type == y.type && x.innerX == y.innerX && x.innerZ == y.innerZ && x.otherTileX == y.otherTileX && x.otherTileZ == y.otherTileZ);
		CHECK(x.cellKnown == y.cellKnown && (!x.cellKnown || (x.cellX == y.cellX && x.cellZ == y.cellZ)));
	}
	CHECK(b.dealsKnown && b.deals.size() == a.deals.size());
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
		f << "regionX=4294967296\npopulation=99999999999\ncitySerial=-1\nconnectionCount=x\ndealCount=999999\n";
	}
	NeighborSnapshot o;
	CHECK(ReadSnapshotFile(path, o, error));
	CHECK(o.regionX == 0 && o.population == 0 && o.citySerial == 0);
	CHECK(o.connectionsKnown && o.connections.empty());
	CHECK(o.dealsKnown && o.deals.size() == kMaxListEntries);
	CHECK(error.find("connectionCount") != std::string::npos && error.find("dealCount") != std::string::npos);

	// Validation helpers.
	// Connection cell: west, east, north, south of a 64-cell tile accepted.
	CHECK(IsConnectionCellAdjacent(0, 55, -1, 55, 64) && IsConnectionCellAdjacent(63, 10, 64, 10, 64));
	CHECK(IsConnectionCellAdjacent(5, 0, 5, -1, 64) && IsConnectionCellAdjacent(5, 63, 5, 64, 64) && IsConnectionCellAdjacent(127, 3, 128, 3, 128));
	// Interior pair, same cell, off-axis, two steps, 256 on a small tile, other axis outside.
	CHECK(!IsConnectionCellAdjacent(10, 10, 11, 10, 64) && !IsConnectionCellAdjacent(0, 55, 0, 55, 64) && !IsConnectionCellAdjacent(0, 55, -1, 54, 64));
	CHECK(!IsConnectionCellAdjacent(0, 55, 2, 55, 64) && !IsConnectionCellAdjacent(255, 0, 256, 0, 64) && !IsConnectionCellAdjacent(0, 64, -1, 64, 64));
	CHECK(!IsConnectionCellAdjacent(0, 5, -1, 5, 0));
	CHECK(IsMirrorableConnectionType(1) && IsMirrorableConnectionType(11) && IsMirrorableConnectionType(10));
	CHECK(!IsMirrorableConnectionType(0) && !IsMirrorableConnectionType(3) && !IsMirrorableConnectionType(8) && !IsMirrorableConnectionType(9) && !IsMirrorableConnectionType(12));
	CHECK(IsDealType(0x4773FFE0) && IsDealType(0x4773FFE5) && !IsDealType(0x4773FFE6) && !IsDealType(0x4773FFDF));
	CHECK(OtherSideDealType(0x4773FFE4) == 0x4773FFE5 && OtherSideDealType(0x4773FFE1) == 0x4773FFE0);
	CHECK(DealCommodity(0x4773FFE0) == 0 && DealCommodity(0x4773FFE3) == 1 && DealCommodity(0x4773FFE5) == 2);
	CHECK(IsValidDealAmount(0) && IsValidDealAmount(10000000) && !IsValidDealAmount(-1) && !IsValidDealAmount(10000001));
	CHECK(IsValidExpiresIn(0) && IsValidExpiresIn(36500) && !IsValidExpiresIn(-1) && !IsValidExpiresIn(36501));
	CHECK(MaskDealFlags(0xFFFFFFFF) == 0x37F && MaskDealFlags(0x81) == 0x1);

	// Missing file reports an error.
	NeighborSnapshot c;
	CHECK(!ReadSnapshotFile(dir / "missing.snapshot", c, error));
	CHECK(!error.empty());

	std::filesystem::remove_all(dir);
	std::printf(failures ? "FAILED (%d)\n" : "OK\n", failures);
	return failures ? 1 : 0;
}
