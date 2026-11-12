#pragma once
#include <Windows.h>
#include <memory>

class Window;
class DX12Device;
class Descriptor;
class DX12CommandQueue;
class DX12SwapChain;
class DX12Renderer;
class ImGuiLayer;

class App
{
public:
	App() = default;
	~App() = default;
public:
	void Init(HINSTANCE hInstance,int nCmdShow);
	void Update();
	void Render();
	void UnLoad();
private:
	std::unique_ptr<Window> _Window;
	std::unique_ptr<DX12Device> _Device;
	std::unique_ptr<Descriptor> _Descriptor;
	std::unique_ptr<DX12CommandQueue> _CommandQueue;
	std::unique_ptr<DX12SwapChain> _SwapChain;
	std::unique_ptr<DX12Renderer> _Renderer;
	std::unique_ptr<ImGuiLayer> _ImGuiLayer;
};

