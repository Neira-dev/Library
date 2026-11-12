#pragma once

// =============================================
// CAPLoader
// =============================================
// CapsuleAssetProtectorの中核となるクラスです(外部へは公開しません)。
// ・拡張子ごとのロード処理を std::map<std::string, AssetHandler> で管理し、RegisterHandlerで動的に追加できる
// ・アセットの読み込み経路
//     パックに入っていて元ファイルが変わっていない → パックから読む(高速)
//     それ以外で元ファイルがある                   → クックしてパックへ追記してから使う
// ・メモリ常駐管理
//     Loadしたアセット(参照カウント>0)は常駐。
//     参照カウント0のアセットは、使われた(SetTexture/DrawMesh等)フレームから数フレームの間は残し、
//     メモリ予算を超えていれば古い順に解放する(GPUが使い終わるまでの待ちはCAPGpuが行う)。
// ・パイプライン(シェーダー)とメッシュの描画補助
// =============================================

#include "CapsuleAssetProtector.h"
#include "CAPUtil.h"
#include "CAPCrypto.h"
#include "CAPPack.h"
#include "CAPGpu.h"
#include "CAPAsset.h"
#include "CAPShader.h"
#include "CAPMesh.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cap
{
	/**
	 * @brief 拡張子ごとのロード処理
	 */
	struct AssetHandler
	{
		AssetType type = AssetType::Unknown;
		// 元ファイルを読み込み、高速に読めるバイナリ(クック済みデータ)へ変換する
		std::function<bool(const std::string& sourcePath, const AddExtraAsset& addExtra, std::vector<uint8_t>& outCooked)> cook;
		// クック済みデータから、メモリ/VRAM上のアセットを作る
		std::function<std::unique_ptr<Asset>(const std::vector<uint8_t>& cooked)> create;
	};
}

class CAPLoader
{
public:
	CAPLoader() = default;
	~CAPLoader();
	CAPLoader(const CAPLoader&) = delete;
	CAPLoader& operator=(const CAPLoader&) = delete;

public:
	bool Initialize(const CapsuleAssetProtector::InitDesc& desc);
	void Shutdown();
	void SetKey(const std::string& key);

	/**
	 * @brief 拡張子(例: ".png")に対するロード処理を登録する(同じ拡張子は上書き)
	 */
	void RegisterHandler(const std::string& extension, const cap::AssetHandler& handler);

	// ---------------------------------------------
	// アセット
	// ---------------------------------------------
	bool Load(const std::string& path);
	void Unload(const std::string& path);
	bool IsLoaded(const std::string& path);
	void Update();
	void SetMemoryBudget(uint64_t bytes) { _memoryBudget = bytes; }
	uint64_t GetResidentBytes() const { return _residentBytes; }

	// ---------------------------------------------
	// パイプライン
	// ---------------------------------------------
	bool CreatePipeline(const std::string& name, const std::string& vertexShaderPath, const std::string& pixelShaderPath,
		const CapsuleAssetProtector::PipelineDesc& desc);
	void DestroyPipeline(const std::string& name);
	bool BindPipeline(ID3D12GraphicsCommandList* commandList, const std::string& name);
	void SetDescriptorHeap(ID3D12GraphicsCommandList* commandList);
	bool SetTexture(ID3D12GraphicsCommandList* commandList, const std::string& slotName, const std::string& texturePath);
	bool SetConstants(ID3D12GraphicsCommandList* commandList, const std::string& bufferName, const void* data, uint32_t size);
	bool SetConstantBuffer(ID3D12GraphicsCommandList* commandList, const std::string& bufferName, D3D12_GPU_VIRTUAL_ADDRESS address);

	// ---------------------------------------------
	// メッシュ
	// ---------------------------------------------
	bool CreateMesh(const std::string& name, const CapsuleAssetProtector::Vertex* vertices, uint32_t vertexCount,
		const uint32_t* indices, uint32_t indexCount);
	void DestroyMesh(const std::string& name);
	bool DrawMesh(ID3D12GraphicsCommandList* commandList, const std::string& meshPath);

	// ---------------------------------------------
	// 取得
	// ---------------------------------------------
	D3D12_GPU_DESCRIPTOR_HANDLE GetTextureHandle(const std::string& texturePath);
	ID3D12DescriptorHeap* GetDescriptorHeap() const { return _gpu.Heap(); }
	ID3D12PipelineState* GetPipelineState(const std::string& name);
	ID3D12RootSignature* GetRootSignature(const std::string& name);
	bool GetSound(const std::string& path, CapsuleAssetProtector::SoundData& outSound);

private:
	struct AssetEntry
	{
		std::string path;
		std::unique_ptr<cap::Asset> asset;
		uint32_t refCount = 0;      // Loadされた回数(0より大きい間は常駐)
		uint64_t lastUsedFrame = 0; // 最後に使われたフレーム
	};

	struct PipelineEntry
	{
		std::string vertexShaderPath;
		std::string pixelShaderPath;
		cap::Pipeline pipeline;
	};

	void RegisterDefaultHandlers();

	cap::AssetKey KeyOf(const std::string& path);
	AssetEntry* Find(const std::string& path);
	bool LoadInternal(const std::string& path, bool addRef);
	bool GetCookedData(const std::string& path, const cap::AssetKey& key, cap::AssetType& outType, std::vector<uint8_t>& outCooked);

	/**
	 * @brief アセットを使う(未読み込みなら自動で読み込み、使用フレームを更新する)
	 */
	cap::Asset* Use(const std::string& path);
	cap::TextureAsset* UseTexture(const std::string& path);

	void ReleaseEntry(AssetEntry& entry);
	void Evict();

	D3D12_GPU_DESCRIPTOR_HANDLE DefaultTextureHandle(cap::TextureRole role) const;
	const cap::PipelineSlot* FindSlot(const std::string& name, cap::SlotKind kind) const;
	static std::string MemoryMeshPath(const std::string& name) { return "#memory/" + name; }

private:
	bool _initialized = false;
	cap::CAPCrypto _crypto;
	cap::CAPPack _pack;
	cap::CAPGpu _gpu;

	// 拡張子 -> ロード処理(動的に追加できる)
	std::map<std::string, cap::AssetHandler> _handlers;
	// 種類 -> ロード処理(拡張子の無い仮想パスをパックから作る時に使う)
	std::map<cap::AssetType, cap::AssetHandler> _handlersByType;

	std::unordered_map<cap::AssetKey, AssetEntry, cap::AssetKeyHash> _assets;
	std::unordered_set<cap::AssetKey, cap::AssetKeyHash> _failed; // 自動読み込みで失敗したもの(毎フレーム再挑戦しない)
	std::unordered_map<std::string, cap::AssetKey> _keyCache;     // パス文字列 -> キー(正規化の負荷を減らす)

	std::unordered_map<std::string, PipelineEntry> _pipelines;
	PipelineEntry* _currentPipeline = nullptr;

	std::unique_ptr<cap::TextureAsset> _whiteTexture;
	std::unique_ptr<cap::TextureAsset> _flatNormalTexture;

	DXGI_FORMAT _rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	DXGI_FORMAT _dsvFormat = DXGI_FORMAT_D32_FLOAT;
	uint64_t _frame = 0;
	uint64_t _memoryBudget = 0;
	uint64_t _residentBytes = 0;
};
