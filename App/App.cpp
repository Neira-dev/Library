#include "App.h"

#include "../Window/Window.h"

#include "../DirectX12/DX12Device.h"
#include "../DirectX12/Descriptor.h"
#include "../DirectX12/DX12CommandQueue.h"
#include "../DirectX12/DX12SwapChain.h"
#include "../DirectX12/DX12Renderer.h"

#include "../ImGui/ImGuiLayer.h"

#include "../CapsuleAssetProtector/CapsuleAssetProtector.h"

App::App()
{
}

App::~App()
{
}

void App::Init(HINSTANCE hInstance, int nCmdShow)
{
	// ウインドウの作成
	Window::WindowDesc wDesc;
	wDesc.hInstance = hInstance;
	wDesc.nCmdShow = nCmdShow;
	wDesc.height = 720;
	wDesc.width = 1280;
	_Window = std::make_unique<Window>(wDesc);

	// Deviceの作成
	DX12Device::DeviceDesc dDesc;
	dDesc.enableDebugLayer = true;
	_Device = std::make_unique<DX12Device>();
	_Device->Initialize(dDesc);

	// Descriptorの作成
	_Descriptor = std::make_unique<Descriptor>();
	_Descriptor->Initialize(_Device.get(), 4096);

	// CommandQueueの作成
	_CommandQueue = std::make_unique<DX12CommandQueue>();
	_CommandQueue->Initialize(_Device->GetDevice(), D3D12_COMMAND_LIST_TYPE_DIRECT);

	// SwapChainの作成
	DX12SwapChain::SwapChainDesc sDesc;
	sDesc.width = _Window->GetDesc().width;
	sDesc.height = _Window->GetDesc().height;
	sDesc.hwnd = _Window->GetHwnd();
	sDesc.device = _Device.get();
	sDesc.commandQueue = _CommandQueue.get();
	sDesc.BufferCount = 2;
	_SwapChain = std::make_unique<DX12SwapChain>();
	_SwapChain->Initialize(sDesc);

	// Rendererの作成
	_Renderer = std::make_unique<DX12Renderer>();
	_Renderer->Initialize(_Device.get(), _CommandQueue.get(), _SwapChain.get());

	// ImGuiLayerの作成
	_ImGuiLayer = std::make_unique<ImGuiLayer>();
	_ImGuiLayer->Initialize({ _Window->GetHwnd(), _Device->GetDevice(), _CommandQueue->GetCommandQueue(), 2, DXGI_FORMAT_R8G8B8A8_UNORM, 64 });
	_Window->AddWndProc(_ImGuiLayer->WndProcHandler);

	// CapsuleAssetProtectorの初期化
	CapsuleAssetProtector::SetKey("YourSecureKeyHere");
	CapsuleAssetProtector::InitDesc desc;
	desc.device = _Device->GetDevice();
	desc.commandQueue = _CommandQueue->GetCommandQueue();
	desc.srvHeap = _Descriptor->GetDescriptorHeap();
	desc.userData = _Descriptor.get();
	desc.allocateSrv = [](void* user) -> uint32_t { return static_cast<Descriptor*>(user)->Allocate(); };
	desc.freeSrv = [](void* user, uint32_t index) { static_cast<Descriptor*>(user)->Free(index); };
	CapsuleAssetProtector::Initialize(desc);
}

void App::Update()
{
}

void App::Render()
{
	_Renderer->BeginFrame(_Descriptor->GetDescriptorHeap());



	_ImGuiLayer->BeginFrame();

	_ImGuiLayer->EndFrame(_Renderer->GetCommandList());

	_Renderer->EndFrame();
}

void App::UnLoad()
{
	_Renderer->Release();
}
