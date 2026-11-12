#include "CAPGpu.h"
#include "CAPUtil.h"

#include <algorithm>
#include <cstring>

namespace cap
{
	namespace
	{
		// 定数バッファ1ページのサイズ
		constexpr uint64_t kConstantPageSize = 2ull * 1024 * 1024;
		// 定数バッファのアドレスは256バイト境界である必要がある
		constexpr uint64_t kConstantAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
		// 定数バッファの最大サイズ(4096 * float4)。未設定スロット用の0埋めバッファはこの大きさで作る
		constexpr uint64_t kMaxConstantBufferSize = 65536;

		D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type)
		{
			D3D12_HEAP_PROPERTIES props = {};
			props.Type = type;
			props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
			props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
			props.CreationNodeMask = 1;
			props.VisibleNodeMask = 1;
			return props;
		}

		D3D12_RESOURCE_DESC BufferDesc(uint64_t size)
		{
			D3D12_RESOURCE_DESC desc = {};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			desc.Width = size;
			desc.Height = 1;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = DXGI_FORMAT_UNKNOWN;
			desc.SampleDesc.Count = 1;
			desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			desc.Flags = D3D12_RESOURCE_FLAG_NONE;
			return desc;
		}

		uint64_t AlignUp(uint64_t value, uint64_t alignment)
		{
			return (value + alignment - 1) & ~(alignment - 1);
		}
	}

	CAPGpu::~CAPGpu()
	{
		Shutdown();
	}

	bool CAPGpu::Initialize(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12DescriptorHeap* srvHeap,
		AllocateSrvFunction allocateSrv, FreeSrvFunction freeSrv, void* srvUserData, uint32_t srvCapacity)
	{
		if (device == nullptr || queue == nullptr)
		{
			SetError("InitDesc の device / commandQueue が設定されていません");
			return false;
		}
		// すべて渡されていれば外部管理、すべて未設定ならCAP内蔵。一部だけは設定ミスとして扱う
		const bool anyExternal = srvHeap != nullptr || allocateSrv != nullptr || freeSrv != nullptr;
		const bool useExternal = srvHeap != nullptr && allocateSrv != nullptr && freeSrv != nullptr;
		if (anyExternal && !useExternal)
		{
			SetError("InitDesc の srvHeap / allocateSrv / freeSrv は、すべて設定する(外部管理)か、すべて未設定にする(CAP内蔵ヒープ)必要があります");
			return false;
		}

		if (useExternal)
		{
			// テクスチャのSRVを置くので、シェーダーから見えるCBV_SRV_UAVヒープである必要がある
			const D3D12_DESCRIPTOR_HEAP_DESC heapDesc = srvHeap->GetDesc();
			if (heapDesc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
				(heapDesc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) == 0)
			{
				SetError("InitDesc::srvHeap はシェーダーから見えるCBV_SRV_UAVヒープである必要があります");
				return false;
			}
		}
		else if (srvCapacity == 0)
		{
			SetError("InitDesc::srvCapacity が0です(CAP内蔵ヒープを使う場合は1以上にしてください)");
			return false;
		}
		_device = device;
		_queue = queue;

		// アップロード用のコマンドアロケータ・コマンドリスト(利用側と同じDIRECTキューで実行する)
		if (FAILED(_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_uploadAllocator))) ||
			FAILED(_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _uploadAllocator.Get(), nullptr, IID_PPV_ARGS(&_uploadList))))
		{
			SetError("アップロード用コマンドリストの作成に失敗しました");
			return false;
		}
		_uploadList->Close();

		if (FAILED(_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_uploadFence))) ||
			FAILED(_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_frameFence))))
		{
			SetError("フェンスの作成に失敗しました");
			return false;
		}
		_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (_event == nullptr)
		{
			SetError("イベントの作成に失敗しました");
			return false;
		}

		_srvDescriptorSize = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		if (useExternal)
		{
			// 外部で一括管理しているヒープを借りる(番号の確保・解放は渡された関数に任せる)
			_srvHeap = srvHeap;
			_allocateSrv = allocateSrv;
			_freeSrv = freeSrv;
			_srvUserData = srvUserData;
			_ownsSrvHeap = false;
		}
		else
		{
			// CAP内蔵: 専用のヒープを作り、番号も自分で管理する
			D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
			heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			heapDesc.NumDescriptors = srvCapacity;
			heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			if (FAILED(_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_srvHeap))))
			{
				SetError("SRVディスクリプタヒープの作成に失敗しました");
				return false;
			}
			_internalFreeSrv.clear();
			_internalFreeSrv.reserve(srvCapacity);
			for (uint32_t i = srvCapacity; i > 0; --i)
			{
				_internalFreeSrv.push_back(i - 1); // 0番から順に使われるよう逆順に積む
			}
			_allocateSrv = &CAPGpu::AllocateInternalSrv;
			_freeSrv = &CAPGpu::FreeInternalSrv;
			_srvUserData = this;
			_ownsSrvHeap = true;
		}

		// 未設定の定数バッファスロットに割り当てる0埋めのバッファ
		if (!CreateUploadBuffer(kMaxConstantBufferSize, _defaultConstants))
		{
			return false;
		}
		void* mapped = nullptr;
		if (SUCCEEDED(_defaultConstants->Map(0, nullptr, &mapped)))
		{
			memset(mapped, 0, static_cast<size_t>(kMaxConstantBufferSize));
			_defaultConstants->Unmap(0, nullptr);
		}
		return true;
	}

	void CAPGpu::Shutdown()
	{
		if (_queue && _frameFence)
		{
			// 今フレームの分もシグナルしてから、GPUの完了を待つ
			_queue->Signal(_frameFence.Get(), _frameFenceValue);
			WaitIdle();
		}

		// GPUの処理は完了しているので、解放待ちのSRVの番号をすべて外部へ返す
		if (_freeSrv != nullptr)
		{
			for (const PendingSrv& pending : _pendingSrv)
			{
				_freeSrv(_srvUserData, pending.index);
			}
		}
		_pendingReleases.clear();
		_pendingSrv.clear();
		_currentPage = {};
		_framePages.clear();
		_retiredPages.clear();
		_freePages.clear();
		_uploadStaging.clear();
		_defaultConstants.Reset();
		_srvHeap.Reset();
		_allocateSrv = nullptr;
		_freeSrv = nullptr;
		_srvUserData = nullptr;
		_ownsSrvHeap = false;
		_internalFreeSrv.clear();
		_uploadList.Reset();
		_uploadAllocator.Reset();
		_uploadFence.Reset();
		_frameFence.Reset();
		_queue.Reset();
		_device.Reset();
		if (_event != nullptr)
		{
			CloseHandle(_event);
			_event = nullptr;
		}
	}

	// ---------------------------------------------
	// アップロード
	// ---------------------------------------------
	bool CAPGpu::BeginUploadList()
	{
		if (_listOpen)
		{
			return true;
		}
		if (FAILED(_uploadAllocator->Reset()) || FAILED(_uploadList->Reset(_uploadAllocator.Get(), nullptr)))
		{
			SetError("アップロード用コマンドリストのリセットに失敗しました");
			return false;
		}
		_listOpen = true;
		return true;
	}

	bool CAPGpu::EndUploadListAndWait()
	{
		if (!_listOpen)
		{
			return true;
		}
		_listOpen = false;

		if (FAILED(_uploadList->Close()))
		{
			SetError("アップロード用コマンドリストのクローズに失敗しました");
			_uploadStaging.clear();
			return false;
		}

		ID3D12CommandList* lists[] = { _uploadList.Get() };
		_queue->ExecuteCommandLists(1, lists);

		const uint64_t value = ++_uploadFenceValue;
		_queue->Signal(_uploadFence.Get(), value);
		if (_uploadFence->GetCompletedValue() < value)
		{
			_uploadFence->SetEventOnCompletion(value, _event);
			WaitForSingleObject(_event, INFINITE);
		}

		// コピーが終わったので中継バッファは解放してよい
		_uploadStaging.clear();
		return true;
	}

	bool CAPGpu::BeginBatch()
	{
		if (!BeginUploadList())
		{
			return false;
		}
		_inBatch = true;
		return true;
	}

	bool CAPGpu::EndBatch()
	{
		_inBatch = false;
		return EndUploadListAndWait();
	}

	bool CAPGpu::CreateUploadBuffer(uint64_t size, ComPtr<ID3D12Resource>& outBuffer)
	{
		const D3D12_HEAP_PROPERTIES props = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
		const D3D12_RESOURCE_DESC desc = BufferDesc(size);
		if (FAILED(_device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&outBuffer))))
		{
			SetError("アップロード用バッファの作成に失敗しました");
			return false;
		}
		return true;
	}

	bool CAPGpu::CreateStaticBuffer(const void* data, uint64_t size, ComPtr<ID3D12Resource>& outBuffer)
	{
		if (data == nullptr || size == 0)
		{
			SetError("CreateStaticBuffer: データが空です");
			return false;
		}

		// VRAM上のバッファ(バッファは常にCOMMON状態で作られ、コピー時に自動でCOPY_DESTへ昇格する)
		const D3D12_HEAP_PROPERTIES props = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
		const D3D12_RESOURCE_DESC desc = BufferDesc(size);
		if (FAILED(_device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&outBuffer))))
		{
			SetError("頂点/インデックスバッファの作成に失敗しました");
			return false;
		}

		ComPtr<ID3D12Resource> staging;
		if (!CreateUploadBuffer(size, staging))
		{
			return false;
		}
		void* mapped = nullptr;
		if (FAILED(staging->Map(0, nullptr, &mapped)))
		{
			SetError("アップロード用バッファのマップに失敗しました");
			return false;
		}
		memcpy(mapped, data, static_cast<size_t>(size));
		staging->Unmap(0, nullptr);

		const bool ownList = !_inBatch;
		if (!BeginUploadList())
		{
			return false;
		}
		_uploadList->CopyBufferRegion(outBuffer.Get(), 0, staging.Get(), 0, size);
		_uploadStaging.push_back(staging);

		// バッファはExecuteCommandListsの完了でCOMMONへ戻り、描画時に頂点/インデックスバッファ状態へ自動で昇格する
		return ownList ? EndUploadListAndWait() : true;
	}

	bool CAPGpu::CreateTexture2D(uint32_t width, uint32_t height, DXGI_FORMAT format,
		const std::vector<TextureMipData>& mips, ComPtr<ID3D12Resource>& outTexture)
	{
		if (width == 0 || height == 0 || mips.empty())
		{
			SetError("CreateTexture2D: テクスチャのサイズが不正です");
			return false;
		}

		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = width;
		desc.Height = height;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = static_cast<UINT16>(mips.size());
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags = D3D12_RESOURCE_FLAG_NONE;

		const D3D12_HEAP_PROPERTIES props = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
		if (FAILED(_device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&outTexture))))
		{
			SetError("テクスチャの作成に失敗しました");
			return false;
		}

		// 各ミップをUPLOADヒープへ書き込むときのレイアウト(行の幅はGPUの要求する境界に揃えられる)
		const UINT mipCount = static_cast<UINT>(mips.size());
		std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(mipCount);
		std::vector<UINT> numRows(mipCount);
		std::vector<UINT64> rowSizes(mipCount);
		UINT64 totalBytes = 0;
		_device->GetCopyableFootprints(&desc, 0, mipCount, 0, layouts.data(), numRows.data(), rowSizes.data(), &totalBytes);

		ComPtr<ID3D12Resource> staging;
		if (!CreateUploadBuffer(totalBytes, staging))
		{
			return false;
		}
		uint8_t* mapped = nullptr;
		if (FAILED(staging->Map(0, nullptr, reinterpret_cast<void**>(&mapped))))
		{
			SetError("アップロード用バッファのマップに失敗しました");
			return false;
		}
		for (UINT i = 0; i < mipCount; ++i)
		{
			const UINT rows = (std::min)(numRows[i], mips[i].rows);
			const size_t copyBytes = static_cast<size_t>((std::min<UINT64>)(rowSizes[i], mips[i].rowPitch));
			for (UINT row = 0; row < rows; ++row)
			{
				memcpy(mapped + layouts[i].Offset + static_cast<UINT64>(row) * layouts[i].Footprint.RowPitch,
					mips[i].pixels + static_cast<size_t>(row) * mips[i].rowPitch,
					copyBytes);
			}
		}
		staging->Unmap(0, nullptr);

		const bool ownList = !_inBatch;
		if (!BeginUploadList())
		{
			return false;
		}
		for (UINT i = 0; i < mipCount; ++i)
		{
			D3D12_TEXTURE_COPY_LOCATION dst = {};
			dst.pResource = outTexture.Get();
			dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dst.SubresourceIndex = i;

			D3D12_TEXTURE_COPY_LOCATION src = {};
			src.pResource = staging.Get();
			src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			src.PlacedFootprint = layouts[i];

			_uploadList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		}

		// コピー先の状態から、どのシェーダーからでも読める状態へ遷移する
		D3D12_RESOURCE_BARRIER barrier = {};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		barrier.Transition.pResource = outTexture.Get();
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		_uploadList->ResourceBarrier(1, &barrier);

		_uploadStaging.push_back(staging);
		return ownList ? EndUploadListAndWait() : true;
	}

	// ---------------------------------------------
	// SRVディスクリプタヒープ
	// ---------------------------------------------
	uint32_t CAPGpu::AllocateInternalSrv(void* userData)
	{
		CAPGpu* gpu = static_cast<CAPGpu*>(userData);
		if (gpu->_internalFreeSrv.empty())
		{
			return UINT32_MAX;
		}
		const uint32_t index = gpu->_internalFreeSrv.back();
		gpu->_internalFreeSrv.pop_back();
		return index;
	}

	void CAPGpu::FreeInternalSrv(void* userData, uint32_t index)
	{
		static_cast<CAPGpu*>(userData)->_internalFreeSrv.push_back(index);
	}

	uint32_t CAPGpu::AllocateSrv()
	{
		if (_allocateSrv == nullptr)
		{
			return UINT32_MAX;
		}
		uint32_t index = _allocateSrv(_srvUserData);
		if (index == UINT32_MAX)
		{
			// 解放待ちの中にGPUが使い終わったものがあれば外部へ返してから再挑戦する
			Reclaim();
			index = _allocateSrv(_srvUserData);
		}
		if (index == UINT32_MAX)
		{
			SetError(_ownsSrvHeap
				? "ディスクリプタヒープに空きがありません(InitDesc::srvCapacity を増やしてください)"
				: "ディスクリプタヒープに空きがありません(外部のディスクリプタヒープの容量を増やしてください)");
		}
		return index;
	}

	void CAPGpu::ReleaseSrv(uint32_t index)
	{
		if (index == UINT32_MAX)
		{
			return;
		}
		_pendingSrv.push_back({ _frameFenceValue, index });
	}

	D3D12_CPU_DESCRIPTOR_HANDLE CAPGpu::CpuHandle(uint32_t index) const
	{
		D3D12_CPU_DESCRIPTOR_HANDLE handle = _srvHeap->GetCPUDescriptorHandleForHeapStart();
		handle.ptr += static_cast<SIZE_T>(index) * _srvDescriptorSize;
		return handle;
	}

	D3D12_GPU_DESCRIPTOR_HANDLE CAPGpu::GpuHandle(uint32_t index) const
	{
		D3D12_GPU_DESCRIPTOR_HANDLE handle = _srvHeap->GetGPUDescriptorHandleForHeapStart();
		handle.ptr += static_cast<UINT64>(index) * _srvDescriptorSize;
		return handle;
	}

	// ---------------------------------------------
	// 遅延解放
	// ---------------------------------------------
	void CAPGpu::ReleaseLater(IUnknown* object)
	{
		if (object == nullptr)
		{
			return;
		}
		// 今フレームのコマンドリストがまだ使っているかもしれないので、今フレームのフェンス値で待つ
		_pendingReleases.push_back({ _frameFenceValue, ComPtr<IUnknown>(object) });
	}

	// ---------------------------------------------
	// フレーム
	// ---------------------------------------------
	void CAPGpu::EndFrame()
	{
		if (!_queue)
		{
			return;
		}

		// 利用側が今フレームに送信したコマンドの後ろにフェンスを置く
		const uint64_t value = _frameFenceValue;
		_queue->Signal(_frameFence.Get(), value);

		// 今フレームで使った定数バッファのページは、このフェンスが通過したら再利用できる
		for (ConstantPage& page : _framePages)
		{
			_retiredPages.push_back({ value, std::move(page) });
		}
		_framePages.clear();
		if (_currentPage.resource)
		{
			_retiredPages.push_back({ value, std::move(_currentPage) });
			_currentPage = {};
		}

		++_frameFenceValue;
		Reclaim();
	}

	void CAPGpu::Reclaim()
	{
		if (!_frameFence)
		{
			return;
		}
		const uint64_t completed = _frameFence->GetCompletedValue();

		_pendingReleases.erase(
			std::remove_if(_pendingReleases.begin(), _pendingReleases.end(),
				[completed](const PendingRelease& p) { return p.fenceValue <= completed; }),
			_pendingReleases.end());

		for (auto it = _pendingSrv.begin(); it != _pendingSrv.end();)
		{
			if (it->fenceValue <= completed)
			{
				_freeSrv(_srvUserData, it->index);
				it = _pendingSrv.erase(it);
			}
			else
			{
				++it;
			}
		}

		for (auto it = _retiredPages.begin(); it != _retiredPages.end();)
		{
			if (it->fenceValue <= completed)
			{
				if (!it->page.dedicated)
				{
					it->page.offset = 0;
					_freePages.push_back(std::move(it->page));
				}
				it = _retiredPages.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	// ---------------------------------------------
	// 定数バッファ
	// ---------------------------------------------
	bool CAPGpu::CreateConstantPage(uint64_t size, ConstantPage& outPage)
	{
		ConstantPage page;
		if (!CreateUploadBuffer(size, page.resource))
		{
			return false;
		}
		// UPLOADヒープは書き込みだけなら永続的にマップしたままでよい
		if (FAILED(page.resource->Map(0, nullptr, reinterpret_cast<void**>(&page.cpu))))
		{
			SetError("定数バッファ領域のマップに失敗しました");
			return false;
		}
		page.gpu = page.resource->GetGPUVirtualAddress();
		page.size = size;
		page.offset = 0;
		outPage = std::move(page);
		return true;
	}

	bool CAPGpu::AllocateConstants(const void* data, uint32_t size, D3D12_GPU_VIRTUAL_ADDRESS& outAddress)
	{
		if (data == nullptr || size == 0)
		{
			SetError("SetConstants: データが空です");
			return false;
		}
		const uint64_t aligned = AlignUp(size, kConstantAlignment);

		// ページより大きいデータは個別に領域を作る
		if (aligned > kConstantPageSize)
		{
			ConstantPage page;
			if (!CreateConstantPage(aligned, page))
			{
				return false;
			}
			page.dedicated = true;
			memcpy(page.cpu, data, size);
			outAddress = page.gpu;
			_framePages.push_back(std::move(page));
			return true;
		}

		if (!_currentPage.resource || _currentPage.offset + aligned > _currentPage.size)
		{
			if (_currentPage.resource)
			{
				_framePages.push_back(std::move(_currentPage));
				_currentPage = {};
			}
			Reclaim();
			if (!_freePages.empty())
			{
				_currentPage = std::move(_freePages.back());
				_freePages.pop_back();
				_currentPage.offset = 0;
			}
			else if (!CreateConstantPage(kConstantPageSize, _currentPage))
			{
				return false;
			}
		}

		memcpy(_currentPage.cpu + _currentPage.offset, data, size);
		outAddress = _currentPage.gpu + _currentPage.offset;
		_currentPage.offset += aligned;
		return true;
	}

	D3D12_GPU_VIRTUAL_ADDRESS CAPGpu::DefaultConstants() const
	{
		return _defaultConstants ? _defaultConstants->GetGPUVirtualAddress() : 0;
	}

	void CAPGpu::WaitIdle()
	{
		if (!_queue || !_uploadFence)
		{
			return;
		}
		const uint64_t value = ++_uploadFenceValue;
		_queue->Signal(_uploadFence.Get(), value);
		if (_uploadFence->GetCompletedValue() < value)
		{
			_uploadFence->SetEventOnCompletion(value, _event);
			WaitForSingleObject(_event, INFINITE);
		}
	}
}
