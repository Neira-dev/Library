#pragma once

// =============================================
// CAPShader
// =============================================
// シェーダーのクック(HLSL→バイトコード)と、パイプライン(ルートシグネチャ+PSO)の自動作成を行います(内部専用)。
//
// ・シェーダーの種類はファイル名から判定します(VS_xxx / xxx_VS = 頂点, PS_xxx / xxx_PS = ピクセル)。
//   判定できない名前の場合は、頂点→ピクセルの順にコンパイルを試します。
// ・ルートシグネチャは頂点/ピクセル両方のリフレクションから作ります。
//     cbuffer          → ルートCBV
//     Texture2D等      → 1つずつディスクリプタテーブル
//     SamplerState     → 静的サンプラー(名前にpoint/aniso/clamp/mirrorを含むと設定が変わる)
//   両方のシェーダーで同じレジスタを使っている場合は1つにまとめます(可視性ALL)。
// ・頂点レイアウトは、頂点シェーダーの入力セマンティクスをCAPの標準頂点から選んで作ります。
// d3dx12.hは使いません。
// =============================================

#include "CAPAsset.h"

#include <memory>
#include <string>
#include <vector>

namespace cap
{
	/**
	 * @brief HLSLファイルをコンパイルし、クック済みデータ(種類+バイトコード)を作る
	 */
	bool CookShader(const std::string& path, std::vector<uint8_t>& outCooked);

	/**
	 * @brief クック済みデータからシェーダーを作る
	 */
	std::unique_ptr<ShaderAsset> CreateShaderAsset(const std::vector<uint8_t>& cooked);

	/**
	 * @brief パイプラインのスロットの種類
	 */
	enum class SlotKind
	{
		ConstantBuffer,
		Texture,
	};

	/**
	 * @brief テクスチャスロットの役割(メッシュ描画時の自動割り当てに使う)
	 */
	enum class TextureRole
	{
		Other,   // 自動では割り当てない(SetTextureで設定する)
		Diffuse, // ベースカラー
		Normal,  // 法線マップ
	};

	/**
	 * @brief シェーダーのリソース1つ分(名前で指定できるようにするための対応表)
	 */
	struct PipelineSlot
	{
		std::string name;
		SlotKind kind = SlotKind::ConstantBuffer;
		uint32_t rootIndex = 0;
		TextureRole role = TextureRole::Other;
	};

	/**
	 * @brief パイプラインの作成設定
	 */
	struct PipelineOptions
	{
		bool alphaBlend = false;
		bool depthTest = true;
		bool depthWrite = true;
		D3D12_CULL_MODE cullMode = D3D12_CULL_MODE_BACK;
		bool wireframe = false;
		DXGI_FORMAT rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
		DXGI_FORMAT dsvFormat = DXGI_FORMAT_D32_FLOAT;
	};

	/**
	 * @brief 作成済みのパイプライン
	 */
	struct Pipeline
	{
		ComPtr<ID3D12RootSignature> rootSignature;
		ComPtr<ID3D12PipelineState> pipelineState;
		std::vector<PipelineSlot> slots;
	};

	/**
	 * @brief 頂点/ピクセルシェーダーをリフレクションし、ルートシグネチャ・頂点レイアウト・PSOを作る
	 */
	bool BuildPipeline(ID3D12Device* device, const ShaderAsset& vertexShader, const ShaderAsset& pixelShader,
		const PipelineOptions& options, Pipeline& outPipeline);
}
