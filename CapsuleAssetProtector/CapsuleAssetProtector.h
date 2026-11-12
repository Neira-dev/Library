#pragma once

// =============================================
// CapsuleAssetProtector
// =============================================
// アセットの管理・保護を行うクラスです。
// アセットの読み込み、バイナリ化、暗号化、復号化などを行います。
// アセットについて考える事なく、簡単にアセットを扱えるようにする事を目的としています。
// 
// Version: 1.0.0

// =============================================
/*
*	更新履歴
*	1.0.0 初版
*		・拡張子からアセットの種類を判定して読み込む(テクスチャ/シェーダー/メッシュ/サウンド)
*		・読み込んだアセットをクック(高速に読める形に変換)し、AESで暗号化してパック(.cpak)に保存
*		・2回目以降はパックから直接読み込む(元ファイルが更新されていれば自動で作り直す)
*		・使用中のアセットだけをメモリ/VRAMに展開し、使われなくなったものは自動で解放
*		・シェーダーのリフレクションから、ルートシグネチャ/PSO/頂点レイアウトを自動で作成
*/
// =============================================

// =============================================
// 対応環境
// DirectX12
// =============================================

// =============================================
// 基本的な使い方
// =============================================
/*
*	// 初期化(デバイスと、描画に使っているDIRECTコマンドキューを渡す)
*	CapsuleAssetProtector::SetKey("任意の暗号化キー");
*	CapsuleAssetProtector::InitDesc desc;
*	desc.device = device;
*	desc.commandQueue = commandQueue;
*
*	// テクスチャのSRVを置くディスクリプタヒープは2通りから選ぶ
*	// (1) 外部管理: ディスクリプタを一括管理している場合、そのヒープの番号を借りる
*	desc.srvHeap = descriptor.GetDescriptorHeap();
*	desc.userData = &descriptor;
*	desc.allocateSrv = [](void* user) -> uint32_t { return static_cast<Descriptor*>(user)->Allocate(); };
*	desc.freeSrv = [](void* user, uint32_t index) { static_cast<Descriptor*>(user)->Free(index); };
*	// (2) CAP内蔵: 上の4つを設定しなければ、CAPが専用のヒープ(srvCapacity個)を作る。
*	//     この場合は描画前に CapsuleAssetProtector::SetDescriptorHeap(commandList) を呼ぶ
*
*	CapsuleAssetProtector::Initialize(desc);
*
*	// シェーダーの組み合わせ(パイプライン)を名前を付けて作る
*	CapsuleAssetProtector::CreatePipeline("Model", "Shader/VS_Model.hlsl", "Shader/PS_Model.hlsl");
*
*	// 毎フレームの描画(読み込み・VRAMへの転送・ディスクリプタの管理はCAPが自動で行う)
*	CapsuleAssetProtector::BindPipeline(commandList, "Model");
*	CapsuleAssetProtector::SetConstants(commandList, "Transform", &transform, sizeof(transform));
*	CapsuleAssetProtector::DrawMesh(commandList, "Model/Character.fbx");
*
*	// フレームの最後(コマンドリストを送信した後)
*	CapsuleAssetProtector::Update();
*
*	// 終了時
*	CapsuleAssetProtector::Shutdown();
*/
// =============================================

#include "CAPExport.h"

#include <d3d12.h>
#include <cstdint>

namespace CapsuleAssetProtector
{
	/**
	 * @brief SRVディスクリプタの番号を確保する関数(失敗時は UINT32_MAX を返す)
	 */
	using AllocateSrvFunction = uint32_t(*)(void* userData);

	/**
	 * @brief SRVディスクリプタの番号を解放する関数
	 */
	using FreeSrvFunction = void(*)(void* userData, uint32_t index);

	/**
	 * @brief CAPが扱う標準の頂点形式
	 * @details CAPが読み込むメッシュは、すべてこの形式で頂点を持ちます。
	 *          パイプラインの頂点レイアウトは、頂点シェーダーが入力に使っているセマンティクスだけを
	 *          この形式から自動で選んで作るので、シェーダー側は必要な要素だけを宣言すれば動きます。
	 *          対応セマンティクス: POSITION / NORMAL / TEXCOORD / TANGENT / COLOR (いずれもインデックス0)
	 */
	struct Vertex
	{
		float position[3] = { 0.0f, 0.0f, 0.0f };
		float normal[3] = { 0.0f, 0.0f, 1.0f };
		float uv[2] = { 0.0f, 0.0f };
		float tangent[4] = { 1.0f, 0.0f, 0.0f, 1.0f }; // xyz = 接線, w = 従法線の向き(+1 / -1)
		float color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	};

	/**
	 * @brief 初期化情報
	 */
	struct InitDesc
	{
		/*
		* D3D12デバイス
		*/
		ID3D12Device* device = nullptr;
		/*
		* 描画に使っているDIRECTコマンドキュー
		* VRAMへの転送と、GPUが使い終わったリソースの解放タイミングの判定に使います
		*/
		ID3D12CommandQueue* commandQueue = nullptr;
		/*
		* クック済みアセットを保存するパックファイルのパス
		*/
		const char* packPath = "Assets.cpak";
		/*
		* 描画先のレンダーターゲットのフォーマット(パイプライン作成に使う)
		*/
		DXGI_FORMAT rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
		/*
		* 描画先の深度バッファのフォーマット(深度バッファを使わない場合は DXGI_FORMAT_UNKNOWN)
		*/
		DXGI_FORMAT dsvFormat = DXGI_FORMAT_D32_FLOAT;
		/*
		* テクスチャのSRVを作るディスクリプタヒープ(CBV_SRV_UAV・シェーダーから見えるもの)
		* 【外部管理】srvHeap/allocateSrv/freeSrvをすべて設定すると、外部(Descriptorクラス等)で一括管理している
		*             ヒープの番号を借りて使います。描画時にこのヒープがSetDescriptorHeapsされている必要があります。
		* 【CAP内蔵】すべて未設定(nullptr)にすると、CAPが専用のヒープ(srvCapacity個)を作って管理します。
		*             描画前に SetDescriptorHeap でCAPのヒープを設定してください。
		* 一部だけ設定した場合は Initialize が失敗します。
		*/
		ID3D12DescriptorHeap* srvHeap = nullptr;
		/*
		* srvHeap内の番号を確保/解放する関数
		* 解放はGPUがそのSRVを使い終わってから(Update内で)呼ばれます。
		*/
		AllocateSrvFunction allocateSrv = nullptr;
		FreeSrvFunction freeSrv = nullptr;
		/*
		* allocateSrv/freeSrvに渡されるユーザーデータ(Descriptorクラスのポインタ等)
		*/
		void* userData = nullptr;
		/*
		* 【CAP内蔵】の場合に作るヒープのディスクリプタ数(同時に展開できるテクスチャの最大数)
		* 外部管理の場合は使いません
		*/
		uint32_t srvCapacity = 1024;
	};

	/**
	 * @brief カリング(裏面を描画しない設定)
	 */
	enum class CullMode
	{
		None,  // 両面描画
		Front, // 表面を描画しない
		Back,  // 裏面を描画しない
	};

	/**
	 * @brief パイプラインの描画設定
	 * @details 同じシェーダーでも設定が違えば別のパイプラインになるので、別の名前で作成してください
	 */
	struct PipelineDesc
	{
		bool alphaBlend = false;            // 半透明合成をするか
		bool depthTest = true;              // 深度テストをするか
		bool depthWrite = true;             // 深度を書き込むか
		CullMode cullMode = CullMode::Back; // カリング
		bool wireframe = false;             // ワイヤーフレーム表示
	};

	/**
	 * @brief サウンドのデータ(XAudio2等にそのまま渡せる形)
	 */
	struct SoundData
	{
		const void* format = nullptr;  // WAVEFORMATEX(またはWAVEFORMATEXTENSIBLE)
		uint32_t formatSize = 0;
		const void* samples = nullptr; // PCMデータ
		uint32_t sampleSize = 0;
	};

	// =============================================
	// 初期化・終了
	// =============================================

	/**
	 * @brief CapsuleAssetProtectorの初期化処理を行います。
	 * @param desc 初期化情報(デバイスとコマンドキューは必須)
	 * @return 初期化に成功した場合はtrue
	 */
	CAP_API bool Initialize(const InitDesc& desc);

	/**
	 * @brief CapsuleAssetProtectorの終了処理を行います。
	 * @details GPUの処理完了を待ってからすべてのアセットを解放し、パックを保存します。
	 */
	CAP_API void Shutdown();

	/**
	 * @brief フレームの最後に毎フレーム呼んでください(コマンドリストを送信した後)
	 * @details 使われなくなったアセットの解放、パックの保存、定数バッファ領域の再利用を行います。
	 */
	CAP_API void Update();

	// =============================================
	// アセット
	// =============================================

	/**
	 * @brief CapsuleAssetProtectorのアセットをロードします。
	 * @param path ロードするアセットのパス
	 * @return ロードに成功した場合はtrue、失敗した場合はfalseを返します。
	 * @details 拡張子から種類を判定します(.png/.jpg等=テクスチャ, .hlsl=シェーダー, .fbx/.obj/.gltf/.glb=メッシュ, .wav=サウンド)。
	 *          Loadしたアセットは Unload するまで常駐します。
	 *          SetTexture/DrawMesh で使うアセットはLoadしなくても自動で読み込まれ、使われている間だけ常駐します。
	 */
	CAP_API bool Load(const char* path);

	/**
	 * @brief Loadしたアセットを手放します(使われなくなったら自動で解放されます)
	 */
	CAP_API void Unload(const char* path);

	/**
	 * @brief アセットがメモリ上に展開されているかどうか
	 */
	CAP_API bool IsLoaded(const char* path);

	/**
	 * @brief CapsuleAssetProtectorの暗号化キーを設定します。
	 * @param key　暗号化キーの文字列
	 * @details Initializeの前後どちらで呼んでも構いません。設定しない場合、パックは暗号化されません。
	 */
	CAP_API void SetKey(const char* key);

	/**
	 * @brief 使われていないアセットをメモリ上に残しておく上限(バイト)を設定します
	 * @details 0(既定)の場合、使われなくなったアセットはすぐに解放されます。
	 *          大きくすると、再び使われた時にパックから読み直さずに済みます。
	 */
	CAP_API void SetMemoryBudget(uint64_t bytes);

	/**
	 * @brief 現在メモリ/VRAMに展開しているアセットの合計サイズ(概算)
	 */
	CAP_API uint64_t GetResidentMemory();

	// =============================================
	// パイプライン(シェーダー)
	// =============================================

	/**
	 * @brief 頂点シェーダーとピクセルシェーダーの組み合わせから、名前付きのパイプラインを作ります
	 * @details シェーダーのリフレクションからルートシグネチャ・頂点レイアウト・PSOを自動で作るので、
	 *          ルートパラメータの番号や頂点レイアウトを意識する必要はありません。
	 *          シェーダーのエントリポイント名は main です。
	 */
	CAP_API bool CreatePipeline(const char* name, const char* vertexShaderPath, const char* pixelShaderPath, const PipelineDesc& desc = PipelineDesc());

	/**
	 * @brief パイプラインを破棄します
	 */
	CAP_API void DestroyPipeline(const char* name);

	/**
	 * @brief パイプラインをコマンドリストに設定します
	 * @details 外部管理の場合、ディスクリプタヒープは設定しません(InitDesc::srvHeapを呼び出し側で設定しておいてください)。
	 *          CAP内蔵ヒープの場合は、CAPのヒープもここで設定します。
	 *          未設定のテクスチャには白、定数バッファには0のデータが仮で割り当てられます。
	 *          描画系の関数(BindPipeline/SetTexture/SetConstants/DrawMesh)はデバッグ・確認用です。
	 */
	CAP_API bool BindPipeline(ID3D12GraphicsCommandList* commandList, const char* name);

	/**
	 * @brief 現在のパイプラインの、指定した名前のテクスチャスロットにテクスチャを設定します
	 * @param slotName シェーダー内のテクスチャの変数名(例: "DiffuseTexture")
	 * @param texturePath テクスチャのパス(未読み込みなら自動で読み込みます)
	 */
	CAP_API bool SetTexture(ID3D12GraphicsCommandList* commandList, const char* slotName, const char* texturePath);

	/**
	 * @brief 現在のパイプラインの、指定した名前の定数バッファにデータを設定します
	 * @param bufferName シェーダー内のcbufferの名前(例: "Transform")
	 * @details データはCAPがGPUへ転送するので、定数バッファのリソースを用意する必要はありません。
	 */
	CAP_API bool SetConstants(ID3D12GraphicsCommandList* commandList, const char* bufferName, const void* data, uint32_t size);

	/**
	 * @brief 現在のパイプラインの、指定した名前の定数バッファに、自前で用意したバッファを設定します
	 */
	CAP_API bool SetConstantBuffer(ID3D12GraphicsCommandList* commandList, const char* bufferName, D3D12_GPU_VIRTUAL_ADDRESS address);

	// =============================================
	// メッシュ
	// =============================================

	/**
	 * @brief メモリ上の頂点・インデックスから名前付きのメッシュを作ります(DestroyMeshまで常駐)
	 */
	CAP_API bool CreateMesh(const char* name, const Vertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount);

	/**
	 * @brief CreateMeshで作ったメッシュを破棄します
	 */
	CAP_API void DestroyMesh(const char* name);

	/**
	 * @brief メッシュを描画します(BindPipelineの後に呼んでください)
	 * @param meshPath モデルファイルのパス、またはCreateMeshで付けた名前
	 * @details モデルのテクスチャは、パイプラインのテクスチャスロット名から自動で割り当てます。
	 *          名前に"normal"を含むスロット = 法線マップ、"diffuse"/"albedo"/"base"/"color"/"main"を含むスロット
	 *          (無ければ最初のスロット) = ベースカラー。
	 */
	CAP_API bool DrawMesh(ID3D12GraphicsCommandList* commandList, const char* meshPath);

	// =============================================
	// ディスクリプタヒープ
	// =============================================

	/**
	 * @brief CAPがテクスチャのSRVを置いているディスクリプタヒープをコマンドリストに設定します
	 * @details CAP内蔵ヒープの場合、CAPのテクスチャ(GetTextureHandle)を使う前に呼んでください。
	 *          外部管理の場合は InitDesc::srvHeap が設定されるだけなので、呼ばなくても構いません。
	 */
	CAP_API void SetDescriptorHeap(ID3D12GraphicsCommandList* commandList);

	// =============================================
	// 取得
	// =============================================

	/**
	 * @brief テクスチャのSRVのGPUハンドルを取得します(InitDesc::srvHeap内)
	 */
	CAP_API D3D12_GPU_DESCRIPTOR_HANDLE GetTextureHandle(const char* texturePath);

	/**
	 * @brief テクスチャのSRVを作っているディスクリプタヒープ(InitDesc::srvHeap)を取得します
	 */
	CAP_API ID3D12DescriptorHeap* GetDescriptorHeap();

	/**
	 * @brief パイプラインのPSOを取得します
	 */
	CAP_API ID3D12PipelineState* GetPipelineState(const char* name);

	/**
	 * @brief パイプラインのルートシグネチャを取得します
	 */
	CAP_API ID3D12RootSignature* GetRootSignature(const char* name);

	/**
	 * @brief サウンドのデータを取得します
	 * @details 再生中にデータが解放されないよう、サウンドは事前に Load しておく必要があります。
	 */
	CAP_API bool GetSound(const char* path, SoundData* outSound);

	/**
	 * @brief 最後に起きたエラーの内容を取得します
	 */
	CAP_API const char* GetLastErrorMessage();
};
