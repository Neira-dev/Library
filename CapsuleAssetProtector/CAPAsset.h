#pragma once

// =============================================
// CAPAsset
// =============================================
// CAPがメモリ/VRAM上に展開したアセットの型をまとめたファイルです(内部専用)。
// どのアセットも Release() でGPUリソースを遅延解放に回してから破棄されます。
// =============================================

#include "CAPUtil.h"
#include "CAPGpu.h"

#include <string>
#include <vector>

namespace cap
{
	/**
	 * @brief 展開済みアセットの基底
	 */
	struct Asset
	{
		virtual ~Asset() = default;

		AssetType type = AssetType::Unknown;
		// 常駐しているメモリ/VRAMの概算(メモリ予算の判定に使う)
		uint64_t memoryBytes = 0;

		/**
		 * @brief GPUリソースを遅延解放に回す(GPUが使い終わってから解放される)
		 */
		virtual void Release(CAPGpu& gpu) { (void)gpu; }
	};

	/**
	 * @brief テクスチャ(VRAM上のテクスチャとCAPのヒープ内のSRV)
	 */
	struct TextureAsset : Asset
	{
		ComPtr<ID3D12Resource> texture;
		uint32_t srvIndex = UINT32_MAX;
		uint32_t width = 0;
		uint32_t height = 0;

		void Release(CAPGpu& gpu) override
		{
			gpu.ReleaseLater(texture.Get());
			gpu.ReleaseSrv(srvIndex);
			texture.Reset();
			srvIndex = UINT32_MAX;
		}
	};

	/**
	 * @brief シェーダーの種類
	 */
	enum class ShaderStage : uint32_t
	{
		Vertex = 0,
		Pixel = 1,
	};

	/**
	 * @brief コンパイル済みシェーダー(パイプライン作成時にリフレクションして使う)
	 */
	struct ShaderAsset : Asset
	{
		ShaderStage stage = ShaderStage::Vertex;
		std::vector<uint8_t> bytecode;
	};

	/**
	 * @brief メッシュを構成する部分メッシュ(マテリアルごとに分かれる)
	 */
	struct SubMesh
	{
		std::string name;
		uint32_t startIndex = 0;
		uint32_t indexCount = 0;
		int32_t baseVertex = 0;
		std::string diffusePath; // 空ならテクスチャ無し(既定テクスチャを使う)
		std::string normalPath;
	};

	/**
	 * @brief メッシュ(全部分メッシュの頂点・インデックスを1本ずつのバッファにまとめて持つ)
	 */
	struct MeshAsset : Asset
	{
		ComPtr<ID3D12Resource> vertexBuffer;
		ComPtr<ID3D12Resource> indexBuffer;
		D3D12_VERTEX_BUFFER_VIEW vertexBufferView = {};
		D3D12_INDEX_BUFFER_VIEW indexBufferView = {};
		std::vector<SubMesh> subMeshes;

		void Release(CAPGpu& gpu) override
		{
			gpu.ReleaseLater(vertexBuffer.Get());
			gpu.ReleaseLater(indexBuffer.Get());
			vertexBuffer.Reset();
			indexBuffer.Reset();
		}
	};

	/**
	 * @brief サウンド(WAVのフォーマット情報とPCMデータ)
	 */
	struct SoundAsset : Asset
	{
		std::vector<uint8_t> format;  // WAVEFORMATEX(またはWAVEFORMATEXTENSIBLE)のバイト列
		std::vector<uint8_t> samples; // PCMデータ
	};
}
