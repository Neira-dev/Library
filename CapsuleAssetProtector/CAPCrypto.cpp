#include "CAPCrypto.h"

#include <windows.h>
#include <bcrypt.h>
#include <cstring>

#pragma comment(lib, "bcrypt.lib")

// NTSTATUSの成功判定(bcryptは0以上が成功)
#ifndef BCRYPT_SUCCESS
#define BCRYPT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

namespace cap
{
	// AESのブロックサイズ(=IVの長さ)。CBCモードでは16バイト固定。
	static const ULONG kAesBlockSize = 16;

	void CAPCrypto::SetKey(const std::string& key)
	{
		// パスフレーズをSHA-256に通して必ず32バイト(AES-256の鍵長)に変換する。
		// これにより、ユーザーが渡す鍵文字列の長さに関わらず常に正しい鍵長になる。
		BCRYPT_ALG_HANDLE hAlg = nullptr;
		BCRYPT_HASH_HANDLE hHash = nullptr;

		if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
		{
			return;
		}

		if (BCRYPT_SUCCESS(BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0)))
		{
			BCryptHashData(hHash, reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())), static_cast<ULONG>(key.size()), 0);
			BCryptFinishHash(hHash, _key, sizeof(_key), 0);
			_hasKey = true;
			BCryptDestroyHash(hHash);
		}

		BCryptCloseAlgorithmProvider(hAlg, 0);
	}

	bool CAPCrypto::Encrypt(const std::vector<uint8_t>& plain, std::vector<uint8_t>& outCipher) const
	{
		if (!_hasKey)
		{
			return false;
		}

		BCRYPT_ALG_HANDLE hAlg = nullptr;
		if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0)))
		{
			return false;
		}

		// CBCモードに設定する
		bool ok = false;
		if (BCRYPT_SUCCESS(BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
			reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
			sizeof(BCRYPT_CHAIN_MODE_CBC), 0)))
		{
			BCRYPT_KEY_HANDLE hKey = nullptr;
			if (BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0,
				const_cast<PUCHAR>(_key), sizeof(_key), 0)))
			{
				// IV(初期化ベクトル)を暗号化のたびにランダム生成する。
				// 同じ平文でも毎回異なる暗号文になり、解析されにくくなる。
				uint8_t iv[kAesBlockSize] = {};
				if (BCRYPT_SUCCESS(BCryptGenRandom(nullptr, iv, kAesBlockSize, BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
				{
					// BCryptEncryptはIVバッファを書き換えるので、コピーを渡す
					uint8_t ivWork[kAesBlockSize] = {};
					memcpy(ivWork, iv, kAesBlockSize);

					// まず必要な出力サイズを問い合わせる(パディング分を含む)
					ULONG cipherSize = 0;
					if (BCRYPT_SUCCESS(BCryptEncrypt(hKey,
						const_cast<PUCHAR>(plain.data()), static_cast<ULONG>(plain.size()),
						nullptr, ivWork, kAesBlockSize,
						nullptr, 0, &cipherSize, BCRYPT_BLOCK_PADDING)))
					{
						// 出力 = IV(先頭16バイト) + 暗号本体
						outCipher.resize(kAesBlockSize + cipherSize);
						memcpy(outCipher.data(), iv, kAesBlockSize);

						ULONG written = 0;
						if (BCRYPT_SUCCESS(BCryptEncrypt(hKey,
							const_cast<PUCHAR>(plain.data()), static_cast<ULONG>(plain.size()),
							nullptr, ivWork, kAesBlockSize,
							outCipher.data() + kAesBlockSize, cipherSize, &written, BCRYPT_BLOCK_PADDING)))
						{
							outCipher.resize(kAesBlockSize + written);
							ok = true;
						}
					}
				}
				BCryptDestroyKey(hKey);
			}
		}

		BCryptCloseAlgorithmProvider(hAlg, 0);
		return ok;
	}

	bool CAPCrypto::Decrypt(const std::vector<uint8_t>& cipher, std::vector<uint8_t>& outPlain) const
	{
		if (!_hasKey)
		{
			return false;
		}
		// 先頭16バイトのIVが無ければ復号できない
		if (cipher.size() < kAesBlockSize)
		{
			return false;
		}

		BCRYPT_ALG_HANDLE hAlg = nullptr;
		if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0)))
		{
			return false;
		}

		bool ok = false;
		if (BCRYPT_SUCCESS(BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
			reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
			sizeof(BCRYPT_CHAIN_MODE_CBC), 0)))
		{
			BCRYPT_KEY_HANDLE hKey = nullptr;
			if (BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0,
				const_cast<PUCHAR>(_key), sizeof(_key), 0)))
			{
				// 先頭16バイトをIVとして取り出す(BCryptDecryptが書き換えるのでコピーを使う)
				uint8_t ivWork[kAesBlockSize] = {};
				memcpy(ivWork, cipher.data(), kAesBlockSize);

				const uint8_t* body = cipher.data() + kAesBlockSize;
				ULONG bodySize = static_cast<ULONG>(cipher.size() - kAesBlockSize);

				// まず必要な平文サイズを問い合わせる
				ULONG plainSize = 0;
				if (BCRYPT_SUCCESS(BCryptDecrypt(hKey,
					const_cast<PUCHAR>(body), bodySize,
					nullptr, ivWork, kAesBlockSize,
					nullptr, 0, &plainSize, BCRYPT_BLOCK_PADDING)))
				{
					outPlain.resize(plainSize);
					ULONG written = 0;
					if (BCRYPT_SUCCESS(BCryptDecrypt(hKey,
						const_cast<PUCHAR>(body), bodySize,
						nullptr, ivWork, kAesBlockSize,
						outPlain.data(), plainSize, &written, BCRYPT_BLOCK_PADDING)))
					{
						outPlain.resize(written);
						ok = true;
					}
				}
				BCryptDestroyKey(hKey);
			}
		}

		BCryptCloseAlgorithmProvider(hAlg, 0);
		return ok;
	}
}
