#pragma once
#include <string>
#include <vector>
#include <map>
#include <d3d12.h>

class TextureManager
{
public:
	struct SubResourceData
	{
		// RGBAを格納するための構造体
		std::vector<uint8_t> pixels;
		// 横 * 4 サイズを格納するための変数(例:解像度1920 * 4 = 7680)
		UINT rowPitch = 0;
		// 縦 * 横 * 4 サイズを格納するための変数(例:解像度1920 * 1080 * 4 = 8294400)
		UINT slicePitch = 0;
	};

	struct TextureCpuData
	{
		// テクスチャの元解像度の幅
		UINT width = 0;
		// テクスチャの元解像度の高さ
		UINT height = 0;
		// テクスチャのミップマップレベル数
		UINT mipLevels = 1;
		// テクスチャの配列サイズ
		UINT arraySize = 1;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		// テクスチャのサブリソースデータを格納するための配列
		std::vector<SubResourceData> subResources;
	};

public: // Singleton pattern
	static TextureManager* Singleton();
	static void DestroySingleton();
public:

	/**
	 * @brief　TextureDataを追加する 
	 * @param Name 登録名
	 * @param InTexturedata テクスチャデータ
	 * @return 追加結果
	 */
	bool AddTextureData(std::string Name, TextureCpuData InTexturedata);



protected:
	// テクスチャのCPU側データを格納するためのマップ
	std::map<std::string, TextureCpuData> _textureDataMap;
private:
	TextureManager() = default;
	~TextureManager() = default;
private:
	static TextureManager* _instance;
};

