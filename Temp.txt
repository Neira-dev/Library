#include "../Window/Window.h"

#include "../DirectX12/DX12Device.h"
#include "../DirectX12/Descriptor.h"
#include "../DirectX12/DX12CommandQueue.h"
#include "../DirectX12/DX12SwapChain.h"
#include "../DirectX12/DX12Renderer.h"

#include "../ImGui/ImGuiLayer.h"

#include "../CapsuleAssetProtector/CapsuleAssetProtector.h"

#include <DirectXMath.h>

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	Window::WindowDesc desc;
	desc.hInstance = hInstance;
	desc.nCmdShow = nCmdShow;
	Window window(desc);

	DX12Device::DeviceDesc deviceDesc;
	DX12Device device;
	device.Initialize(deviceDesc);

	Descriptor descriptor;
	descriptor.Initialize(&device);

	DX12CommandQueue commandQueue;
	commandQueue.Initialize(device.GetDevice(), D3D12_COMMAND_LIST_TYPE_DIRECT);

	DX12SwapChain::SwapChainDesc swapChainDesc;
	swapChainDesc.width = window.GetDesc().width;
	swapChainDesc.height = window.GetDesc().height;
	swapChainDesc.hwnd = window.GetHwnd();
	swapChainDesc.device = &device;
	swapChainDesc.commandQueue = &commandQueue;
	swapChainDesc.BufferCount = 2;

	DX12SwapChain swapChain;
	swapChain.Initialize(swapChainDesc);

	DX12Renderer renderer;
	renderer.Initialize(&device, &commandQueue, &swapChain);

	ImGuiLayer imGuiLayer;
	imGuiLayer.Initialize({ window.GetHwnd(), device.GetDevice(), commandQueue.GetCommandQueue(), swapChain.GetBufferCount(), DXGI_FORMAT_R8G8B8A8_UNORM, 64 });
	window.AddWndProc(ImGuiLayer::WndProcHandler);

	// CapsuleAssetProtectorの初期化(アセットの読み込み・VRAMへの転送・暗号化・パックへの保存はCAPが行う)
	CapsuleAssetProtector::SetKey("Neira-Library-Sample-Key");
	CapsuleAssetProtector::InitDesc capDesc;
	capDesc.device = device.GetDevice();
	capDesc.commandQueue = commandQueue.GetCommandQueue();
	// テクスチャのSRVはDescriptorクラスが一括管理しているヒープに作る(CAPは番号を借りるだけ)
	capDesc.srvHeap = descriptor.GetDescriptorHeap();
	capDesc.userData = &descriptor;
	capDesc.allocateSrv = [](void* user) -> uint32_t { return static_cast<Descriptor*>(user)->Allocate(); };
	capDesc.freeSrv = [](void* user, uint32_t index) { static_cast<Descriptor*>(user)->Free(index); };
	bool capReady = CapsuleAssetProtector::Initialize(capDesc);

	// シェーダーの組み合わせから、ルートシグネチャ・頂点レイアウト・PSOを自動で作る
	if (capReady)
	{
		capReady = CapsuleAssetProtector::CreatePipeline("Model", "Assets/Shader/VS_Model.hlsl", "Assets/Shader/PS_Model.hlsl");
	}
	if (!capReady)
	{
		MessageBoxA(window.GetHwnd(), CapsuleAssetProtector::GetLastErrorMessage(), "CapsuleAssetProtector", MB_OK | MB_ICONERROR);
	}

	// VS_Model.hlslのcbuffer Transformと同じ並び
	struct Transform
	{
		DirectX::XMFLOAT4X4 world;
		DirectX::XMFLOAT4X4 viewProjection;
	};

	MSG msg = {};
	while (msg.message != WM_QUIT)
	{
		// メッセージがあれば処理して次へ。無ければ描画する(GetMessageだとメッセージが来るまで止まるため)
		if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
			continue;
		}

		// 描画処理
		renderer.BeginFrame(descriptor.GetDescriptorHeap());

		// CAPでモデルを描画(モデル・テクスチャの読み込みとVRAMへの転送は初回の描画時にCAPが自動で行う)
		if (capReady)
		{
			using namespace DirectX;
			const float angle = static_cast<float>(GetTickCount64() % 100000) * 0.001f;
			const float aspect = static_cast<float>(window.GetDesc().width) / static_cast<float>(window.GetDesc().height);
			const XMMATRIX world = XMMatrixRotationY(angle) * XMMatrixRotationX(angle * 0.5f);
			const XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0.0f, 1.2f, -2.5f, 1.0f), XMVectorZero(), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
			const XMMATRIX projection = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), aspect, 0.1f, 100.0f);

			// HLSLの行列は列優先で読まれるので、転置して渡す
			Transform transform;
			XMStoreFloat4x4(&transform.world, XMMatrixTranspose(world));
			XMStoreFloat4x4(&transform.viewProjection, XMMatrixTranspose(view * projection));

			ID3D12GraphicsCommandList* commandList = renderer.GetCommandList();
			CapsuleAssetProtector::BindPipeline(commandList, "Model");
			CapsuleAssetProtector::SetConstants(commandList, "Transform", &transform, sizeof(transform));
			CapsuleAssetProtector::DrawMesh(commandList, "Assets/Model/Cube.obj");
		}

		imGuiLayer.BeginFrame();
	
		ImGui::Begin("Hello, ImGui!");
		ImGui::Text("This is some useful text.");
		ImGui::Text("CAP resident: %.1f KB", static_cast<double>(CapsuleAssetProtector::GetResidentMemory()) / 1024.0);
		ImGui::End();
		

		imGuiLayer.EndFrame(renderer.GetCommandList());
		renderer.EndFrame();

		// フレームの最後(コマンドリストを送信した後)にCAPを更新する
		CapsuleAssetProtector::Update();
	}

	// GPUの処理完了を待ってから、CAPのアセットを解放してパックを保存する
	CapsuleAssetProtector::Shutdown();

	imGuiLayer.Shutdown();
	renderer.Release();
	return (int)msg.wParam;
}
