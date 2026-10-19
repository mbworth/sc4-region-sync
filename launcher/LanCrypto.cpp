/*
 * sc4-region-sync launcher
 *
 * LanCrypto.cpp - system randomness and DPAPI for the stored PeerKey.
 */

#include "Lan.h"
#include <bcrypt.h>
#include <wincrypt.h>
#include <dpapi.h>

namespace launcher
{
	bool RandomBytes(void* out, size_t size)
	{
		return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
	}

	// CryptProtectData (current user), base64 without line breaks.
	bool ProtectKey(const lan::Bytes32& key, std::wstring& base64)
	{
		DATA_BLOB in{ static_cast<DWORD>(key.size()), const_cast<BYTE*>(key.data()) };
		DATA_BLOB out{};
		if (!CryptProtectData(&in, L"SC4RegionSync PeerKey", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
		DWORD chars = 0;
		const DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
		bool ok = CryptBinaryToStringW(out.pbData, out.cbData, flags, nullptr, &chars) != FALSE;
		if (ok)
		{
			base64.assign(chars, L'\0');
			ok = CryptBinaryToStringW(out.pbData, out.cbData, flags, base64.data(), &chars) != FALSE;
			base64.resize(chars);
		}
		LocalFree(out.pbData);
		return ok;
	}

	bool UnprotectKey(const std::wstring& base64, lan::Bytes32& key)
	{
		if (base64.empty() || base64.size() > 4096) return false;
		DWORD bytes = 0;
		if (!CryptStringToBinaryW(base64.c_str(), static_cast<DWORD>(base64.size()), CRYPT_STRING_BASE64, nullptr, &bytes, nullptr, nullptr)) return false;
		std::vector<BYTE> blob(bytes);
		if (!CryptStringToBinaryW(base64.c_str(), static_cast<DWORD>(base64.size()), CRYPT_STRING_BASE64, blob.data(), &bytes, nullptr, nullptr)) return false;
		DATA_BLOB in{ bytes, blob.data() };
		DATA_BLOB out{};
		if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
		const bool ok = out.cbData == key.size();
		if (ok) memcpy(key.data(), out.pbData, key.size());
		SecureZeroMemory(out.pbData, out.cbData);
		LocalFree(out.pbData);
		return ok;
	}
}
