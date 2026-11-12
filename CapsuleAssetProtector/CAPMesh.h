#pragma once

// =============================================
// CAPMesh
// =============================================
// メッシュのクック(モデルファイル→CAP標準頂点の配列)と、VRAMへの展開を行います(内部専用)。
// モデルの読み込みにはassimpを使います(CAPのDLL内部に静的リンクされ、利用側には見えません)。
//
// ・静的メッシュとして読み込みます(ノードの変換は頂点に焼き込む)。スキニングは未対応です。
// ・座標系は左手系・UVのV反転・時計回りの面を表とする、DirectXに合わせた形に変換します。
// ・マテリアルのベースカラー/法線テクスチャのパスを部分メッシュごとに保存します。
//   モデルに埋め込まれたテクスチャは "モデルのパス#テクスチャ名" という仮想パスでパックへ一緒に保存します。
// =============================================

#include "CAPAsset.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace CapsuleAssetProtector
{
	struct Vertex;
}

namespace cap
{
	/**
	 * @brief クック中に追加のアセット(埋め込みテクスチャ等)をパックへ保存するための関数
	 */
	using AddExtraAsset = std::function<bool(const std::string& virtualPath, AssetType type, const std::vector<uint8_t>& cooked)>;

	/**
	 * @brief モデルファイルを読み込み、クック済みデータを作る
	 */
	bool CookMesh(const std::string& path, const AddExtraAsset& addExtra, std::vector<uint8_t>& outCooked);

	/**
	 * @brief クック済みデータからVRAM上のメッシュを作る
	 */
	std::unique_ptr<MeshAsset> CreateMeshAsset(CAPGpu& gpu, const std::vector<uint8_t>& cooked);

	/**
	 * @brief メモリ上の頂点・インデックスからメッシュを作る(CreateMesh用)
	 */
	std::unique_ptr<MeshAsset> CreateMeshFromData(CAPGpu& gpu, const CapsuleAssetProtector::Vertex* vertices, uint32_t vertexCount,
		const uint32_t* indices, uint32_t indexCount);

	/**
	 * @brief メッシュとして読み込める拡張子の一覧
	 */
	const std::vector<std::string>& MeshExtensions();
}
