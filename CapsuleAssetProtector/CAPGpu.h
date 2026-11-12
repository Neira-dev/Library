#pragma once

// =============================================
// CAPGpu
// =============================================
// CAPがGPUに対して行う処理をまとめた内部クラスです。利用者はここを意識する必要はありません。
// ・VRAMへのアップロード(UPLOADヒープ経由でDEFAULTヒープへコピーし、完了まで待つ)
// ・テクスチャのSRV
//     外部管理: 外部で一括管理しているディスクリプタヒープの番号を、渡された関数で借りる
//     CAP内蔵 : 外部から何も渡されなければ、CAPが専用のヒープを作って番号も自分で管理する
// ・遅延解放(GPUがまだ使っているかもしれないリソースは、フレームのフェンスを待ってから解放する)
// ・定数バッファ用のアップロード領域(SetConstantsで渡されたデータを毎フレーム書き込む)
//
// フェンスは2つ使い分けます。
//   _uploadFence : CAP自身のアップロード完了待ち用
//   _frameFence  : 利用側のフレーム境界(Update)用。遅延解放と定数バッファの再利用に使う
// アップロードのフェンスでフレーム境界を判断すると、まだ送信されていない利用側の
// コマンドリストより先に完了してしまうため、必ず別にしています。
// d3dx12.hは使いません。
// =============================================

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace cap
{
	using Microsoft::WRL::ComPtr;

	/**
	 * @brief VRAMへ送るテクスチャ1ミップ分のCPUデータ
	 */
	struct TextureMipData
	{
		const uint8_t* pixels = nullptr; // 先頭ピクセル
		uint32_t rowPitch = 0;           // 1行のバイト数
		uint32_t rows = 0;               // 行数(=高さ)
	};

	class CAPGpu
	{
	public:
		CAPGpu() = default;
		~CAPGpu();

		// 外部のディスクリプタヒープの番号を確保/解放する関数
		using AllocateSrvFunction = uint32_t(*)(void* userData);
		using FreeSrvFunction = void(*)(void* userData, uint32_t index);

		/**
		 * @brief 初期化
		 * @details srvHeap/allocateSrv/freeSrvをすべて渡すと外部管理、すべてnullptrならCAP内蔵ヒープ(srvCapacity個)を使う
		 */
		bool Initialize(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12DescriptorHeap* srvHeap,
			AllocateSrvFunction allocateSrv, FreeSrvFunction freeSrv, void* srvUserData, uint32_t srvCapacity);
		void Shutdown();

		ID3D12Device* Device() const { return _device.Get(); }

		// ---------------------------------------------
		// アップロード
		// ---------------------------------------------
		/**
		 * @brief 複数のアップロードを1回の送信・待機にまとめる(メッシュの頂点とインデックス等)
		 */
		bool BeginBatch();
		bool EndBatch();

		/**
		 * @brief データをVRAM(DEFAULTヒープ)上のバッファにアップロードする
		 */
		bool CreateStaticBuffer(const void* data, uint64_t size, ComPtr<ID3D12Resource>& outBuffer);

		/**
		 * @brief ミップ付きの2DテクスチャをVRAMにアップロードし、シェーダーから読める状態にする
		 */
		bool CreateTexture2D(uint32_t width, uint32_t height, DXGI_FORMAT format,
			const std::vector<TextureMipData>& mips, ComPtr<ID3D12Resource>& outTexture);

		// ---------------------------------------------
		// SRVディスクリプタヒープ
		// ---------------------------------------------
		uint32_t AllocateSrv();
		void ReleaseSrv(uint32_t index); // フレームのフェンスを待ってから外部へ返す
		D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(uint32_t index) const;
		D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(uint32_t index) const;
		ID3D12DescriptorHeap* Heap() const { return _srvHeap.Get(); }
		/**
		 * @brief CAP内蔵のヒープを使っているか(trueなら描画前にCAPがヒープを設定する必要がある)
		 */
		bool OwnsHeap() const { return _ownsSrvHeap; }

		// ---------------------------------------------
		// 遅延解放
		// ---------------------------------------------
		/**
		 * @brief GPUが使い終わるのを待ってから解放する(リソース・PSO・ルートシグネチャ等)
		 */
		void ReleaseLater(IUnknown* object);

		// ---------------------------------------------
		// フレーム
		// ---------------------------------------------
		/**
		 * @brief フレームの終わり(利用側がコマンドリストを送信した後)に呼ぶ
		 */
		void EndFrame();

		// ---------------------------------------------
		// 定数バッファ
		// ---------------------------------------------
		/**
		 * @brief 定数データを今フレーム用の領域に書き込み、そのGPUアドレスを返す
		 */
		bool AllocateConstants(const void* data, uint32_t size, D3D12_GPU_VIRTUAL_ADDRESS& outAddress);

		/**
		 * @brief 中身がすべて0の定数バッファ(未設定のスロットに仮で割り当てる)
		 */
		D3D12_GPU_VIRTUAL_ADDRESS DefaultConstants() const;

		/**
		 * @brief GPUの処理がすべて終わるまで待つ
		 */
		void WaitIdle();

	private:
		// 定数バッファ用のアップロード領域1ページ分
		struct ConstantPage
		{
			ComPtr<ID3D12Resource> resource;
			uint8_t* cpu = nullptr;
			D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
			uint64_t size = 0;
			uint64_t offset = 0;
			bool dedicated = false; // ページより大きいデータ用に個別に作ったもの(再利用しない)
		};

		struct PendingRelease
		{
			uint64_t fenceValue;
			ComPtr<IUnknown> object;
		};

		struct PendingSrv
		{
			uint64_t fenceValue;
			uint32_t index;
		};

		struct RetiredPage
		{
			uint64_t fenceValue;
			ConstantPage page;
		};

		// CAP内蔵ヒープ用の番号の確保/解放(外部管理と同じ関数の形にして、以降の処理を共通にする)
		static uint32_t AllocateInternalSrv(void* userData);
		static void FreeInternalSrv(void* userData, uint32_t index);

		bool BeginUploadList();
		bool EndUploadListAndWait();
		bool CreateUploadBuffer(uint64_t size, ComPtr<ID3D12Resource>& outBuffer);
		bool CreateConstantPage(uint64_t size, ConstantPage& outPage);
		void Reclaim();

	private:
		ComPtr<ID3D12Device> _device;
		ComPtr<ID3D12CommandQueue> _queue;

		// アップロード用
		ComPtr<ID3D12CommandAllocator> _uploadAllocator;
		ComPtr<ID3D12GraphicsCommandList> _uploadList;
		ComPtr<ID3D12Fence> _uploadFence;
		uint64_t _uploadFenceValue = 0;
		std::vector<ComPtr<ID3D12Resource>> _uploadStaging; // コピーが終わるまで保持する中継バッファ
		bool _inBatch = false;
		bool _listOpen = false;
		HANDLE _event = nullptr;

		// フレーム境界用
		ComPtr<ID3D12Fence> _frameFence;
		uint64_t _frameFenceValue = 1; // 次のEndFrameでシグナルする値

		// SRVヒープ(外部で作成されたもの。番号の確保・解放は外部の関数に任せる)
		ComPtr<ID3D12DescriptorHeap> _srvHeap;
		uint32_t _srvDescriptorSize = 0;
		AllocateSrvFunction _allocateSrv = nullptr;
		FreeSrvFunction _freeSrv = nullptr;
		void* _srvUserData = nullptr;
		bool _ownsSrvHeap = false;                // CAP内蔵ヒープを使っているか
		std::vector<uint32_t> _internalFreeSrv;   // CAP内蔵ヒープの空き番号

		// 遅延解放待ち
		std::vector<PendingRelease> _pendingReleases;
		std::vector<PendingSrv> _pendingSrv;

		// 定数バッファ
		ConstantPage _currentPage;
		std::vector<ConstantPage> _framePages;  // 今フレームで使い切ったページ
		std::vector<RetiredPage> _retiredPages; // GPUが使い終わるのを待っているページ
		std::vector<ConstantPage> _freePages;   // 再利用できるページ
		ComPtr<ID3D12Resource> _defaultConstants;
	};
}
