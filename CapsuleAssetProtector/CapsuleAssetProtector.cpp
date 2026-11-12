#include "CapsuleAssetProtector.h"

#include "CAPLoader.h"
#include "CAPUtil.h"

#include <memory>
#include <string>

// CapsuleAssetProtectorのグローバル変数
std::unique_ptr<CAPLoader> g_capLoader = nullptr;

namespace
{
	// Initializeより前にSetKeyされた場合に、鍵を覚えておく
	std::string g_pendingKey;
	bool g_hasPendingKey = false;

	// 初期化済みかどうか(未初期化ならエラーを記録する)
	bool IsReady()
	{
		if (!g_capLoader)
		{
			cap::SetError("CapsuleAssetProtector::Initialize が呼ばれていません");
			return false;
		}
		return true;
	}

	std::string ToString(const char* text)
	{
		return text != nullptr ? std::string(text) : std::string();
	}
}


bool CapsuleAssetProtector::Initialize(const InitDesc& desc)
{
	g_capLoader = std::make_unique<CAPLoader>();
	if (g_hasPendingKey)
	{
		g_capLoader->SetKey(g_pendingKey);
	}
	if (!g_capLoader->Initialize(desc))
	{
		g_capLoader.reset();
		return false;
	}
	return true;
}


void CapsuleAssetProtector::Shutdown()
{
	g_capLoader.reset();
}

void CapsuleAssetProtector::Update()
{
	if (g_capLoader)
	{
		g_capLoader->Update();
	}
}

bool CapsuleAssetProtector::Load(const char* path)
{
	if (!IsReady() || path == nullptr)
	{
		return false;
	}
	return g_capLoader->Load(path);
}

void CapsuleAssetProtector::Unload(const char* path)
{
	if (g_capLoader && path != nullptr)
	{
		g_capLoader->Unload(path);
	}
}

bool CapsuleAssetProtector::IsLoaded(const char* path)
{
	return g_capLoader && path != nullptr && g_capLoader->IsLoaded(path);
}

void CapsuleAssetProtector::SetKey(const char* key)
{
	g_pendingKey = ToString(key);
	g_hasPendingKey = true;
	if (g_capLoader)
	{
		g_capLoader->SetKey(g_pendingKey);
	}
}

void CapsuleAssetProtector::SetMemoryBudget(uint64_t bytes)
{
	if (g_capLoader)
	{
		g_capLoader->SetMemoryBudget(bytes);
	}
}

uint64_t CapsuleAssetProtector::GetResidentMemory()
{
	return g_capLoader ? g_capLoader->GetResidentBytes() : 0;
}

bool CapsuleAssetProtector::CreatePipeline(const char* name, const char* vertexShaderPath, const char* pixelShaderPath, const PipelineDesc& desc)
{
	if (!IsReady() || name == nullptr || vertexShaderPath == nullptr || pixelShaderPath == nullptr)
	{
		return false;
	}
	return g_capLoader->CreatePipeline(name, vertexShaderPath, pixelShaderPath, desc);
}

void CapsuleAssetProtector::DestroyPipeline(const char* name)
{
	if (g_capLoader && name != nullptr)
	{
		g_capLoader->DestroyPipeline(name);
	}
}

bool CapsuleAssetProtector::BindPipeline(ID3D12GraphicsCommandList* commandList, const char* name)
{
	if (!IsReady() || name == nullptr)
	{
		return false;
	}
	return g_capLoader->BindPipeline(commandList, name);
}

bool CapsuleAssetProtector::SetTexture(ID3D12GraphicsCommandList* commandList, const char* slotName, const char* texturePath)
{
	if (!IsReady() || slotName == nullptr)
	{
		return false;
	}
	return g_capLoader->SetTexture(commandList, slotName, ToString(texturePath));
}

bool CapsuleAssetProtector::SetConstants(ID3D12GraphicsCommandList* commandList, const char* bufferName, const void* data, uint32_t size)
{
	if (!IsReady() || bufferName == nullptr)
	{
		return false;
	}
	return g_capLoader->SetConstants(commandList, bufferName, data, size);
}

bool CapsuleAssetProtector::SetConstantBuffer(ID3D12GraphicsCommandList* commandList, const char* bufferName, D3D12_GPU_VIRTUAL_ADDRESS address)
{
	if (!IsReady() || bufferName == nullptr)
	{
		return false;
	}
	return g_capLoader->SetConstantBuffer(commandList, bufferName, address);
}

bool CapsuleAssetProtector::CreateMesh(const char* name, const Vertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount)
{
	if (!IsReady() || name == nullptr)
	{
		return false;
	}
	return g_capLoader->CreateMesh(name, vertices, vertexCount, indices, indexCount);
}

void CapsuleAssetProtector::DestroyMesh(const char* name)
{
	if (g_capLoader && name != nullptr)
	{
		g_capLoader->DestroyMesh(name);
	}
}

bool CapsuleAssetProtector::DrawMesh(ID3D12GraphicsCommandList* commandList, const char* meshPath)
{
	if (!IsReady() || meshPath == nullptr)
	{
		return false;
	}
	return g_capLoader->DrawMesh(commandList, meshPath);
}

D3D12_GPU_DESCRIPTOR_HANDLE CapsuleAssetProtector::GetTextureHandle(const char* texturePath)
{
	if (!IsReady() || texturePath == nullptr)
	{
		return {};
	}
	return g_capLoader->GetTextureHandle(texturePath);
}

ID3D12DescriptorHeap* CapsuleAssetProtector::GetDescriptorHeap()
{
	return g_capLoader ? g_capLoader->GetDescriptorHeap() : nullptr;
}

void CapsuleAssetProtector::SetDescriptorHeap(ID3D12GraphicsCommandList* commandList)
{
	if (g_capLoader)
	{
		g_capLoader->SetDescriptorHeap(commandList);
	}
}

ID3D12PipelineState* CapsuleAssetProtector::GetPipelineState(const char* name)
{
	return (g_capLoader && name != nullptr) ? g_capLoader->GetPipelineState(name) : nullptr;
}

ID3D12RootSignature* CapsuleAssetProtector::GetRootSignature(const char* name)
{
	return (g_capLoader && name != nullptr) ? g_capLoader->GetRootSignature(name) : nullptr;
}

bool CapsuleAssetProtector::GetSound(const char* path, SoundData* outSound)
{
	if (!IsReady() || path == nullptr || outSound == nullptr)
	{
		return false;
	}
	return g_capLoader->GetSound(path, *outSound);
}

const char* CapsuleAssetProtector::GetLastErrorMessage()
{
	return cap::GetError();
}
