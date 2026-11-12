#pragma once

// =============================================
// CAPCrypto
// =============================================
// アセットの暗号化・復号を担当する内部クラスです。
// Windows標準のBCrypt APIを使うため、外部の暗号ライブラリ依存はありません。
// パスフレーズ(SetKeyで渡される文字列)をSHA-256で32バイトの鍵に変換し、
// AES-256-CBCで暗号化します。IV(初期化ベクトル)は暗号化のたびにランダム生成し、
// 暗号文の先頭16バイトとして一緒に保存します(復号時に取り出して使う)。
// このクラスはCAP内部でのみ使用するため、DLLの外へは公開しません。
// =============================================

#include <vector>
#include <string>
#include <cstdint>

namespace cap
{
	class CAPCrypto
	{
	public:
		CAPCrypto() = default;
		~CAPCrypto() = default;

		/**
		 * @brief パスフレーズから暗号鍵を設定する(SHA-256で32バイトに変換する)
		 * @param key パスフレーズ文字列
		 */
		void SetKey(const std::string& key);

		/**
		 * @brief 鍵が設定済みかどうか
		 */
		bool HasKey() const { return _hasKey; }

		/**
		 * @brief 平文を暗号化する(出力は先頭16バイトのIV + 暗号本体)
		 * @param plain 平文データ
		 * @param outCipher 暗号化結果の出力先
		 * @return 成功したらtrue
		 */
		bool Encrypt(const std::vector<uint8_t>& plain, std::vector<uint8_t>& outCipher) const;

		/**
		 * @brief 暗号文を復号する(先頭16バイトをIVとして取り出して使う)
		 * @param cipher 暗号化データ(IV + 暗号本体)
		 * @param outPlain 復号結果の出力先
		 * @return 成功したらtrue
		 */
		bool Decrypt(const std::vector<uint8_t>& cipher, std::vector<uint8_t>& outPlain) const;

	private:
		// SHA-256で導出した32バイトのAES-256鍵
		uint8_t _key[32] = {};
		bool _hasKey = false;
	};
}
