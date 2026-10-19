// Launcher LAN core: frames, CPace, field formats, name rules, manifest diff, journal.
// Game-free, Windows-free.   make test-launcher
#include "Frame.h"
#include "Handshake.h"
#include "Manifest.h"
#include "Journal.h"
#include "InstanceStatus.h"
#include "monocypher.h"
#include <cstdio>

using namespace lan;

static int failures = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #__VA_ARGS__); failures++; } } while (0)

static Bytes32 Fill(uint8_t seed)
{
	Bytes32 b;
	for (size_t i = 0; i < b.size(); i++) b[i] = static_cast<uint8_t>(seed + i * 7);
	return b;
}

static std::string Hash(char c) { return std::string(64, c); }

static const DiffItem* Find(const DiffResult& r, std::string_view name)
{
	for (const auto& i : r.items) if (EqualsNoCase(i.name, name)) return &i;
	return nullptr;
}

static DiffAction ActionOf(const DiffResult& r, std::string_view name)
{
	const DiffItem* i = Find(r, name);
	return i ? i->action : static_cast<DiffAction>(-1);
}

static void TestFrames()
{
	const Bytes32 ks = Fill(1);
	FrameEncoder enc('G');
	enc.StartSession(ks);
	std::vector<uint8_t> f1, f2, f3;
	const std::vector<uint8_t> payload = { 1, 2, 3, 0, 255 };
	CHECK(enc.Encode(FrameType::Put, Fields().Add("kind", "snapshot").Add("name", "1_2.snapshot").Text(), payload, f1) == FrameStatus::Ok);
	CHECK(enc.Encode(FrameType::Ping, "", {}, f2) == FrameStatus::Ok);
	CHECK(enc.Encode(FrameType::Ping, "", {}, f3) == FrameStatus::Ok);

	// Round trip, fed one byte at a time.
	{
		FrameDecoder dec('G');
		dec.StartSession(ks);
		Frame f;
		for (size_t i = 0; i + 1 < f1.size(); i++)
		{
			dec.Feed(std::span(&f1[i], 1));
			CHECK(dec.Next(f) == FrameStatus::NeedMore);
		}
		dec.Feed(std::span(&f1.back(), 1));
		CHECK(dec.Next(f) == FrameStatus::Ok);
		CHECK(f.type == FrameType::Put && f.counter == 1 && f.data == payload && f.raw == f1);
		KeyValues kv;
		CHECK(ParseFields(f.text, kv) && Get(kv, "name") == "1_2.snapshot");
		dec.Feed(f2);
		dec.Feed(f3);
		CHECK(dec.Next(f) == FrameStatus::Ok && f.counter == 2);
		CHECK(dec.Next(f) == FrameStatus::Ok && f.counter == 3);
		CHECK(dec.Next(f) == FrameStatus::NeedMore);
	}
	// MAC failure: flipped data byte, wrong key, wrong direction. Errors are sticky.
	{
		std::vector<uint8_t> bad = f1;
		bad[bad.size() - 40] ^= 1;
		FrameDecoder dec('G');
		dec.StartSession(ks);
		Frame f;
		dec.Feed(bad);
		CHECK(dec.Next(f) == FrameStatus::BadMac);
		dec.Feed(f2);
		CHECK(dec.Next(f) == FrameStatus::BadMac);

		FrameDecoder wrongKey('G');
		wrongKey.StartSession(Fill(2));
		wrongKey.Feed(f1);
		CHECK(wrongKey.Next(f) == FrameStatus::BadMac);

		FrameDecoder wrongDir('H');
		wrongDir.StartSession(ks);
		wrongDir.Feed(f1);
		CHECK(wrongDir.Next(f) == FrameStatus::BadMac);
	}
	// Counter regression (replay) and skip.
	{
		Frame f;
		FrameDecoder replay('G');
		replay.StartSession(ks);
		replay.Feed(f1);
		replay.Feed(f1);
		CHECK(replay.Next(f) == FrameStatus::Ok);
		CHECK(replay.Next(f) == FrameStatus::BadCounter);

		FrameDecoder skip('G');
		skip.StartSession(ks);
		skip.Feed(f1);
		skip.Feed(f3);
		CHECK(skip.Next(f) == FrameStatus::Ok);
		CHECK(skip.Next(f) == FrameStatus::BadCounter);

		FrameDecoder first('G');
		first.StartSession(ks);
		first.Feed(f2);
		CHECK(first.Next(f) == FrameStatus::BadCounter);
	}
	// Handshake frames: counter 0, MAC checked by the caller, with context.
	{
		const Bytes32 k = Fill(9);
		Frame h;
		h.type = FrameType::Hello;
		h.text = "a=1\n";
		const std::vector<uint8_t> ctx = { 7, 7, 7 };
		CHECK(EncodeHandshakeFrame(h, 'H', &k, ctx) == FrameStatus::Ok);
		FrameDecoder dec('H');
		dec.Feed(h.raw);
		Frame f;
		CHECK(dec.Next(f) == FrameStatus::Ok && f.counter == 0 && f.raw == h.raw);
		CHECK(VerifyFrameMac(f, 'H', k, ctx));
		CHECK(!VerifyFrameMac(f, 'H', k));
		CHECK(!VerifyFrameMac(f, 'G', k, ctx));

		Frame unkeyed;
		unkeyed.type = FrameType::Hello;
		CHECK(EncodeHandshakeFrame(unkeyed, 'G', nullptr) == FrameStatus::Ok && unkeyed.mac == Bytes32{});

		// A session frame (counter 1) before the handshake completes.
		FrameDecoder early('G');
		early.Feed(f1);
		CHECK(early.Next(f) == FrameStatus::BadCounter);
	}
	// Caps.
	{
		Frame big;
		big.type = FrameType::Hello;
		big.text = "x=" + std::string(4096, 'a');
		CHECK(EncodeHandshakeFrame(big, 'G', nullptr) == FrameStatus::BadLength);

		std::vector<uint8_t> out;
		FrameEncoder e('H');
		e.StartSession(ks);
		CHECK(e.Encode(FrameType::File, "", std::vector<uint8_t>(1024 * 1024 + 18), out) == FrameStatus::Ok);
		CHECK(e.Encode(FrameType::File, "", std::vector<uint8_t>(1024 * 1024 + 19), out) == FrameStatus::BadLength);
		CHECK(e.Encode(FrameType::Put, std::string(kMaxText + 1, 'a'), {}, out) == FrameStatus::TextTooLong);

		auto header = [](uint32_t len, uint8_t version, uint8_t type, uint32_t textLen) {
			std::vector<uint8_t> h;
			for (int i = 0; i < 4; i++) h.push_back(static_cast<uint8_t>(len >> (8 * i)));
			h.push_back(version);
			h.push_back(type);
			for (int i = 0; i < 8; i++) h.push_back(0);
			for (int i = 0; i < 4; i++) h.push_back(static_cast<uint8_t>(textLen >> (8 * i)));
			return h;
		};
		Frame f;
		FrameDecoder d1('G');
		d1.Feed(header(4097, 1, 1, 0));
		CHECK(d1.Next(f) == FrameStatus::BadLength);
		FrameDecoder d2('G');
		d2.StartSession(ks);
		d2.Feed(header(4097, 1, 1, 0));
		CHECK(d2.Next(f) == FrameStatus::NeedMore);
		FrameDecoder d3('G');
		d3.StartSession(ks);
		d3.Feed(header(1024 * 1024 + 65, 1, 1, 0));
		CHECK(d3.Next(f) == FrameStatus::BadLength);
		FrameDecoder d4('G');
		d4.StartSession(ks);
		d4.Feed(header(1024 * 1024, 1, 1, 64 * 1024 + 1));
		CHECK(d4.Next(f) == FrameStatus::TextTooLong);
		FrameDecoder d5('G');
		d5.Feed(header(100, 1, 1, 60));
		CHECK(d5.Next(f) == FrameStatus::BadLength);
		FrameDecoder d6('G');
		d6.Feed(header(100, 1, 99, 0));
		CHECK(d6.Next(f) == FrameStatus::UnknownType);
		FrameDecoder d7('G');
		d7.Feed(header(100, 2, 1, 0));
		CHECK(d7.Next(f) == FrameStatus::BadVersion);
		FrameDecoder d8('G');
		d8.Feed(header(10, 1, 1, 0));
		CHECK(d8.Next(f) == FrameStatus::BadLength);
	}
	// Key=value text.
	{
		CHECK(!Fields().Add("k", "a\nb=c").Ok());
		CHECK(!Fields().Add("k", "a\r").Ok());
		CHECK(!Fields().Add("k=x", "v").Ok());
		CHECK(!Fields().Add("", "v").Ok());
		CHECK(Fields().Add("k", "a=b").Text() == "k=a=b\n");
		KeyValues kv;
		CHECK(ParseFields("a=1\nb=x=y\n", kv) && Get(kv, "b") == "x=y");
		CHECK(!ParseFields("a=1\r\n", kv));
		CHECK(!ParseFields("a=1\na=2\n", kv));
		CHECK(!ParseFields("a=1\nnoequals\n", kv));
		CHECK(!ParseFields("; comment\n", kv));
		CHECK(!ParseFields("=v\n", kv));
	}
}

static Hello MakeHello(const char* role, const std::string& nonce)
{
	Hello h;
	h.role = role;
	h.site = role[0] == 'h' ? "GAMEBOX-01" : "LAPTOP";
	h.siteId = role[0] == 'h' ? std::string(32, 'a') : std::string(32, 'b');
	h.nonce = nonce;
	h.build = "0.2.0+abc1234";
	h.pluginsHash = Hash('c');
	return h;
}

static void TestFieldFormats()
{
	CHECK(IsValidSiteId(std::string(32, 'f')));
	CHECK(!IsValidSiteId(std::string(32, 'F')));
	CHECK(!IsValidSiteId(std::string(31, 'f')));
	CHECK(!IsValidSiteId(std::string(31, 'f') + "\n"));
	CHECK(IsValidSite("GAMEBOX-01") && IsValidSite("a"));
	CHECK(!IsValidSite("") && !IsValidSite("ABCDEFGHIJKLMNOP") && !IsValidSite("pc_1") && !IsValidSite("pc 1"));
	CHECK(!IsValidSite("PC\r") && !IsValidSite("PC\n"));
	CHECK(IsValidBuild("0.2.0+abc1234") && IsValidBuild("10+0000000"));
	CHECK(!IsValidBuild("0.2.0+ABC1234") && !IsValidBuild("0..2+abc1234") && !IsValidBuild("+abc1234")
		&& !IsValidBuild("0.2.0abc1234") && !IsValidBuild("0.2.+abc1234") && !IsValidBuild("0.2.0+abc12345")
		&& !IsValidBuild("0.2.0+abc1234\r"));
	CHECK(IsValidPluginsHash(Hash('0')) && !IsValidPluginsHash(Hash('g')));
	CHECK(IsValidNonce(std::string(32, '9')) && !IsValidNonce(std::string(33, '9')));
	CHECK(IsValidY(Hash('e')) && !IsValidY(Hash('e').substr(1) + "\n"));
	CHECK(IsValidPairCode("012345") && !IsValidPairCode("12345") && !IsValidPairCode("12345a"));

	Hello h = MakeHello("host", std::string(32, '1'));
	h.result = "ok";
	h.regions = { "Timbuktu", "New Region" };
	h.y = Hash('d');
	std::string text, error;
	CHECK(BuildHelloText(h, text));
	Hello back;
	CHECK(ParseHello(text, back, error));
	CHECK(back.site == h.site && back.siteId == h.siteId && back.nonce == h.nonce && back.build == h.build
		&& back.pluginsHash == h.pluginsHash && back.result == "ok" && back.regions == h.regions && back.y == h.y && !back.pair);

	// Injection and bad values are refused at build and at parse.
	Hello injected = h;
	injected.site = "PC\nrole=guest";
	CHECK(!BuildHelloText(injected, text));
	injected = h;
	injected.regions = { "bad\nregion" };
	CHECK(!BuildHelloText(injected, text));
	CHECK(!ParseHello("proto=1\r\nrole=guest\n", back, error));
	std::string good;
	CHECK(BuildHelloText(MakeHello("guest", std::string(32, '2')), good));
	CHECK(ParseHello(good, back, error) && back.role == "guest");
	CHECK(!ParseHello(good + "site=X\n", back, error));          // duplicate key
	CHECK(!ParseHello(good + "region.0=R\n", back, error));      // regions from a guest
	CHECK(!ParseHello(good + "Y=" + Hash('d') + "\n", back, error));
	std::string hostText;
	CHECK(BuildHelloText(MakeHello("host", std::string(32, '3')), hostText));
	CHECK(!ParseHello(hostText + "region.1=R\n", back, error));  // gap
	CHECK(!ParseHello(hostText + "region.0=a/b\n", back, error));
	CHECK(!ParseHello(hostText + "region.0=CON\n", back, error));
	CHECK(ParseHello(hostText + "region.0=R\n", back, error) && back.regions.size() == 1);

	// 40 regions of 120 chars: truncated so the HELLO still fits the 4 KiB handshake cap.
	Hello many = MakeHello("host", std::string(32, '6'));
	many.y = Hash('d');
	for (int i = 0; i < 40; i++) many.regions.push_back(std::string(117, 'r') + std::to_string(100 + i));
	Frame mf;
	mf.type = FrameType::Hello;
	CHECK(BuildHelloText(many, mf.text));
	CHECK(EncodeHandshakeFrame(mf, 'H', nullptr) == FrameStatus::Ok);
	CHECK(ParseHello(mf.text, back, error) && back.regions.size() > 20 && back.regions.size() < 40 && back.y == many.y);
	std::string swapped = good;
	swapped.replace(swapped.find("pair=0"), 6, "pair=2");
	CHECK(!ParseHello(swapped, back, error));
}

struct Party
{
	Bytes32 scalar;
	Bytes32 y;
	Bytes32 k;
};

static void TestCpace()
{
	const std::string nonceG(32, '4'), nonceH(32, '5');
	const Hello gh = MakeHello("guest", nonceG);
	Hello hh = MakeHello("host", nonceH);

	auto run = [&](const char* guestCode, const char* hostCode, bool tamper, bool& confGOk, bool& confHOk, Bytes32& ksG, Bytes32& ksH) {
		Frame gHello, hHello;
		gHello.type = hHello.type = FrameType::Hello;
		Hello g = gh;
		g.pair = true;
		CHECK(BuildHelloText(g, gHello.text));
		CHECK(EncodeHandshakeFrame(gHello, 'G', nullptr) == FrameStatus::Ok);

		Party host{ Fill(40), {}, {} }, guest{ Fill(80), {}, {} };
		Bytes32 pH, pG;
		CHECK(CpaceGenerator(hostCode, hh.siteId, gh.siteId, nonceG, nonceH, pH));
		CHECK(CpacePublic(host.scalar, pH, host.y));
		Hello h = hh;
		h.y = ToHex(host.y);
		CHECK(BuildHelloText(h, hHello.text));
		CHECK(EncodeHandshakeFrame(hHello, 'H', nullptr) == FrameStatus::Ok);

		// Guest side.
		std::vector<uint8_t> hostHelloSeen = hHello.raw;
		if (tamper) hostHelloSeen[30] ^= 1;
		CHECK(CpaceGenerator(guestCode, hh.siteId, gh.siteId, nonceG, nonceH, pG));
		CHECK(CpacePublic(guest.scalar, pG, guest.y));
		const auto tG = CpaceTranscript(gHello.raw, hostHelloSeen, guest.y, host.y);
		CHECK(CpaceKey(guest.scalar, host.y, tG, guest.k));
		const Bytes32 confG = CpaceConfirmation(guest.k, 'G', tG);

		// PAKE frame text round trip.
		Bytes32 yIn, confIn;
		CHECK(ParsePakeText(BuildPakeText(guest.y, confG), yIn, confIn) && yIn == guest.y && confIn == confG);

		// Host side.
		const auto tH = CpaceTranscript(gHello.raw, hHello.raw, yIn, host.y);
		CHECK(CpaceKey(host.scalar, yIn, tH, host.k));
		confGOk = VerifyCpaceConfirmation(host.k, 'G', tH, confIn);
		const Bytes32 confH = CpaceConfirmation(host.k, 'H', tH);
		Bytes32 confHIn;
		CHECK(ParseConfirmText(BuildConfirmText(confH), confHIn));
		confHOk = VerifyCpaceConfirmation(guest.k, 'H', tG, confHIn);
		ksG = SessionKey(guest.k, nonceG, nonceH);
		ksH = SessionKey(host.k, nonceG, nonceH);
	};

	bool confGOk = false, confHOk = false;
	Bytes32 ksG, ksH;
	run("123456", "123456", false, confGOk, confHOk, ksG, ksH);
	CHECK(confGOk && confHOk && ksG == ksH);
	run("123457", "123456", false, confGOk, confHOk, ksG, ksH);
	CHECK(!confGOk && !confHOk && ksG != ksH);
	run("123456", "123456", true, confGOk, confHOk, ksG, ksH);
	CHECK(!confGOk && !confHOk);

	// The generator binds every input.
	Bytes32 p1, p2;
	CHECK(CpaceGenerator("123456", hh.siteId, gh.siteId, nonceG, nonceH, p1));
	CHECK(CpaceGenerator("123456", gh.siteId, hh.siteId, nonceG, nonceH, p2) && p1 != p2);
	CHECK(CpaceGenerator("123456", hh.siteId, gh.siteId, nonceH, nonceG, p2) && p1 != p2);
	CHECK(!CpaceGenerator("12345", hh.siteId, gh.siteId, nonceG, nonceH, p2));
	CHECK(!CpaceGenerator("123456", hh.siteId, gh.siteId, nonceG + "\n", nonceH, p2));

	// Low-order and zero peers.
	Bytes32 k;
	const std::vector<uint8_t> t = { 1 };
	CHECK(!CpaceKey(Fill(3), Bytes32{}, t, k));
	Bytes32 one{};
	one[0] = 1;
	CHECK(!CpaceKey(Fill(3), one, t, k));
	CHECK(!CpacePublic(Fill(3), one, k));

	// Session key binds the nonces.
	CHECK(SessionKey(Fill(5), nonceG, nonceH) != SessionKey(Fill(5), nonceH, nonceG));

	// Paired reconnect: HELLO MACs with K, host HELLO over both, CONFIRM adds nonceH.
	const Bytes32 K = Fill(60);
	Frame g, h, c;
	g.type = h.type = FrameType::Hello;
	c.type = FrameType::Confirm;
	CHECK(BuildHelloText(gh, g.text) && EncodeHandshakeFrame(g, 'G', &K) == FrameStatus::Ok);
	CHECK(BuildHelloText(hh, h.text) && EncodeHandshakeFrame(h, 'H', &K, g.raw) == FrameStatus::Ok);
	const auto ctx = PairedConfirmContext(g.raw, h.raw, nonceH);
	CHECK(EncodeHandshakeFrame(c, 'G', &K, ctx) == FrameStatus::Ok);
	CHECK(VerifyFrameMac(g, 'G', K) && VerifyFrameMac(h, 'H', K, g.raw) && VerifyFrameMac(c, 'G', K, ctx));
	CHECK(!VerifyFrameMac(g, 'G', Fill(61)));
	Frame otherG = g;
	otherG.text += "x=1\n";
	CHECK(EncodeHandshakeFrame(otherG, 'G', &K) == FrameStatus::Ok);
	CHECK(!VerifyFrameMac(h, 'H', K, otherG.raw));
	CHECK(!VerifyFrameMac(c, 'G', K, PairedConfirmContext(g.raw, h.raw, nonceG)));
}

static void TestNames()
{
	CHECK(IsValidRegionName("Timbuktu") && IsValidRegionName("New Region (2)") && IsValidRegionName(std::string(120, 'a')));
	CHECK(!IsValidRegionName(std::string(121, 'a')));
	for (const char* bad : { "", ".", "..", "..\\x", "../x", "a/b", "a\\b", "C:x", "a~b", "City~1", " a", "a ", ".a", "a.",
		"a*", "a?", "a|b", "a<b", "a\"b", "CON", "con", "Con.txt", "NUL .sc4", "LPT1.sc4", "com9", "AUX.a.b", "CONOUT$" })
	{
		if (IsValidRegionName(bad)) std::printf("FAIL region name accepted: '%s'\n", bad), failures++;
	}
	CHECK(!IsValidRegionName("a\tb") && !IsValidRegionName("a\nb") && !IsValidRegionName("R\xC3\xA9gion"));
	CHECK(IsValidRegionName("CONSOLE") && IsValidRegionName("COM10") && IsValidRegionName("NULL"));

	CHECK(IsValidRegionFileName("City - Worth.sc4") && IsValidRegionFileName("X.SC4"));
	CHECK(IsValidRegionFileName("region.ini") && IsValidRegionFileName("Region.INI") && IsValidRegionFileName("CONFIG.bmp"));
	CHECK(!IsValidRegionFileName("foo.txt") && !IsValidRegionFileName("desktop.ini") && !IsValidRegionFileName(".sc4")
		&& !IsValidRegionFileName("a.sc4.") && !IsValidRegionFileName("..\\a.sc4") && !IsValidRegionFileName("CON.sc4")
		&& !IsValidRegionFileName("a.sc4x") && !IsValidRegionFileName("config.bmp.sc4~"));

	Tile t;
	CHECK(ParseSnapshotName("0_0.snapshot", 16, 16, t) && t == Tile{ 0, 0 });
	CHECK(ParseSnapshotName("15_9.snapshot", 16, 16, t) && t == Tile{ 15, 9 });
	for (const char* bad : { "01_2.snapshot", "1_02.snapshot", "-1_0.snapshot", "+1_0.snapshot", "10000_0.snapshot",
		"1_2.snapshot.tmp", "1-2.snapshot", "1_2.Snapshot", "_2.snapshot", ".snapshot", "1_2_3.snapshot", "1 _2.snapshot" })
	{
		if (ParseSnapshotName(bad, 20000, 20000, t)) std::printf("FAIL snapshot name accepted: '%s'\n", bad), failures++;
	}
	CHECK(!ParseSnapshotName("16_0.snapshot", 16, 16, t) && !ParseSnapshotName("0_16.snapshot", 16, 16, t));
	CHECK(ParseSnapshotName("9999_9999.snapshot", 10000, 10000, t));

	uint32_t pid = 0;
	uint64_t start = 0;
	CHECK(ParseStatusName("1234-134000000000000000.status", pid, start) && pid == 1234 && start == 134000000000000000ULL);
	CHECK(ParseStatusName("4294967295-18446744073709551615.status", pid, start));
	for (const char* bad : { "0-1.status", "1-0.status", "01-1.status", "4294967296-1.status", "1-18446744073709551616.status",
		"1-2.status.tmp", "1-2.Status", "-1.status", "1-.status", "a-1.status", "1-2-3.status", "1--2.status" })
	{
		if (ParseStatusName(bad, pid, start)) std::printf("FAIL status name accepted: '%s'\n", bad), failures++;
	}
}

static void TestManifest()
{
	Manifest m;
	m.region = "Timbuktu";
	m.files = { { "City - A.sc4", 1234, Hash('a'), Tile{ 0, 0 } }, { "region.ini", 10, Hash('b'), std::nullopt },
		{ "config.bmp", 1ull << 30, Hash('c'), std::nullopt } };
	std::string text, data, error;
	CHECK(BuildManifestText(m, text, data, error));
	CHECK(text == "region=Timbuktu\ncount=3\n");
	Manifest back;
	CHECK(ParseManifest(text, data, back, error));
	CHECK(back.region == m.region && back.files.size() == 3 && back.files[0].name == "City - A.sc4"
		&& back.files[0].size == 1234 && back.files[0].tile == (Tile{ 0, 0 }) && !back.files[1].tile && back.files[2].size == (1ull << 30));

	Manifest bad = m;
	bad.files.clear();
	CHECK(!BuildManifestText(bad, text, data, error));
	CHECK(!ParseManifest("region=R\ncount=0\n", "", back, error));
	bad = m;
	bad.files[0].size = (1ull << 30) + 1;
	CHECK(!BuildManifestText(bad, text, data, error));
	bad = m;
	bad.files.push_back({ "CITY - a.SC4", 1, Hash('d'), std::nullopt });
	CHECK(!BuildManifestText(bad, text, data, error));
	CHECK(!ParseManifest("region=R\ncount=2\n", "file.0=a.sc4|1|" + Hash('a') + "|\nfile.1=A.sc4|1|" + Hash('a') + "|\n", back, error));
	CHECK(!ParseManifest("region=R\ncount=1\n", "file.0=..\\a.sc4|1|" + Hash('a') + "|\n", back, error));
	CHECK(!ParseManifest("region=R\ncount=1\n", "file.0=a.txt|1|" + Hash('a') + "|\n", back, error));
	CHECK(!ParseManifest("region=R\ncount=1\n", "file.0=a.sc4|1|" + Hash('a') + "|01_1\n", back, error));
	CHECK(!ParseManifest("region=R\ncount=1\n", "file.0=a.sc4|01|" + Hash('a') + "|\n", back, error));
	CHECK(!ParseManifest("region=R\ncount=1\n", "file.0=a.sc4|1|" + Hash('A') + "|\n", back, error));
	CHECK(!ParseManifest("region=R\ncount=2\n", "file.0=a.sc4|1|" + Hash('a') + "|\n", back, error));
	CHECK(!ParseManifest("region=R\ncount=1\n", "file.0=a.sc4|1|" + Hash('a') + "|\nfile.1=b.sc4|1|" + Hash('a') + "|\n", back, error));
	CHECK(!ParseManifest("region=a/b\ncount=1\n", "file.0=a.sc4|1|" + Hash('a') + "|\n", back, error));
	CHECK(ParseManifest("region=R\ncount=1\n", "file.0=a.sc4|1|" + Hash('a') + "|3_4\n", back, error) && back.files[0].tile == (Tile{ 3, 4 }));
	bad = m;
	bad.files.clear();
	for (size_t i = 0; i < kMaxManifestFiles + 1; i++) bad.files.push_back({ "c" + std::to_string(i) + ".sc4", 1, Hash('a'), std::nullopt });
	CHECK(!BuildManifestText(bad, text, data, error));
	std::string many;
	for (size_t i = 0; i < 1025; i++) many += "file." + std::to_string(i) + "=c" + std::to_string(i) + ".sc4|1|" + Hash('a') + "|\n";
	CHECK(!ParseManifest("region=R\ncount=1025\n", many, back, error));
	CHECK(!ParseManifest("region=R\ncount=1\nfile.0=a.sc4|1|" + Hash('a') + "|\n", "", back, error));
	CHECK(!ParseManifest("region=R\n", "file.0=a.sc4|1|" + Hash('a') + "|\n", back, error));
	CHECK(!ParseManifest("region=R\ncount=1\n", "region=S\nfile.0=a.sc4|1|" + Hash('a') + "|\n", back, error));

	// Over 64 KiB, under 1 MiB: 1024 files with long names.
	Manifest large;
	large.region = "R";
	for (size_t i = 0; i < kMaxManifestFiles; i++)
		large.files.push_back({ "City - " + std::string(100, 'x') + std::to_string(i) + ".sc4", i, Hash('a'), Tile{ int32_t(i % 64), int32_t(i / 64) } });
	CHECK(BuildManifestText(large, text, data, error));
	CHECK(data.size() > kMaxText);
	CHECK(ParseManifest(text, data, back, error) && back.files.size() == kMaxManifestFiles && back.files[1023].tile == (Tile{ 63, 15 }));
	std::vector<uint8_t> out;
	FrameEncoder enc('H');
	enc.StartSession(Fill(3));
	CHECK(enc.Encode(FrameType::Manifest, text, std::span(reinterpret_cast<const uint8_t*>(data.data()), data.size()), out) == FrameStatus::Ok);
}

static void TestDiff()
{
	const Tile t00{ 0, 0 }, t10{ 1, 0 }, t20{ 2, 0 }, t30{ 3, 0 };
	Manifest m;
	m.region = "R";
	m.files = {
		{ "region.ini", 10, Hash('1'), std::nullopt },
		{ "config.bmp", 20, Hash('2'), std::nullopt },
		{ "City - Same.sc4", 100, Hash('3'), t00 },
		{ "City - Neighbor.sc4", 200, Hash('4'), t10 },     // guest neighbor-wrote into this host tile
		{ "City - New.sc4", 300, Hash('5'), std::nullopt }, // no local copy
	};
	Journal j;
	j.tiles[t00] = { "City - Same.sc4", Hash('3'), 2 };
	j.tiles[t10] = { "City - Neighbor.sc4", Hash('4'), 5 };
	std::map<Tile, SaveCounter> saves;
	saves[t00] = { 2, "City - Same.sc4", "" };
	saves[t10] = { 5, "City - Neighbor.sc4", "" };
	std::map<std::string, std::string> local = {
		{ "region.ini", Hash('1') },
		{ "config.bmp", Hash('9') },
		{ "city - same.SC4", Hash('3') },
		{ "City - Neighbor.sc4", Hash('8') },
		{ "City - Gone.sc4", Hash('7') },
		{ "Thumbs.db", Hash('6') },
	};

	DiffResult r = DiffRegion(m, local, saves, j, {});
	CHECK(r.keptTiles.empty());
	CHECK(ActionOf(r, "region.ini") == DiffAction::Same);
	CHECK(ActionOf(r, "config.bmp") == DiffAction::Download);
	CHECK(ActionOf(r, "City - Same.sc4") == DiffAction::Same && Find(r, "City - Same.sc4")->localName == "city - same.SC4");
	CHECK(ActionOf(r, "City - Neighbor.sc4") == DiffAction::Download);
	CHECK(ActionOf(r, "City - New.sc4") == DiffAction::Download && Find(r, "City - New.sc4")->localName.empty());
	CHECK(ActionOf(r, "City - Gone.sc4") == DiffAction::Archive);
	CHECK(ActionOf(r, "Thumbs.db") == DiffAction::Archive);
	CHECK(r.items.size() == 7);
	CHECK(r.downloadBytes == 20 + 200 + 300);

	// Kept tile: a guest save (count > baseCount). Its file stays and nothing of the tile is fetched.
	saves[t10].count = 6;
	r = DiffRegion(m, local, saves, j, {});
	CHECK(r.keptTiles == std::vector<Tile>{ t10 });
	CHECK(ActionOf(r, "City - Neighbor.sc4") == DiffAction::KeepNotSent);
	CHECK(r.downloadBytes == 20 + 300);
	saves[t10].count = 5;

	// saveCountFailed: counter unchanged but the writer could not record its save.
	r = DiffRegion(m, local, saves, j, { t10 });
	CHECK(r.keptTiles == std::vector<Tile>{ t10 } && ActionOf(r, "City - Neighbor.sc4") == DiffAction::KeepNotSent);

	// Offline save of a new city on an empty tile: no journal entry, count 1, file not in the manifest.
	{
		auto s2 = saves;
		auto l2 = local;
		s2[t20] = { 1, "City - Offline.sc4", "" };
		l2["City - Offline.sc4"] = Hash('a');
		r = DiffRegion(m, l2, s2, j, {});
		CHECK(r.keptTiles == std::vector<Tile>{ t20 } && ActionOf(r, "City - Offline.sc4") == DiffAction::KeepNotSent);
	}

	// Guest renamed its city (save under a new name): both names stay, the host's old-name copy is not fetched.
	// The manifest entry has no tile, so only the journal and .saves claim it.
	{
		Manifest m2 = m;
		m2.files[2].tile.reset();
		auto s2 = saves;
		auto l2 = local;
		s2[t00] = { 3, "City - Renamed.sc4", "City - Same.sc4" };
		l2["City - Renamed.sc4"] = Hash('b');
		r = DiffRegion(m2, l2, s2, j, {});
		CHECK(r.keptTiles == std::vector<Tile>{ t00 });
		CHECK(ActionOf(r, "City - Same.sc4") == DiffAction::KeepNotSent);
		CHECK(ActionOf(r, "City - Renamed.sc4") == DiffAction::KeepNotSent);

		// Renamed again: the old name is claimed by the journal alone, the middle one by prevFile alone.
		s2[t00] = { 4, "City - Third.sc4", "City - Renamed.sc4" };
		l2.erase("city - same.SC4");
		r = DiffRegion(m2, l2, s2, j, {});
		CHECK(ActionOf(r, "City - Same.sc4") == DiffAction::KeepNotSent && Find(r, "City - Same.sc4")->localName.empty());
		CHECK(ActionOf(r, "City - Renamed.sc4") == DiffAction::KeepNotSent);
		CHECK(r.downloadBytes == 20 + 200 + 300);
	}

	// A kept tile that names no file (saveCountFailed, no .saves, no journal file, no manifest tile):
	// every unclaimed local .sc4 that differs stays.
	{
		auto l2 = local;
		l2["City - Mystery.sc4"] = Hash('e');
		r = DiffRegion(m, l2, saves, j, { t30 });
		CHECK(r.keptTiles == std::vector<Tile>{ t30 });
		CHECK(ActionOf(r, "City - Mystery.sc4") == DiffAction::KeepNotSent);
		CHECK(ActionOf(r, "City - Gone.sc4") == DiffAction::KeepNotSent);
		CHECK(ActionOf(r, "City - Neighbor.sc4") == DiffAction::KeepNotSent);
		CHECK(ActionOf(r, "City - Same.sc4") == DiffAction::Same);
		CHECK(ActionOf(r, "Thumbs.db") == DiffAction::Archive);
		CHECK(ActionOf(r, "config.bmp") == DiffAction::Download);
	}

	// Host renamed its city: new name downloaded, old local name archived.
	{
		Manifest m2 = m;
		m2.files[2] = { "City - HostRenamed.sc4", 111, Hash('c'), t00 };
		r = DiffRegion(m2, local, saves, j, {});
		CHECK(r.keptTiles.empty());
		CHECK(ActionOf(r, "City - HostRenamed.sc4") == DiffAction::Download);
		CHECK(ActionOf(r, "city - same.SC4") == DiffAction::Archive);
	}

	// A kept tile claims a manifest file through the manifest's tile field alone.
	{
		Manifest m2 = m;
		m2.files.push_back({ "City - Host Only.sc4", 50, Hash('d'), t30 });
		auto s2 = saves;
		s2[t30] = { 1, "City - Guest Only.sc4", "" };
		r = DiffRegion(m2, local, s2, j, {});
		CHECK(ActionOf(r, "City - Host Only.sc4") == DiffAction::KeepNotSent);
	}
}

static void TestInstanceStatus()
{
	regionsync::InstanceStatus s;
	regionsync::ParseInstanceStatus("; x\nschemaVersion=1\nhost=GAMEBOX-01\npid=42\nstart=134000000000000000\nuserDir=C:\\Users\\player\\Instance1\r\n"
		"state=city\nregion=Timbuktu\nregionX=3\nregionZ=7\ncityName=Worth\nconflict=1\nsaveCountFailed=3_7,0_1\nwrittenUnixTime=1760000000\n", s);
	CHECK(s.writer.host == "GAMEBOX-01" && s.writer.pid == 42 && s.writer.start == 134000000000000000ULL);
	CHECK(s.userDir == "C:\\Users\\player\\Instance1" && s.state == "city" && s.region == "Timbuktu");
	CHECK(s.regionX == 3 && s.regionZ == 7 && s.cityName == "Worth" && s.conflict && s.saveCountFailedTiles.size() == 2 && s.saveCountFailedTiles[1].second == 1 && s.writtenUnixTime == 1760000000);
	regionsync::ParseInstanceStatus("host=GAMEBOX-01\npid=42\nstate=region\n", s);
	CHECK(!s.writer.Known() && s.state == "region" && s.regionX == -1 && s.saveCountFailedTiles.empty());

	regionsync::SaveCounter c;
	regionsync::ParseSaveCounter("count=4\nfile=B.sc4\nprevFile=A.sc4\nlastOk=1\n", c);
	CHECK(c.count == 4 && c.file == "B.sc4" && c.prevFile == "A.sc4" && c.lastOk == 1);
}

static void TestJournal()
{
	Journal j;
	j.tiles[Tile{ 0, 0 }] = { "City - A.sc4", Hash('a'), 3 };
	j.tiles[Tile{ 12, 4 }] = { "", "", 0 };
	std::string error;
	Journal back;
	CHECK(ParseJournalText(BuildJournalText(j), back, error));
	CHECK(back.tiles.size() == 2 && back.tiles[Tile{ 0, 0 }].file == "City - A.sc4" && back.tiles[Tile{ 0, 0 }].baseHash == Hash('a')
		&& back.tiles[Tile{ 0, 0 }].baseCount == 3 && back.tiles[Tile{ 12, 4 }].file.empty());
	CHECK(!ParseJournalText("01_0=a.sc4|" + Hash('a') + "|1\n", back, error));
	CHECK(!ParseJournalText("0_0=a.sc4|" + Hash('a') + "\n", back, error));
	CHECK(!ParseJournalText("0_0=a.txt|" + Hash('a') + "|1\n", back, error));
	CHECK(!ParseJournalText("0_0=a.sc4||1\n", back, error));
	CHECK(!ParseJournalText("0_0=a.sc4|" + Hash('a') + "|-1\n", back, error));

	// After a swap: kept tile untouched, others take the manifest file and the local count.
	const Tile t00{ 0, 0 }, t10{ 1, 0 }, t20{ 2, 0 }, t30{ 3, 0 };
	Manifest m;
	m.region = "R";
	m.files = { { "City - A2.sc4", 1, Hash('b'), t00 }, { "City - B.sc4", 1, Hash('c'), std::nullopt }, { "region.ini", 1, Hash('d'), std::nullopt } };
	Journal jj;
	jj.tiles[t00] = { "City - A.sc4", Hash('a'), 3 };
	jj.tiles[t10] = { "City - B.sc4", Hash('9'), 1 };
	jj.tiles[t30] = { "City - Deleted.sc4", Hash('8'), 2 };
	std::map<Tile, SaveCounter> saves;
	saves[t00] = { 3, "City - A.sc4", "" };
	saves[t10] = { 2, "City - B.sc4", "" };
	saves[t20] = { 7, "City - Kept.sc4", "" };
	UpdateJournalAfterSwap(jj, m, saves, { t20 });
	CHECK(jj.tiles[t00].file == "City - A2.sc4" && jj.tiles[t00].baseHash == Hash('b') && jj.tiles[t00].baseCount == 3);
	CHECK(jj.tiles[t10].file == "City - B.sc4" && jj.tiles[t10].baseHash == Hash('c') && jj.tiles[t10].baseCount == 2);
	CHECK(jj.tiles.count(t20) == 0);
	CHECK(jj.tiles[t30].file.empty() && jj.tiles[t30].baseHash.empty() && jj.tiles[t30].baseCount == 2);
	CHECK(ParseJournalText(BuildJournalText(jj), back, error) && back.tiles.size() == 3);
}

int main()
{
	TestFrames();
	TestFieldFormats();
	TestCpace();
	TestNames();
	TestManifest();
	TestDiff();
	TestInstanceStatus();
	TestJournal();
	if (failures)
	{
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("OK\n");
	return 0;
}
