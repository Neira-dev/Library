#include "CAPPack.h"

#include <windows.h>
#include <filesystem>

namespace cap
{
	namespace
	{
		// 無駄な領域がこれより小さいうちは詰め直さない
		constexpr uint64_t kCompactMinBytes = 4ull * 1024 * 1024;
	}

	CAPPack::~CAPPack()
	{
		Close();
	}

	bool CAPPack::Open(const std::string& path)
	{
		Close();
		_path = ToWide(path);

		std::error_code ec;
		if (!std::filesystem::exists(_path, ec))
		{
			// 新規作成: 空のヘッダーだけを書いておく
			std::ofstream create(_path, std::ios::binary | std::ios::trunc);
			if (!create)
			{
				SetError("パックファイルを作成できません: " + path);
				return false;
			}
			Header header = {};
			memcpy(header.magic, "CPAK", 4);
			header.version = kVersion;
			create.write(reinterpret_cast<const char*>(&header), sizeof(header));
		}

		_file.open(_path, std::ios::in | std::ios::out | std::ios::binary);
		if (!_file)
		{
			SetError("パックファイルを開けません: " + path);
			return false;
		}

		_file.seekg(0, std::ios::end);
		const uint64_t fileSize = static_cast<uint64_t>(_file.tellg());
		_file.seekg(0, std::ios::beg);

		Header header = {};
		_file.read(reinterpret_cast<char*>(&header), sizeof(header));
		if (!_file || memcmp(header.magic, "CPAK", 4) != 0 || header.version != kVersion)
		{
			SetError("パックファイルの形式が正しくありません: " + path);
			_file.close();
			return false;
		}

		_entries.clear();
		_liveBytes = 0;
		if (header.entryCount > 0)
		{
			const uint64_t tableSize = static_cast<uint64_t>(header.entryCount) * sizeof(Entry);
			if (header.tableOffset < sizeof(Header) || header.tableOffset + tableSize > fileSize)
			{
				SetError("パックファイルのテーブルが壊れています: " + path);
				_file.close();
				return false;
			}

			std::vector<Entry> table(header.entryCount);
			_file.seekg(static_cast<std::streamoff>(header.tableOffset));
			_file.read(reinterpret_cast<char*>(table.data()), static_cast<std::streamsize>(tableSize));
			if (!_file)
			{
				SetError("パックファイルのテーブルを読み込めません: " + path);
				_file.close();
				return false;
			}

			for (const Entry& entry : table)
			{
				if (entry.offset + entry.size > fileSize)
				{
					continue; // 範囲外のエントリは無視する
				}
				_entries[entry.key] = entry;
				_liveBytes += entry.size;
			}
		}

		// 新しいデータは常に末尾(現在のテーブルより後ろ)へ追記する
		_fileEnd = fileSize;
		_open = true;
		_dirty = false;
		return true;
	}

	void CAPPack::Close()
	{
		if (!_open)
		{
			return;
		}
		if (_dirty)
		{
			Save();
		}
		_file.close();
		_entries.clear();
		_open = false;
		_dirty = false;
	}

	bool CAPPack::GetInfo(const AssetKey& key, AssetType& outType, uint64_t& outStamp) const
	{
		auto it = _entries.find(key);
		if (it == _entries.end())
		{
			return false;
		}
		outType = static_cast<AssetType>(it->second.type);
		outStamp = it->second.stamp;
		return true;
	}

	bool CAPPack::Read(const AssetKey& key, const CAPCrypto& crypto, std::vector<uint8_t>& outData)
	{
		auto it = _entries.find(key);
		if (!_open || it == _entries.end())
		{
			return false;
		}
		const Entry& entry = it->second;

		std::vector<uint8_t> stored(static_cast<size_t>(entry.size));
		_file.clear();
		_file.seekg(static_cast<std::streamoff>(entry.offset));
		_file.read(reinterpret_cast<char*>(stored.data()), static_cast<std::streamsize>(entry.size));
		if (!_file)
		{
			SetError("パックからデータを読み込めません");
			return false;
		}

		if ((entry.flags & kFlagEncrypted) == 0)
		{
			outData = std::move(stored);
			return true;
		}

		if (!crypto.HasKey())
		{
			SetError("暗号化されたアセットを読むには SetKey で鍵を設定してください");
			return false;
		}
		if (!crypto.Decrypt(stored, outData))
		{
			SetError("アセットの復号に失敗しました(鍵が違うか、パックが壊れています)");
			return false;
		}
		return true;
	}

	bool CAPPack::Write(const AssetKey& key, AssetType type, uint64_t stamp, const std::vector<uint8_t>& data, const CAPCrypto& crypto)
	{
		if (!_open)
		{
			return false;
		}

		std::vector<uint8_t> encrypted;
		const std::vector<uint8_t>* stored = &data;
		uint32_t flags = 0;
		if (crypto.HasKey())
		{
			if (!crypto.Encrypt(data, encrypted))
			{
				SetError("アセットの暗号化に失敗しました");
				return false;
			}
			stored = &encrypted;
			flags |= kFlagEncrypted;
		}

		_file.clear();
		_file.seekp(static_cast<std::streamoff>(_fileEnd));
		_file.write(reinterpret_cast<const char*>(stored->data()), static_cast<std::streamsize>(stored->size()));
		_file.flush();
		if (!_file)
		{
			SetError("パックへの書き込みに失敗しました");
			return false;
		}

		Entry entry = {};
		entry.key = key;
		entry.type = static_cast<uint32_t>(type);
		entry.flags = flags;
		entry.offset = _fileEnd;
		entry.size = stored->size();
		entry.stamp = stamp;

		// 同じキーの古いデータは無効(無駄な領域)になる
		auto it = _entries.find(key);
		if (it != _entries.end())
		{
			_liveBytes -= it->second.size;
		}
		_entries[key] = entry;
		_liveBytes += entry.size;
		_fileEnd += entry.size;
		_dirty = true;
		return true;
	}

	bool CAPPack::WriteTableAndHeader(std::ostream& stream, uint64_t tableOffset, const std::map<AssetKey, Entry>& entries)
	{
		stream.seekp(static_cast<std::streamoff>(tableOffset));
		for (const auto& pair : entries)
		{
			stream.write(reinterpret_cast<const char*>(&pair.second), sizeof(Entry));
		}

		// テーブルを書き終えてから最後にヘッダーを書き換える(途中で落ちても古いテーブルが有効なまま)
		Header header = {};
		memcpy(header.magic, "CPAK", 4);
		header.version = kVersion;
		header.tableOffset = tableOffset;
		header.entryCount = static_cast<uint32_t>(entries.size());
		stream.seekp(0);
		stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
		stream.flush();
		return static_cast<bool>(stream);
	}

	bool CAPPack::Save()
	{
		if (!_open || !_dirty)
		{
			return true;
		}

		// 無駄な領域(上書きされた古いデータ・古いテーブル)が多ければ詰め直す
		const uint64_t dataBytes = _fileEnd - sizeof(Header);
		if (dataBytes > kCompactMinBytes && _liveBytes * 2 < dataBytes)
		{
			return Compact();
		}

		_file.clear();
		const uint64_t tableOffset = _fileEnd;
		if (!WriteTableAndHeader(_file, tableOffset, _entries))
		{
			SetError("パックの保存に失敗しました");
			return false;
		}

		// 次の追記は今書いたテーブルより後ろから行う(今のテーブルを壊さないため)
		_fileEnd = tableOffset + static_cast<uint64_t>(_entries.size()) * sizeof(Entry);
		_dirty = false;
		return true;
	}

	bool CAPPack::Compact()
	{
		const std::wstring tempPath = _path + L".tmp";
		std::map<AssetKey, Entry> newEntries = _entries;
		uint64_t position = sizeof(Header);

		{
			std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
			if (!out)
			{
				SetError("パックの詰め直し用ファイルを作成できません");
				return false;
			}

			// ヘッダーの場所を空けておく(最後に書く)
			Header placeholder = {};
			out.write(reinterpret_cast<const char*>(&placeholder), sizeof(placeholder));

			std::vector<uint8_t> buffer;
			for (auto& pair : newEntries)
			{
				Entry& entry = pair.second;
				buffer.resize(static_cast<size_t>(entry.size));
				_file.clear();
				_file.seekg(static_cast<std::streamoff>(entry.offset));
				_file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(entry.size));
				if (!_file)
				{
					SetError("パックの詰め直し中に読み込みに失敗しました");
					return false;
				}
				out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
				entry.offset = position;
				position += entry.size;
			}

			if (!WriteTableAndHeader(out, position, newEntries))
			{
				SetError("パックの詰め直しに失敗しました");
				return false;
			}
		}

		// 新しいファイルで置き換える
		_file.close();
		if (!MoveFileExW(tempPath.c_str(), _path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			SetError("パックの置き換えに失敗しました");
			_file.open(_path, std::ios::in | std::ios::out | std::ios::binary);
			return false;
		}

		_file.open(_path, std::ios::in | std::ios::out | std::ios::binary);
		if (!_file)
		{
			SetError("詰め直したパックを開けません");
			_open = false;
			return false;
		}

		_entries = std::move(newEntries);
		_fileEnd = position + static_cast<uint64_t>(_entries.size()) * sizeof(Entry);
		_dirty = false;
		return true;
	}
}
