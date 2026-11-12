#pragma once

// =============================================
// CAPTexture
// =============================================
// テクスチャのクック(元画像→ミップ付きの生ピクセル)と、VRAMへの展開を行います(内部専用)。
// 画像のデコードにはWindows標準のWIC(Windows Imaging Component)を使うため、外部ライブラリは不要です。
// ミップマップはボックスフィルタ(2x2の平均)で生成します。
// =============================================

#include "CAPAsset.h"

#include <memory>
#include <string>
#include <vector>

namespace cap
{
	/**
	 * @brief 画像ファイルをデコードし、クック済みデータを作る
	 */
	bool CookTextureFromFile(const std::string& path, std::vector<uint8_t>& outCooked);

	/**
	 * @brief メモリ上の圧縮画像(png/jpg等のバイト列)からクック済みデータを作る(モデル埋め込みテクスチャ用)
	 */
	bool CookTextureFromMemory(const void* data, size_t size, std::vector<uint8_t>& outCooked);

	/**
	 * @brief RGBA8の生ピクセルからクック済みデータを作る
	 */
	bool CookTextureFromRgba(uint32_t width, uint32_t height, const uint8_t* rgba, std::vector<uint8_t>& outCooked);

	/**
	 * @brief クック済みデータからVRAM上のテクスチャとSRVを作る
	 */
	std::unique_ptr<TextureAsset> CreateTextureAsset(CAPGpu& gpu, const std::vector<uint8_t>& cooked);

	/**
	 * @brief 1x1の単色テクスチャを作る(テクスチャが無い時の既定値に使う)
	 */
	std::unique_ptr<TextureAsset> CreateSolidTexture(CAPGpu& gpu, uint8_t r, uint8_t g, uint8_t b, uint8_t a);

	/**
	 * @brief テクスチャとして読み込める拡張子の一覧
	 */
	const std::vector<std::string>& TextureExtensions();
}
