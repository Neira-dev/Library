#include "CAPSound.h"

#include <cstring>

namespace cap
{
	namespace
	{
		constexpr uint32_t kSoundVersion = 1;

		uint32_t ReadU32(const uint8_t* p)
		{
			uint32_t value = 0;
			memcpy(&value, p, sizeof(value));
			return value;
		}
	}

	bool CookSound(const std::string& path, std::vector<uint8_t>& outCooked)
	{
		std::vector<uint8_t> file;
		if (!ReadFileBytes(path, file))
		{
			SetError("サウンドファイルを開けません: " + path);
			return false;
		}
		if (file.size() < 12 || memcmp(file.data(), "RIFF", 4) != 0 || memcmp(file.data() + 8, "WAVE", 4) != 0)
		{
			SetError("WAV形式ではありません: " + path);
			return false;
		}

		// RIFFのチャンクを順に見て、fmtとdataを探す
		const uint8_t* format = nullptr;
		uint32_t formatSize = 0;
		const uint8_t* samples = nullptr;
		uint32_t sampleSize = 0;
		size_t position = 12;
		while (position + 8 <= file.size())
		{
			const uint8_t* chunk = file.data() + position;
			const uint32_t chunkSize = ReadU32(chunk + 4);
			const size_t dataStart = position + 8;
			if (dataStart + chunkSize > file.size())
			{
				break;
			}
			if (memcmp(chunk, "fmt ", 4) == 0)
			{
				format = file.data() + dataStart;
				formatSize = chunkSize;
			}
			else if (memcmp(chunk, "data", 4) == 0)
			{
				samples = file.data() + dataStart;
				sampleSize = chunkSize;
			}
			// チャンクは2バイト境界に揃えられている
			position = dataStart + chunkSize + (chunkSize & 1);
		}

		if (format == nullptr || samples == nullptr || formatSize < 16)
		{
			SetError("WAVファイルにfmtまたはdataがありません: " + path);
			return false;
		}

		BinaryWriter writer;
		writer.Write<uint32_t>(kSoundVersion);
		writer.Write<uint32_t>(formatSize);
		writer.WriteBytes(format, formatSize);
		writer.Write<uint32_t>(sampleSize);
		writer.WriteBytes(samples, sampleSize);
		outCooked = std::move(writer.Buffer());
		return true;
	}

	std::unique_ptr<SoundAsset> CreateSoundAsset(const std::vector<uint8_t>& cooked)
	{
		BinaryReader reader(cooked);
		uint32_t version = 0;
		uint32_t formatSize = 0;
		uint32_t sampleSize = 0;
		if (!reader.Read(version) || version != kSoundVersion || !reader.Read(formatSize))
		{
			SetError("サウンドのクック済みデータが壊れています");
			return nullptr;
		}
		const uint8_t* format = reader.Take(formatSize);
		if (format == nullptr || !reader.Read(sampleSize))
		{
			SetError("サウンドのクック済みデータが壊れています");
			return nullptr;
		}
		const uint8_t* samples = reader.Take(sampleSize);
		if (samples == nullptr)
		{
			SetError("サウンドのクック済みデータが壊れています");
			return nullptr;
		}

		auto asset = std::make_unique<SoundAsset>();
		asset->type = AssetType::Sound;
		asset->format.assign(format, format + formatSize);
		asset->samples.assign(samples, samples + sampleSize);
		asset->memoryBytes = static_cast<uint64_t>(formatSize) + sampleSize;
		return asset;
	}

	const std::vector<std::string>& SoundExtensions()
	{
		static const std::vector<std::string> extensions = { ".wav" };
		return extensions;
	}
}
