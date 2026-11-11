#pragma once
#include <wrl/client.h>
#include <d3d12.h>


struct Vertex
{
	float position[3];
};

class Scene
{
public:
	Scene() = default;
	~Scene() = default;
public:
	void Initialize();
	void Update();
	void Render();
	void UnLoad();

private:
	// ルートシグネチャを作成する
	void CreateRootSignature();
	// PSO(パイプラインステートオブジェクト)を作成する
	void CreatePSO();
	// 頂点バッファを作成する
	void CreateVertexBuffer();

private:
	// シェーダーが受け取るリソースの構成を定義したもの。PSO作成時に参照するので保持しておく
	Microsoft::WRL::ComPtr<ID3D12RootSignature> _rootSignature;
	// 描画に使うシェーダー・各種ステートをまとめたもの
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _pso;
	// 頂点データを保持するGPUリソース(今はUPLOADヒープに直接置く簡易構成)
	Microsoft::WRL::ComPtr<ID3D12Resource> _vertexBuffer;
	// 頂点バッファのGPU仮想アドレス・1頂点のバイト数・全体サイズをまとめたビュー
	D3D12_VERTEX_BUFFER_VIEW _vbv = {};
};
