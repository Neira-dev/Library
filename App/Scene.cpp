#include "Scene.h"
#include "App.h"
#include <wrl/client.h>
#include <d3dcompiler.h> // D3DReadFileToBlob を使うため
#include <cstring> // memcpy を使うため
#pragma comment(lib, "d3dcompiler.lib")

Vertex vertices[] = {
	{ { -0.5f, -0.5f, 0.0f } },
	{ {  0.0f,  0.5f, 0.0f } },
	{ {  0.5f, -0.5f, 0.0f } },
};

void Scene::CreateRootSignature()
{
	// ルートシグネチャの設計図(まだGPUオブジェクトではない)
	D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
	// 定数バッファ・テクスチャ等は今は使わないので0
	rootSignatureDesc.NumParameters = 0;
	rootSignatureDesc.pParameters = nullptr;
	// サンプラーも今は使わないので0
	rootSignatureDesc.NumStaticSamplers = 0;
	rootSignatureDesc.pStaticSamplers = nullptr;
	// 頂点バッファ(インプットレイアウト)経由でVSにデータを渡すことを許可する
	rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	// 設計図をドライバが理解できるバイト列に変換する
	Microsoft::WRL::ComPtr<ID3DBlob> rootSignatureBlob;
	Microsoft::WRL::ComPtr<ID3DBlob> errorBlob; // 変換失敗時のエラー内容が入る(今は未チェック)
	D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootSignatureBlob, &errorBlob);

	// バイト列から実際のGPUオブジェクトを作成し、メンバに保持する(PSO作成まで生存させる)
	GetDevice()->GetDevice()->CreateRootSignature(
		0, // nodeMask: 単一GPU構成では常に0
		rootSignatureBlob->GetBufferPointer(),
		rootSignatureBlob->GetBufferSize(),
		IID_PPV_ARGS(&_rootSignature)
	);
}

void Scene::CreatePSO()
{
	// ビルド時にコンパイルされた.csoファイルを読み込む
	Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
	Microsoft::WRL::ComPtr<ID3DBlob> psBlob;
	D3DReadFileToBlob(L"VS_Default.cso", &vsBlob);
	D3DReadFileToBlob(L"PS_Default.cso", &psBlob);

	// VSが受け取る頂点データの形(今はPOSITIONのみ、float3)
	D3D12_INPUT_ELEMENT_DESC inputElements[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

	// シェーダー・ステートを1つにまとめたPSOの設計図
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = _rootSignature.Get();
	psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
	psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
	psoDesc.InputLayout = { inputElements, _countof(inputElements) };
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

	// ラスタライザ設定(d3dx12.hを使わない方針のため手動で設定)
	psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
	psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
	psoDesc.RasterizerState.DepthClipEnable = TRUE;

	// ブレンド設定(今は合成せず、そのまま書き込む)
	psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

	// 深度・ステンシルは今回使わない(最小構成のため)
	psoDesc.DepthStencilState.DepthEnable = FALSE;
	psoDesc.DepthStencilState.StencilEnable = FALSE;

	psoDesc.SampleMask = UINT_MAX;
	psoDesc.SampleDesc.Count = 1; // MSAA無し
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; // DX12SwapChainと同じフォーマット

	GetDevice()->GetDevice()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&_pso));
}

void Scene::CreateVertexBuffer()
{
	// 今はUploaderの仕組み(DEFAULTヒープ+コピー)が無いので、
	// UPLOADヒープに直接頂点データを置く簡易な方法を使う
	D3D12_HEAP_PROPERTIES heapProps = {};
	heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

	D3D12_RESOURCE_DESC resourceDesc = {};
	resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	resourceDesc.Width = sizeof(vertices);
	resourceDesc.Height = 1;
	resourceDesc.DepthOrArraySize = 1;
	resourceDesc.MipLevels = 1;
	resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
	resourceDesc.SampleDesc.Count = 1;
	resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	GetDevice()->GetDevice()->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&resourceDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, // UPLOADヒープは常にこの状態で使う
		nullptr,
		IID_PPV_ARGS(&_vertexBuffer)
	);

	// UPLOADヒープはCPUから直接書き込める(Map/Unmapでポインタを取得してコピーする)
	void* mapped = nullptr;
	_vertexBuffer->Map(0, nullptr, &mapped);
	memcpy(mapped, vertices, sizeof(vertices));
	_vertexBuffer->Unmap(0, nullptr);

	// 頂点バッファビュー(GPU仮想アドレス+1頂点のバイト数+全体サイズ)
	_vbv.BufferLocation = _vertexBuffer->GetGPUVirtualAddress();
	_vbv.StrideInBytes = sizeof(Vertex);
	_vbv.SizeInBytes = sizeof(vertices);
}

void Scene::Initialize()
{
	// ルートシグネチャを先に作ってから、それを参照するPSOを作る
	CreateRootSignature();
	CreatePSO();
	// 頂点バッファは毎フレーム作り直す必要はないので、ここで1度だけ作る
	CreateVertexBuffer();
}

void Scene::Update()
{
}

void Scene::Render()
{
	// DX12Renderer::BeginFrame()?EndFrame()の間(レンダーターゲットがバインドされた状態)で呼ばれる想定
	ID3D12GraphicsCommandList* commandList = GetRenderer()->GetCommandList();

	// このフレームで使うルートシグネチャとPSOを設定する
	commandList->SetGraphicsRootSignature(_rootSignature.Get());
	commandList->SetPipelineState(_pso.Get());

	// 頂点を三角形として解釈するよう指定する
	commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	// スロット0に頂点バッファをセットする
	commandList->IASetVertexBuffers(0, 1, &_vbv);
	// 頂点3つ、インスタンス1つで描画する
	commandList->DrawInstanced(3, 1, 0, 0);
}

void Scene::UnLoad()
{
}
