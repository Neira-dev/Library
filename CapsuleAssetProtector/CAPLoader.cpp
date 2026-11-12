#include "CAPLoader.h"
#include "CAPTexture.h"
#include "CAPSound.h"

#include <algorithm>

namespace
{
	// 参照カウント0のアセットを、最後に使われてから最低でもこのフレーム数は残す(毎フレームの読み直しを防ぐ)
	constexpr uint64_t kEvictDelayFrames = 3;
}

CAPLoader::~CAPLoader()
{
	Shutdown();
}

bool CAPLoader::Initialize(const CapsuleAssetProtector::InitDesc& desc)
{
	if (_initialized)
	{
		Shutdown();
	}

	if (!_gpu.Initialize(desc.device, desc.commandQueue, desc.srvHeap, desc.allocateSrv, desc.freeSrv, desc.userData, desc.srvCapacity))
	{
		return false;
	}
	if (!_pack.Open(desc.packPath != nullptr ? desc.packPath : "Assets.cpak"))
	{
		_gpu.Shutdown();
		return false;
	}

	_rtvFormat = desc.rtvFormat;
	_dsvFormat = desc.dsvFormat;

	// テクスチャが無い・読めない時に代わりに使う既定テクスチャ
	_whiteTexture = cap::CreateSolidTexture(_gpu, 255, 255, 255, 255);
	_flatNormalTexture = cap::CreateSolidTexture(_gpu, 128, 128, 255, 255);
	if (!_whiteTexture || !_flatNormalTexture)
	{
		_pack.Close();
		_gpu.Shutdown();
		return false;
	}

	RegisterDefaultHandlers();
	_frame = 0;
	_initialized = true;
	return true;
}

void CAPLoader::Shutdown()
{
	if (!_initialized)
	{
		return;
	}

	// パイプライン・アセットのGPUリソースを遅延解放に回し、最後にGPUの完了を待ってまとめて解放する
	for (auto& pair : _pipelines)
	{
		_gpu.ReleaseLater(pair.second.pipeline.pipelineState.Get());
		_gpu.ReleaseLater(pair.second.pipeline.rootSignature.Get());
	}
	_pipelines.clear();
	_currentPipeline = nullptr;

	for (auto& pair : _assets)
	{
		ReleaseEntry(pair.second);
	}
	_assets.clear();
	_failed.clear();
	_keyCache.clear();

	if (_whiteTexture) _whiteTexture->Release(_gpu);
	if (_flatNormalTexture) _flatNormalTexture->Release(_gpu);
	_whiteTexture.reset();
	_flatNormalTexture.reset();

	_pack.Close();
	_gpu.Shutdown();

	_handlers.clear();
	_handlersByType.clear();
	_residentBytes = 0;
	_initialized = false;
}

void CAPLoader::SetKey(const std::string& key)
{
	_crypto.SetKey(key);
	// 鍵が変わると読めなかったアセットが読めるようになる可能性があるので、失敗記録を消す
	_failed.clear();
}

void CAPLoader::RegisterHandler(const std::string& extension, const cap::AssetHandler& handler)
{
	std::string ext = extension;
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
	if (!ext.empty() && ext[0] != '.')
	{
		ext.insert(ext.begin(), '.');
	}

	_handlers[ext] = handler;
	if (_handlersByType.find(handler.type) == _handlersByType.end())
	{
		_handlersByType[handler.type] = handler;
	}
}

void CAPLoader::RegisterDefaultHandlers()
{
	// テクスチャ
	cap::AssetHandler texture;
	texture.type = cap::AssetType::Texture;
	texture.cook = [](const std::string& source, const cap::AddExtraAsset&, std::vector<uint8_t>& out) {
		return cap::CookTextureFromFile(source, out);
	};
	texture.create = [this](const std::vector<uint8_t>& cooked) -> std::unique_ptr<cap::Asset> {
		return cap::CreateTextureAsset(_gpu, cooked);
	};
	for (const std::string& ext : cap::TextureExtensions())
	{
		RegisterHandler(ext, texture);
	}

	// シェーダー
	cap::AssetHandler shader;
	shader.type = cap::AssetType::Shader;
	shader.cook = [](const std::string& source, const cap::AddExtraAsset&, std::vector<uint8_t>& out) {
		return cap::CookShader(source, out);
	};
	shader.create = [](const std::vector<uint8_t>& cooked) -> std::unique_ptr<cap::Asset> {
		return cap::CreateShaderAsset(cooked);
	};
	RegisterHandler(".hlsl", shader);

	// メッシュ
	cap::AssetHandler mesh;
	mesh.type = cap::AssetType::Mesh;
	mesh.cook = [](const std::string& source, const cap::AddExtraAsset& addExtra, std::vector<uint8_t>& out) {
		return cap::CookMesh(source, addExtra, out);
	};
	mesh.create = [this](const std::vector<uint8_t>& cooked) -> std::unique_ptr<cap::Asset> {
		return cap::CreateMeshAsset(_gpu, cooked);
	};
	for (const std::string& ext : cap::MeshExtensions())
	{
		RegisterHandler(ext, mesh);
	}

	// サウンド
	cap::AssetHandler sound;
	sound.type = cap::AssetType::Sound;
	sound.cook = [](const std::string& source, const cap::AddExtraAsset&, std::vector<uint8_t>& out) {
		return cap::CookSound(source, out);
	};
	sound.create = [](const std::vector<uint8_t>& cooked) -> std::unique_ptr<cap::Asset> {
		return cap::CreateSoundAsset(cooked);
	};
	for (const std::string& ext : cap::SoundExtensions())
	{
		RegisterHandler(ext, sound);
	}
}

// ---------------------------------------------
// アセット
// ---------------------------------------------
cap::AssetKey CAPLoader::KeyOf(const std::string& path)
{
	auto it = _keyCache.find(path);
	if (it != _keyCache.end())
	{
		return it->second;
	}
	const cap::AssetKey key = cap::MakeAssetKey(path);
	_keyCache.emplace(path, key);
	return key;
}

CAPLoader::AssetEntry* CAPLoader::Find(const std::string& path)
{
	auto it = _assets.find(KeyOf(path));
	return (it != _assets.end()) ? &it->second : nullptr;
}

bool CAPLoader::GetCookedData(const std::string& path, const cap::AssetKey& key, cap::AssetType& outType, std::vector<uint8_t>& outCooked)
{
	uint64_t stamp = 0;
	const bool hasSource = cap::GetSourceStamp(path, stamp);

	cap::AssetType packType = cap::AssetType::Unknown;
	uint64_t packStamp = 0;
	const bool inPack = _pack.GetInfo(key, packType, packStamp);

	// パックにあり、元ファイルが無い(配布時)か変わっていなければパックから読む
	if (inPack && (!hasSource || packStamp == stamp))
	{
		if (_pack.Read(key, _crypto, outCooked))
		{
			outType = packType;
			return true;
		}
		if (!hasSource)
		{
			return false; // 読めず(鍵違い等)、作り直す元ファイルも無い
		}
		// 読めないが元ファイルがあるので、作り直してパックを上書きする
	}

	if (!hasSource)
	{
		cap::SetError("アセットが見つかりません(元ファイルもパック内にもありません): " + path);
		return false;
	}

	const std::string ext = cap::GetExtension(path);
	auto handler = _handlers.find(ext);
	if (handler == _handlers.end())
	{
		cap::SetError("未対応の拡張子です: " + path);
		return false;
	}

	// 埋め込みテクスチャ等はモデルと同じ更新状態でパックへ保存する
	const cap::AddExtraAsset addExtra = [this, stamp](const std::string& virtualPath, cap::AssetType type, const std::vector<uint8_t>& cooked) {
		return _pack.Write(KeyOf(virtualPath), type, stamp, cooked, _crypto);
	};
	if (!handler->second.cook(path, addExtra, outCooked))
	{
		return false;
	}

	// パックへの保存に失敗しても、今回の読み込み自体は続ける
	_pack.Write(key, handler->second.type, stamp, outCooked, _crypto);
	outType = handler->second.type;
	return true;
}

bool CAPLoader::LoadInternal(const std::string& path, bool addRef)
{
	if (!_initialized)
	{
		cap::SetError("CapsuleAssetProtector::Initialize が呼ばれていません");
		return false;
	}

	const cap::AssetKey key = KeyOf(path);
	auto it = _assets.find(key);
	if (it != _assets.end())
	{
		if (addRef)
		{
			++it->second.refCount;
		}
		it->second.lastUsedFrame = _frame;
		return true;
	}

	// 自動読み込みで一度失敗したものは毎フレーム再挑戦しない(明示的なLoadは再挑戦する)
	if (!addRef && _failed.count(key) > 0)
	{
		return false;
	}

	cap::AssetType type = cap::AssetType::Unknown;
	std::vector<uint8_t> cooked;
	if (!GetCookedData(path, key, type, cooked))
	{
		_failed.insert(key);
		return false;
	}

	auto handler = _handlersByType.find(type);
	if (handler == _handlersByType.end())
	{
		cap::SetError("アセットの種類に対応する処理がありません: " + path);
		_failed.insert(key);
		return false;
	}

	std::unique_ptr<cap::Asset> asset = handler->second.create(cooked);
	if (!asset)
	{
		_failed.insert(key);
		return false;
	}

	AssetEntry entry;
	entry.path = path;
	entry.refCount = addRef ? 1 : 0;
	entry.lastUsedFrame = _frame;
	_residentBytes += asset->memoryBytes;
	entry.asset = std::move(asset);
	_assets.emplace(key, std::move(entry));
	_failed.erase(key);
	return true;
}

bool CAPLoader::Load(const std::string& path)
{
	return LoadInternal(path, true);
}

void CAPLoader::Unload(const std::string& path)
{
	AssetEntry* entry = Find(path);
	if (entry != nullptr && entry->refCount > 0)
	{
		// 参照カウントが0になっても、すぐには解放しない(Updateで使用状況と予算を見て解放する)
		--entry->refCount;
	}
}

bool CAPLoader::IsLoaded(const std::string& path)
{
	return Find(path) != nullptr;
}

cap::Asset* CAPLoader::Use(const std::string& path)
{
	AssetEntry* entry = Find(path);
	if (entry == nullptr)
	{
		if (!LoadInternal(path, false))
		{
			return nullptr;
		}
		entry = Find(path);
		if (entry == nullptr)
		{
			return nullptr;
		}
	}
	entry->lastUsedFrame = _frame;
	return entry->asset.get();
}

cap::TextureAsset* CAPLoader::UseTexture(const std::string& path)
{
	if (path.empty())
	{
		return nullptr;
	}
	cap::Asset* asset = Use(path);
	if (asset == nullptr || asset->type != cap::AssetType::Texture)
	{
		return nullptr;
	}
	return static_cast<cap::TextureAsset*>(asset);
}

void CAPLoader::ReleaseEntry(AssetEntry& entry)
{
	if (entry.asset)
	{
		_residentBytes -= (std::min)(_residentBytes, entry.asset->memoryBytes);
		entry.asset->Release(_gpu);
		entry.asset.reset();
	}
}

void CAPLoader::Evict()
{
	if (_residentBytes <= _memoryBudget)
	{
		return;
	}

	// 参照カウント0で、しばらく使われていないものを古い順に解放する
	std::vector<std::pair<uint64_t, cap::AssetKey>> candidates;
	for (const auto& pair : _assets)
	{
		const AssetEntry& entry = pair.second;
		if (entry.refCount == 0 && _frame - entry.lastUsedFrame >= kEvictDelayFrames)
		{
			candidates.emplace_back(entry.lastUsedFrame, pair.first);
		}
	}
	std::sort(candidates.begin(), candidates.end(),
		[](const auto& a, const auto& b) { return a.first < b.first; });

	for (const auto& candidate : candidates)
	{
		if (_residentBytes <= _memoryBudget)
		{
			break;
		}
		auto it = _assets.find(candidate.second);
		if (it == _assets.end())
		{
			continue;
		}
		ReleaseEntry(it->second);
		_assets.erase(it);
	}
}

void CAPLoader::Update()
{
	if (!_initialized)
	{
		return;
	}

	// 利用側が送信した今フレームのコマンドの後ろにフェンスを置き、GPUが使い終わったものを回収する
	_gpu.EndFrame();
	++_frame;

	Evict();

	// 新しくクックしたアセットがあれば、パックのテーブルを書き込んで確定させる
	if (_pack.IsDirty())
	{
		_pack.Save();
	}
}

// ---------------------------------------------
// パイプライン
// ---------------------------------------------
bool CAPLoader::CreatePipeline(const std::string& name, const std::string& vertexShaderPath, const std::string& pixelShaderPath,
	const CapsuleAssetProtector::PipelineDesc& desc)
{
	if (!_initialized)
	{
		cap::SetError("CapsuleAssetProtector::Initialize が呼ばれていません");
		return false;
	}

	// 同じ名前があれば作り直す
	DestroyPipeline(name);

	if (!LoadInternal(vertexShaderPath, true))
	{
		return false;
	}
	if (!LoadInternal(pixelShaderPath, true))
	{
		Unload(vertexShaderPath);
		return false;
	}

	AssetEntry* vertexEntry = Find(vertexShaderPath);
	AssetEntry* pixelEntry = Find(pixelShaderPath);
	auto fail = [&](const std::string& message) {
		cap::SetError(message);
		Unload(vertexShaderPath);
		Unload(pixelShaderPath);
		return false;
	};

	if (vertexEntry == nullptr || vertexEntry->asset->type != cap::AssetType::Shader ||
		static_cast<cap::ShaderAsset*>(vertexEntry->asset.get())->stage != cap::ShaderStage::Vertex)
	{
		return fail(vertexShaderPath + " は頂点シェーダーではありません");
	}
	if (pixelEntry == nullptr || pixelEntry->asset->type != cap::AssetType::Shader ||
		static_cast<cap::ShaderAsset*>(pixelEntry->asset.get())->stage != cap::ShaderStage::Pixel)
	{
		return fail(pixelShaderPath + " はピクセルシェーダーではありません");
	}

	cap::PipelineOptions options;
	options.alphaBlend = desc.alphaBlend;
	options.depthTest = desc.depthTest;
	options.depthWrite = desc.depthWrite;
	options.wireframe = desc.wireframe;
	switch (desc.cullMode)
	{
	case CapsuleAssetProtector::CullMode::None:  options.cullMode = D3D12_CULL_MODE_NONE; break;
	case CapsuleAssetProtector::CullMode::Front: options.cullMode = D3D12_CULL_MODE_FRONT; break;
	default:                                     options.cullMode = D3D12_CULL_MODE_BACK; break;
	}
	options.rtvFormat = _rtvFormat;
	options.dsvFormat = _dsvFormat;

	PipelineEntry entry;
	entry.vertexShaderPath = vertexShaderPath;
	entry.pixelShaderPath = pixelShaderPath;
	if (!cap::BuildPipeline(_gpu.Device(),
		*static_cast<cap::ShaderAsset*>(vertexEntry->asset.get()),
		*static_cast<cap::ShaderAsset*>(pixelEntry->asset.get()),
		options, entry.pipeline))
	{
		// BuildPipelineが設定したエラーメッセージを残したまま参照だけ戻す
		Unload(vertexShaderPath);
		Unload(pixelShaderPath);
		return false;
	}

	_pipelines[name] = std::move(entry);
	return true;
}

void CAPLoader::DestroyPipeline(const std::string& name)
{
	auto it = _pipelines.find(name);
	if (it == _pipelines.end())
	{
		return;
	}
	if (_currentPipeline == &it->second)
	{
		_currentPipeline = nullptr;
	}
	// 今フレームのコマンドリストがまだ使っているかもしれないので遅延解放する
	_gpu.ReleaseLater(it->second.pipeline.pipelineState.Get());
	_gpu.ReleaseLater(it->second.pipeline.rootSignature.Get());
	Unload(it->second.vertexShaderPath);
	Unload(it->second.pixelShaderPath);
	_pipelines.erase(it);
}

D3D12_GPU_DESCRIPTOR_HANDLE CAPLoader::DefaultTextureHandle(cap::TextureRole role) const
{
	const cap::TextureAsset* texture = (role == cap::TextureRole::Normal) ? _flatNormalTexture.get() : _whiteTexture.get();
	return _gpu.GpuHandle(texture->srvIndex);
}

const cap::PipelineSlot* CAPLoader::FindSlot(const std::string& name, cap::SlotKind kind) const
{
	if (_currentPipeline == nullptr)
	{
		return nullptr;
	}
	for (const cap::PipelineSlot& slot : _currentPipeline->pipeline.slots)
	{
		if (slot.kind == kind && slot.name == name)
		{
			return &slot;
		}
	}
	return nullptr;
}

bool CAPLoader::BindPipeline(ID3D12GraphicsCommandList* commandList, const std::string& name)
{
	if (commandList == nullptr)
	{
		cap::SetError("BindPipeline: コマンドリストがnullです");
		return false;
	}
	auto it = _pipelines.find(name);
	if (it == _pipelines.end())
	{
		cap::SetError("パイプラインが見つかりません: " + name);
		return false;
	}

	// 外部管理の場合、ヒープ(InitDesc::srvHeap)は呼び出し側で設定済みである前提。
	// CAP内蔵ヒープの場合は呼び出し側では設定できないので、ここで設定する
	if (_gpu.OwnsHeap())
	{
		SetDescriptorHeap(commandList);
	}
	PipelineEntry& entry = it->second;
	commandList->SetGraphicsRootSignature(entry.pipeline.rootSignature.Get());
	commandList->SetPipelineState(entry.pipeline.pipelineState.Get());
	commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	// 未設定のまま描画してもGPUが不正なアドレスを読まないよう、すべてのスロットに既定値を入れておく
	for (const cap::PipelineSlot& slot : entry.pipeline.slots)
	{
		if (slot.kind == cap::SlotKind::ConstantBuffer)
		{
			commandList->SetGraphicsRootConstantBufferView(slot.rootIndex, _gpu.DefaultConstants());
		}
		else
		{
			commandList->SetGraphicsRootDescriptorTable(slot.rootIndex, DefaultTextureHandle(slot.role));
		}
	}

	_currentPipeline = &entry;
	return true;
}

void CAPLoader::SetDescriptorHeap(ID3D12GraphicsCommandList* commandList)
{
	if (commandList == nullptr || _gpu.Heap() == nullptr)
	{
		return;
	}
	ID3D12DescriptorHeap* heaps[] = { _gpu.Heap() };
	commandList->SetDescriptorHeaps(1, heaps);
}

bool CAPLoader::SetTexture(ID3D12GraphicsCommandList* commandList, const std::string& slotName, const std::string& texturePath)
{
	if (commandList == nullptr || _currentPipeline == nullptr)
	{
		cap::SetError("SetTexture: 先に BindPipeline を呼んでください");
		return false;
	}
	const cap::PipelineSlot* slot = FindSlot(slotName, cap::SlotKind::Texture);
	if (slot == nullptr)
	{
		cap::SetError("現在のパイプラインにテクスチャスロット '" + slotName + "' はありません");
		return false;
	}

	cap::TextureAsset* texture = UseTexture(texturePath);
	commandList->SetGraphicsRootDescriptorTable(slot->rootIndex,
		texture != nullptr ? _gpu.GpuHandle(texture->srvIndex) : DefaultTextureHandle(slot->role));
	return texture != nullptr;
}

bool CAPLoader::SetConstants(ID3D12GraphicsCommandList* commandList, const std::string& bufferName, const void* data, uint32_t size)
{
	if (commandList == nullptr || _currentPipeline == nullptr)
	{
		cap::SetError("SetConstants: 先に BindPipeline を呼んでください");
		return false;
	}
	const cap::PipelineSlot* slot = FindSlot(bufferName, cap::SlotKind::ConstantBuffer);
	if (slot == nullptr)
	{
		cap::SetError("現在のパイプラインに定数バッファ '" + bufferName + "' はありません");
		return false;
	}

	D3D12_GPU_VIRTUAL_ADDRESS address = 0;
	if (!_gpu.AllocateConstants(data, size, address))
	{
		return false;
	}
	commandList->SetGraphicsRootConstantBufferView(slot->rootIndex, address);
	return true;
}

bool CAPLoader::SetConstantBuffer(ID3D12GraphicsCommandList* commandList, const std::string& bufferName, D3D12_GPU_VIRTUAL_ADDRESS address)
{
	if (commandList == nullptr || _currentPipeline == nullptr)
	{
		cap::SetError("SetConstantBuffer: 先に BindPipeline を呼んでください");
		return false;
	}
	const cap::PipelineSlot* slot = FindSlot(bufferName, cap::SlotKind::ConstantBuffer);
	if (slot == nullptr)
	{
		cap::SetError("現在のパイプラインに定数バッファ '" + bufferName + "' はありません");
		return false;
	}
	commandList->SetGraphicsRootConstantBufferView(slot->rootIndex, address);
	return true;
}

// ---------------------------------------------
// メッシュ
// ---------------------------------------------
bool CAPLoader::CreateMesh(const std::string& name, const CapsuleAssetProtector::Vertex* vertices, uint32_t vertexCount,
	const uint32_t* indices, uint32_t indexCount)
{
	if (!_initialized)
	{
		cap::SetError("CapsuleAssetProtector::Initialize が呼ばれていません");
		return false;
	}

	std::unique_ptr<cap::MeshAsset> mesh = cap::CreateMeshFromData(_gpu, vertices, vertexCount, indices, indexCount);
	if (!mesh)
	{
		return false;
	}

	const std::string path = MemoryMeshPath(name);
	const cap::AssetKey key = KeyOf(path);
	auto it = _assets.find(key);
	if (it != _assets.end())
	{
		ReleaseEntry(it->second);
		_assets.erase(it);
	}

	AssetEntry entry;
	entry.path = path;
	entry.refCount = 1; // DestroyMeshまで常駐
	entry.lastUsedFrame = _frame;
	_residentBytes += mesh->memoryBytes;
	entry.asset = std::move(mesh);
	_assets.emplace(key, std::move(entry));
	return true;
}

void CAPLoader::DestroyMesh(const std::string& name)
{
	Unload(MemoryMeshPath(name));
}

bool CAPLoader::DrawMesh(ID3D12GraphicsCommandList* commandList, const std::string& meshPath)
{
	if (commandList == nullptr || _currentPipeline == nullptr)
	{
		cap::SetError("DrawMesh: 先に BindPipeline を呼んでください");
		return false;
	}

	// CreateMeshで作った名前を優先し、無ければモデルファイルとして扱う(未読み込みなら自動で読み込む)
	cap::Asset* asset = nullptr;
	if (AssetEntry* entry = Find(MemoryMeshPath(meshPath)))
	{
		entry->lastUsedFrame = _frame;
		asset = entry->asset.get();
	}
	else
	{
		asset = Use(meshPath);
	}
	if (asset == nullptr || asset->type != cap::AssetType::Mesh)
	{
		if (asset != nullptr)
		{
			cap::SetError(meshPath + " はメッシュではありません");
		}
		return false;
	}
	cap::MeshAsset* mesh = static_cast<cap::MeshAsset*>(asset);

	commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	commandList->IASetVertexBuffers(0, 1, &mesh->vertexBufferView);
	commandList->IASetIndexBuffer(&mesh->indexBufferView);

	for (const cap::SubMesh& sub : mesh->subMeshes)
	{
		// 部分メッシュのテクスチャを、役割が一致するスロットへ自動で割り当てる
		for (const cap::PipelineSlot& slot : _currentPipeline->pipeline.slots)
		{
			if (slot.kind != cap::SlotKind::Texture || slot.role == cap::TextureRole::Other)
			{
				continue; // Otherは利用側のSetTexture(または既定の白)のまま
			}
			const std::string& texturePath = (slot.role == cap::TextureRole::Normal) ? sub.normalPath : sub.diffusePath;
			cap::TextureAsset* texture = UseTexture(texturePath);
			commandList->SetGraphicsRootDescriptorTable(slot.rootIndex,
				texture != nullptr ? _gpu.GpuHandle(texture->srvIndex) : DefaultTextureHandle(slot.role));
		}
		commandList->DrawIndexedInstanced(sub.indexCount, 1, sub.startIndex, sub.baseVertex, 0);
	}
	return true;
}

// ---------------------------------------------
// 取得
// ---------------------------------------------
D3D12_GPU_DESCRIPTOR_HANDLE CAPLoader::GetTextureHandle(const std::string& texturePath)
{
	if (!_initialized)
	{
		return {};
	}
	cap::TextureAsset* texture = UseTexture(texturePath);
	return texture != nullptr ? _gpu.GpuHandle(texture->srvIndex) : DefaultTextureHandle(cap::TextureRole::Diffuse);
}

ID3D12PipelineState* CAPLoader::GetPipelineState(const std::string& name)
{
	auto it = _pipelines.find(name);
	return it != _pipelines.end() ? it->second.pipeline.pipelineState.Get() : nullptr;
}

ID3D12RootSignature* CAPLoader::GetRootSignature(const std::string& name)
{
	auto it = _pipelines.find(name);
	return it != _pipelines.end() ? it->second.pipeline.rootSignature.Get() : nullptr;
}

bool CAPLoader::GetSound(const std::string& path, CapsuleAssetProtector::SoundData& outSound)
{
	AssetEntry* entry = Find(path);
	if (entry == nullptr || entry->asset->type != cap::AssetType::Sound)
	{
		cap::SetError("GetSound: 先に Load してください: " + path);
		return false;
	}
	entry->lastUsedFrame = _frame;

	const cap::SoundAsset* sound = static_cast<const cap::SoundAsset*>(entry->asset.get());
	outSound.format = sound->format.data();
	outSound.formatSize = static_cast<uint32_t>(sound->format.size());
	outSound.samples = sound->samples.data();
	outSound.sampleSize = static_cast<uint32_t>(sound->samples.size());
	return true;
}
