#include "App.h"

#include "../ImGui/ImGuiLayer.h"

Window* g_window = nullptr;
DX12Device* g_device = nullptr;
DX12CommandQueue* g_commandQueue = nullptr;
DX12SwapChain* g_swapChain = nullptr;
DX12Renderer* g_renderer = nullptr;
Descriptor* g_descriptor = nullptr;

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	Window::WindowDesc desc;
	desc.hInstance = hInstance;
	desc.nCmdShow = nCmdShow;
	g_window = new Window(desc);

	DX12Device::DeviceDesc deviceDesc;
	g_device = new DX12Device();
	g_device->Initialize(deviceDesc);

	g_descriptor = new Descriptor();
	g_descriptor->Initialize(g_device);

	g_commandQueue = new DX12CommandQueue();
	g_commandQueue->Initialize(g_device->GetDevice(), D3D12_COMMAND_LIST_TYPE_DIRECT);

	DX12SwapChain::SwapChainDesc swapChainDesc;
	swapChainDesc.width = g_window->GetDesc().width;
	swapChainDesc.height = g_window->GetDesc().height;
	swapChainDesc.hwnd = g_window->GetHwnd();
	swapChainDesc.device = g_device;
	swapChainDesc.commandQueue = g_commandQueue;
	swapChainDesc.BufferCount = 2;

	g_swapChain = new DX12SwapChain();
	g_swapChain->Initialize(swapChainDesc);	
	g_renderer = new DX12Renderer();
	g_renderer->Initialize(g_device, g_commandQueue, g_swapChain);

	ImGuiLayer imGuiLayer;
	imGuiLayer.Initialize({ g_window->GetHwnd(), g_device->GetDevice(), g_commandQueue->GetCommandQueue(), g_swapChain->GetBufferCount(), DXGI_FORMAT_R8G8B8A8_UNORM, 64 });
	g_window->AddWndProc(ImGuiLayer::WndProcHandler);	
	MSG msg;
	while (GetMessage(&msg, nullptr, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);

		// •`‰æˆ—
		g_renderer->BeginFrame(g_descriptor->GetDescriptorHeap());
		imGuiLayer.BeginFrame();
	
		ImGui::Begin("Hello, ImGui!");
		ImGui::Text("This is some useful text.");
		ImGui::End();
		

		imGuiLayer.EndFrame(g_renderer->GetCommandList());
		g_renderer->EndFrame();
	}

	imGuiLayer.Shutdown();
	g_renderer->Release();

	delete g_renderer;
	g_renderer = nullptr;

	delete g_swapChain;
	g_swapChain = nullptr;

	delete g_commandQueue;
	g_commandQueue = nullptr;

	delete g_descriptor;
	g_descriptor = nullptr;

	delete g_device;
	g_device = nullptr;

	delete g_window;
	g_window = nullptr;

	return (int)msg.wParam;
}

Window* GetWindow()
{
	return g_window;
}

DX12Device* GetDevice()
{
	return g_device;
}

Descriptor* GetDescriptor()
{
	return g_descriptor;
}

DX12CommandQueue* GetCommandQueue()
{
	return g_commandQueue;
}

DX12SwapChain* GetSwapChain()
{
	return g_swapChain;
}

DX12Renderer* GetRenderer()
{
	return g_renderer;
}
