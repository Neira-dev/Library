#pragma once

// =============================================
// CAPUtil
// =============================================
// CAP内部で共通して使う小さな道具をまとめたファイルです。
// ・アセットを識別するキー(パスを正規化してハッシュ化したもの)
// ・文字コード変換(引数のconst char*はACP、assimp等はUTF-8を使うため)
// ・クック済みデータを作る/読むためのバイナリライター/リーダー
// ・エラーメッセージの保持(GetLastErrorMessageで外から取得できる)
// =============================================

#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace cap
{
	/**
	 * @brief アセットを識別する128bitのキー
	 * @details パスを正規化(大文字小文字・区切り文字・"./"等の違いを吸収)してからハッシュ化する。
	 *          同じファイルを指していれば表記が違っても同じキーになる。
	 */
	struct AssetKey
	{
		uint64_t hi = 0;
		uint64_t lo = 0;
		bool operator==(const AssetKey& o) const { return hi == o.hi && lo == o.lo; }
		bool operator<(const AssetKey& o) const { return hi != o.hi ? hi < o.hi : lo < o.lo; }
	};

	struct AssetKeyHash
	{
		size_t operator()(const AssetKey& k) const { return static_cast<size_t>(k.hi ^ (k.lo * 0x9E3779B97F4A7C15ull)); }
	};

	/**
	 * @brief アセットの種類(パック内にも保存される)
	 */
	enum class AssetType : uint32_t
	{
		Unknown = 0,
		Texture = 1,
		Shader = 2,
		Mesh = 3,
		Sound = 4,
	};

	// ---------------------------------------------
	// 文字コード変換
	// ---------------------------------------------
	std::wstring ToWide(const std::string& acp);       // ACP(Shift-JIS等) -> UTF-16
	std::string ToNarrow(const std::wstring& wide);    // UTF-16 -> ACP
	std::string ToUtf8(const std::wstring& wide);      // UTF-16 -> UTF-8
	std::wstring FromUtf8(const std::string& utf8);    // UTF-8 -> UTF-16

	// ---------------------------------------------
	// パス
	// ---------------------------------------------
	/**
	 * @brief パスを正規化する(区切り文字を'/'に統一・小文字化・"."や".."を解決)
	 * @details マルチバイト文字を壊さないよう、一度UTF-16にしてから処理する
	 */
	std::string NormalizePath(const std::string& path);

	/**
	 * @brief パスからアセットキーを作る(内部で正規化する)
	 */
	AssetKey MakeAssetKey(const std::string& path);

	/**
	 * @brief 拡張子を小文字で取得する(例: ".png")。仮想パス(#を含む)の場合は空文字列
	 */
	std::string GetExtension(const std::string& path);

	/**
	 * @brief 仮想パス(モデルに埋め込まれたテクスチャ等、ディスク上に実体が無いもの)かどうか
	 * @details "model.fbx#texture" のように'#'を含むものを仮想パスとして扱う
	 */
	bool IsVirtualPath(const std::string& path);

	/**
	 * @brief ファイルの中身をすべて読み込む
	 */
	bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out);

	/**
	 * @brief 元ファイルの更新状態を表す値(更新日時とサイズから作る)を取得する
	 * @return ファイルが存在しない場合はfalse
	 */
	bool GetSourceStamp(const std::string& path, uint64_t& outStamp);

	// ---------------------------------------------
	// エラー
	// ---------------------------------------------
	/**
	 * @brief 最後に起きたエラーを記録する(デバッグ出力にも出す)
	 */
	void SetError(const std::string& message);

	/**
	 * @brief 最後に起きたエラーを取得する
	 */
	const char* GetError();

	// ---------------------------------------------
	// クック済みデータの書き込み/読み込み
	// ---------------------------------------------
	class BinaryWriter
	{
	public:
		template <typename T>
		void Write(const T& value)
		{
			static_assert(std::is_trivially_copyable_v<T>, "BinaryWriter::Write はそのままメモリコピーできる型のみ");
			WriteBytes(&value, sizeof(T));
		}

		void WriteBytes(const void* data, size_t size)
		{
			if (size == 0) return;
			const uint8_t* p = static_cast<const uint8_t*>(data);
			_buffer.insert(_buffer.end(), p, p + size);
		}

		void WriteString(const std::string& text)
		{
			Write<uint32_t>(static_cast<uint32_t>(text.size()));
			WriteBytes(text.data(), text.size());
		}

		std::vector<uint8_t>& Buffer() { return _buffer; }

	private:
		std::vector<uint8_t> _buffer;
	};

	class BinaryReader
	{
	public:
		explicit BinaryReader(const std::vector<uint8_t>& data) : _data(data.data()), _size(data.size()) {}

		template <typename T>
		bool Read(T& value)
		{
			static_assert(std::is_trivially_copyable_v<T>, "BinaryReader::Read はそのままメモリコピーできる型のみ");
			return ReadBytes(&value, sizeof(T));
		}

		bool ReadBytes(void* out, size_t size)
		{
			if (size > _size - _pos) return false;
			if (size > 0) memcpy(out, _data + _pos, size);
			_pos += size;
			return true;
		}

		bool ReadString(std::string& out)
		{
			uint32_t length = 0;
			if (!Read(length)) return false;
			if (length > _size - _pos) return false;
			out.assign(reinterpret_cast<const char*>(_data + _pos), length);
			_pos += length;
			return true;
		}

		/**
		 * @brief 現在位置のポインタを返し、size分読み進める(コピーせずに参照したい時に使う)
		 */
		const uint8_t* Take(size_t size)
		{
			if (size > _size - _pos) return nullptr;
			const uint8_t* p = _data + _pos;
			_pos += size;
			return p;
		}

	private:
		const uint8_t* _data = nullptr;
		size_t _size = 0;
		size_t _pos = 0;
	};
}
