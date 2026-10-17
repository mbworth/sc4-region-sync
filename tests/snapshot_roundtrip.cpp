// Round-trip test for NeighborSnapshot. Game-free, Windows-free.
//   g++ -std=c++20 -I src tests/snapshot_roundtrip.cpp src/NeighborSnapshot.cpp -o /tmp/snap && /tmp/snap
#include "NeighborSnapshot.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace regionsync;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main()
{
	NeighborSnapshot a;
	a.regionX = 3; a.regionZ = 7; a.sizeX = 2; a.sizeZ = 4;
	a.citySerial = 0xDEADBEEF; a.cityName = "New Worth = Town"; a.established = true;
	a.writtenUnixTime = 1760000000123LL; a.simDateNumber = -42;
	a.simYear = 2031; a.simMonth = 11; a.simDay = 30;
	a.population = 123456; a.commercialJobs = 2345; a.industrialJobs = 678;
	a.workforcePercentage = 0.625f; a.mayorRating = -17; a.budget = -12345.5f; a.income = 987.25f;
	for (size_t i = 0; i < kCommoditySlots; i++)
	{
		a.commodities[i] = { 1.5f * i, 2.5f * i, 3.5f * i, 4.5f * i, 0.125f * i };
	}
	a.rciGroupCount = 3;
	a.rciGroups[0] = { 0x1010, 100 }; a.rciGroups[1] = { 0x3110, 200 }; a.rciGroups[2] = { 0x4400, -1 };

	const std::filesystem::path dir = std::filesystem::temp_directory_path() / "sc4rs-test";
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

	// Missing file reports an error.
	NeighborSnapshot c;
	CHECK(!ReadSnapshotFile(dir / "missing.snapshot", c, error));
	CHECK(!error.empty());

	std::filesystem::remove_all(dir);
	std::printf(failures ? "FAILED (%d)\n" : "OK\n", failures);
	return failures ? 1 : 0;
}
