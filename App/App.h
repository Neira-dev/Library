#pragma once
#include "../Window/Window.h"
#include "../DirectX12/DX12Device.h"
#include "../DirectX12/Descriptor.h"
#include "../DirectX12/DX12CommandQueue.h"
#include "../DirectX12/DX12SwapChain.h"
#include "../DirectX12/DX12Renderer.h"

Window* GetWindow();
DX12Device* GetDevice();
Descriptor* GetDescriptor();
DX12CommandQueue* GetCommandQueue();
DX12SwapChain* GetSwapChain();
DX12Renderer* GetRenderer();
