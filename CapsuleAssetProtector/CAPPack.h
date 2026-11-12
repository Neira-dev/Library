#pragma once

// =============================================
// CAPPack
// =============================================
// クック済み(高速に読める形に変換済み)のアセットを1つのファイル(.cpak)にまとめて保存する内部クラスです。
// 各アセットは暗号化(鍵が設定されている場合)してから格納します。
//
// ファイル構造:
//   [Header 32バイト] [データ本体...] [エントリテーブル]
//
// 新しいデータは常にファイルの末尾へ追記し、最後にテーブルとヘッダーを書き直します。
// テーブルを書き直す前に強制終了しても、ヘッダーは古い(壊れていない)テーブルを指したままなので
// パックが壊れることはありません(追記したデータが無駄になるだけ)。
// 無駄な領域が増えすぎたら、保存時に自動で詰め直します。
// =============================================

#include "CAPUtil.h"
#include "CAPCrypto.h"

#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace cap
{
	class CAPPack
	{
	public:
		CAPPack() = default;
		~CAPPack();

		/**
		 * @brief パックファイルを開く(存在しなければ新規作成する)
		 * @param path パックファイルのパス
		 */
		bool Open(const std::string& path);

		/**
		 * @brief パックファイルを閉じる(未保存の変更があれば保存する)
		 */
		void Close();

		/**
		 * @brief 指定キーのアセットがパックに入っているか、入っていれば種類と元ファイルの更新状態を返す
		 */
		bool GetInfo(const AssetKey& key, AssetType& outType, uint64_t& outStamp) const;

		/**
		 * @brief パックからクック済みデータを読み込む(必要なら復号する)
		 */
		bool Read(const AssetKey& key, const CAPCrypto& crypto, std::vector<uint8_t>& outData);

		/**
		 * @brief クック済みデータをパックへ追記する(鍵が設定されていれば暗号化する)
		 * @param stamp 元ファイルの更新状態(元ファイルが変わったら作り直すために保存する)
		 */
		bool Write(const AssetKey& key, AssetType type, uint64_t stamp, const std::vector<uint8_t>& data, const CAPCrypto& crypto);

		/**
		 * @brief エントリテーブルとヘッダーを書き込み、変更を確定させる
		 */
		bool Save();

		bool IsOpen() const { return _open; }
		bool IsDirty() const { return _dirty; }

	private:
		// ファイル先頭のヘッダー
		struct Header
		{
			char magic[4];         // "CPAK"
			uint32_t version;      // フォーマットのバージョン
			uint64_t tableOffset;  // エントリテーブルの位置
			uint32_t entryCount;   // エントリ数
			uint32_t reserved;
			uint64_t reserved2;
		};

		// 1アセット分のエントリ
		struct Entry
		{
			AssetKey key;
			uint32_t type;    // AssetType
			uint32_t flags;   // kFlagEncrypted 等
			uint64_t offset;  // データ本体の位置
			uint64_t size;    // データ本体のサイズ(暗号化後)
			uint64_t stamp;   // 元ファイルの更新状態
		};

		static_assert(sizeof(Header) == 32, "Headerのサイズが想定と違います");
		static_assert(sizeof(Entry) == 48, "Entryのサイズが想定と違います");

		static constexpr uint32_t kVersion = 1;
		static constexpr uint32_t kFlagEncrypted = 1u << 0;

		bool WriteTableAndHeader(std::ostream& stream, uint64_t tableOffset, const std::map<AssetKey, Entry>& entries);
		bool Compact();

	private:
		std::wstring _path;
		std::fstream _file;
		std::map<AssetKey, Entry> _entries;
		uint64_t _fileEnd = 0;    // 次にデータを追記する位置
		uint64_t _liveBytes = 0;  // 現在有効なデータの合計サイズ
		bool _open = false;
		bool _dirty = false;
	};
}
