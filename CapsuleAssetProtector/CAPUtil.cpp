#include "CAPUtil.h"

#include <windows.h>
#include <filesystem>
#include <fstream>

namespace cap
{
	namespace
	{
		// 最後に起きたエラーメッセージ
		std::string g_lastError;

		std::wstring Convert(UINT codePage, const std::string& text)
		{
			if (text.empty()) return std::wstring();
			const int length = MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
			std::wstring result(length, L'\0');
			MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
			return result;
		}

		std::string Convert(UINT codePage, const std::wstring& text)
		{
			if (text.empty()) return std::string();
			const int length = WideCharToMultiByte(codePage, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
			std::string result(length, '\0');
			WideCharToMultiByte(codePage, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
			return result;
		}

		// splitmix64(ハッシュ値の偏りを散らすための仕上げ処理)
		uint64_t Finalize(uint64_t x)
		{
			x += 0x9E3779B97F4A7C15ull;
			x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
			x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
			return x ^ (x >> 31);
		}
	}

	std::wstring ToWide(const std::string& acp) { return Convert(CP_ACP, acp); }
	std::string ToNarrow(const std::wstring& wide) { return Convert(CP_ACP, wide); }
	std::string ToUtf8(const std::wstring& wide) { return Convert(CP_UTF8, wide); }
	std::wstring FromUtf8(const std::string& utf8) { return Convert(CP_UTF8, utf8); }

	std::string NormalizePath(const std::string& path)
	{
		// Shift-JISの2バイト目に'\\'(0x5C)が来る文字があるため、バイト単位ではなくUTF-16で処理する
		std::wstring wide = ToWide(path);
		std::wstring normalized = std::filesystem::path(wide).lexically_normal().generic_wstring();
		if (!normalized.empty())
		{
			CharLowerBuffW(normalized.data(), static_cast<DWORD>(normalized.size()));
		}
		return ToUtf8(normalized);
	}

	AssetKey MakeAssetKey(const std::string& path)
	{
		const std::string normalized = NormalizePath(path);

		// 互いに独立した2系統のFNV-1aを合わせて128bitにする
		uint64_t a = 14695981039346656037ull;
		uint64_t b = 0x84222325CBF29CE4ull;
		for (unsigned char c : normalized)
		{
			a ^= c;
			a *= 1099511628211ull;
			b ^= c;
			b *= 0x00000100000001B3ull ^ 0x9E37ull;
		}

		AssetKey key;
		key.hi = Finalize(a);
		key.lo = Finalize(b ^ normalized.size());
		return key;
	}

	std::string GetExtension(const std::string& path)
	{
		if (IsVirtualPath(path)) return std::string();
		std::wstring ext = std::filesystem::path(ToWide(path)).extension().wstring();
		if (!ext.empty())
		{
			CharLowerBuffW(ext.data(), static_cast<DWORD>(ext.size()));
		}
		return ToNarrow(ext);
	}

	bool IsVirtualPath(const std::string& path)
	{
		return path.find('#') != std::string::npos;
	}

	bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out)
	{
		std::ifstream file(ToWide(path), std::ios::binary | std::ios::ate);
		if (!file)
		{
			return false;
		}
		const std::streamsize size = file.tellg();
		file.seekg(0, std::ios::beg);
		out.resize(static_cast<size_t>(size));
		if (size > 0 && !file.read(reinterpret_cast<char*>(out.data()), size))
		{
			return false;
		}
		return true;
	}

	bool GetSourceStamp(const std::string& path, uint64_t& outStamp)
	{
		if (IsVirtualPath(path)) return false;

		std::error_code ec;
		const std::filesystem::path p(ToWide(path));
		if (!std::filesystem::is_regular_file(p, ec))
		{
			return false;
		}
		const auto time = std::filesystem::last_write_time(p, ec);
		if (ec) return false;
		const auto size = std::filesystem::file_size(p, ec);
		if (ec) return false;

		// 更新日時かサイズのどちらかが変われば値が変わる
		outStamp = Finalize(static_cast<uint64_t>(time.time_since_epoch().count())) ^ static_cast<uint64_t>(size);
		return true;
	}

	void SetError(const std::string& message)
	{
		g_lastError = message;
		OutputDebugStringA(("[CapsuleAssetProtector] " + message + "\n").c_str());
	}

	const char* GetError()
	{
		return g_lastError.c_str();
	}
}
