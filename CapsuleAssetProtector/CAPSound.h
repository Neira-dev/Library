#pragma once

// =============================================
// CAPSound
// =============================================
// サウンド(WAV)のクックと展開を行います(内部専用)。
// WAVファイルからフォーマット情報(fmtチャンク)とPCMデータ(dataチャンク)を取り出して保存します。
// 取り出したデータはXAudio2等にそのまま渡せます。
// =============================================

#include "CAPAsset.h"

#include <memory>
#include <string>
#include <vector>

namespace cap
{
	/**
	 * @brief WAVファイルを読み込み、クック済みデータを作る
	 */
	bool CookSound(const std::string& path, std::vector<uint8_t>& outCooked);

	/**
	 * @brief クック済みデータからサウンドを作る
	 */
	std::unique_ptr<SoundAsset> CreateSoundAsset(const std::vector<uint8_t>& cooked);

	/**
	 * @brief サウンドとして読み込める拡張子の一覧
	 */
	const std::vector<std::string>& SoundExtensions();
}
