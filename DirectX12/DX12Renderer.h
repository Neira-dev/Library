#pragma once
#include <d3d12.h>
#include <vector>
#include <wrl/client.h>

class DX12Device;
class DX12CommandQueue;
class DX12SwapChain;
class FrameContext;

class DX12Renderer
{
public:
	DX12Renderer() = default;
	~DX12Renderer() = default;
public:
	/**
	 * @brief 初期化
	 * @param commandQueue
	 * @param swapChain
	 * @return
	 */
	HRESULT Initialize(DX12Device* device, DX12CommandQueue* commandQueue, DX12SwapChain* swapChain);

	/**
	 * @brief フレームの開始
	 * @return
	 */
	HRESULT BeginFrame(ID3D12DescriptorHeap* descriptorHeap);

	/**
	 * @brief フレームの終了
	 * @return
	 */
	HRESULT EndFrame();

	/**
	 * @brief 解放処理
	 */
	void Release();

	/**
	 * @brief コマンドリストを取得する
	 * @return ID3D12GraphicsCommandList*
	 */
	ID3D12GraphicsCommandList* GetCommandList() const { return _commandList.Get(); }
protected:
	DX12CommandQueue* _commandQueue = nullptr;
	DX12SwapChain* _swapChain = nullptr;
	std::vector < FrameContext* > _frameContexts;
	UINT _currentFrameIndex = 0;
	UINT _currentFrameContextIndex = 0;
	Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> _commandList;
};
