#pragma once

#include <Windows.h>
#include <string>
class Window
{
public:
	struct WindowClass {
		LPCWSTR className = L"WindowClass";
		UINT style = CS_CLASSDC | BN_DBLCLK;
		HICON hIcon = nullptr;
		HICON hIconSm = nullptr;
		HCURSOR hCursor = nullptr;
		HBRUSH hbrBackground = nullptr;
	};

	struct WindowSettings
	{
		DWORD style = WS_OVERLAPPEDWINDOW;
		DWORD exStyle = WS_EX_OVERLAPPEDWINDOW;
		WCHAR title[256] = L"Window";
	};

	struct WindowDesc
	{
		HINSTANCE hInstance;
		int nCmdShow;
		UINT width = 800;
		UINT height = 600;
		WindowClass windowClass;
		WindowSettings windowSettings;
	};
public:
	Window(WindowDesc desc);
	~Window();

	/**
	 * @brief メインウィンドウのポインターを取得する
	 * @return Window* メインウィンドウのポインター
	 */
	HWND GetHwnd() const { return _hWnd; }

	/**
	 * @brief ウィンドウの設定情報を取得する
	 * @return WindowDesc ウィンドウの設定情報
	 */
	WindowDesc GetDesc() const { return _desc; }

protected:
	HWND _hWnd; // ウィンドウハンドル
	WindowDesc _desc; // ウィンドウの設定情報
private:
	// メインウインドウのポインターを保持するメンバ変数
	static Window* _mainWindowInstance;
};