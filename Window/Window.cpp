#include "Window.h"

Window::Window(WindowDesc desc)
{
	WNDCLASSEX wcex = {};
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.hInstance = desc.hInstance;
	wcex.lpszClassName = desc.windowClass.className;
	wcex.style = desc.windowClass.style;

	if (desc.windowClass.hIcon == nullptr)
		wcex.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
	else
		wcex.hIcon = desc.windowClass.hIcon;

	if (desc.windowClass.hIconSm == nullptr)
		wcex.hIconSm = wcex.hIcon;
	else
		wcex.hIconSm = desc.windowClass.hIconSm;

	if (desc.windowClass.hCursor == nullptr)
		wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	else
		wcex.hCursor = desc.windowClass.hCursor;

	wcex.hbrBackground = desc.windowClass.hbrBackground;

	if (!RegisterClassEx(&wcex))
	{
		MessageBox(nullptr, L"ウィンドウクラスの登録に失敗しました。", L"エラー", MB_OK | MB_ICONERROR);
		return;
	}


	RECT rc = { 0,0, desc.width, desc.height };
	AdjustWindowRectEx(&rc, desc.windowSettings.style, false, desc.windowSettings.exStyle);

	_hWnd = CreateWindowEx(
		desc.windowSettings.exStyle,
		desc.windowClass.className,
		desc.windowSettings.title,
		desc.windowSettings.style,
		CW_USEDEFAULT, CW_USEDEFAULT,
		rc.right - rc.left, rc.bottom - rc.top,
		HWND_DESKTOP, NULL, desc.hInstance, nullptr);
	if(_hWnd == nullptr)
	{
		MessageBox(nullptr, L"ウィンドウの作成に失敗しました。", L"エラー", MB_OK | MB_ICONERROR);
		return;
	}

	ShowWindow(_hWnd, desc.nCmdShow);
	UpdateWindow(_hWnd);

	_desc = desc;
	_mainWindowInstance = this;
}

Window::~Window()
{

}

