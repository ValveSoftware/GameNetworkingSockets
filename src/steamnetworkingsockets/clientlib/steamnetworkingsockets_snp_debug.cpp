//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef WIN32
// Put everything in a namespace, so we don't violate the one definition rule
namespace SteamNetworkingSocketsLib {

bool SNPDebugWindowActive() { return false; }
void InitSNPDebugWindow() {};
void ShutdownSNPDebugWindow() {};
void RunFrameSNPDebugWindow() {};
void SetSNPDebugText( int nCols, const char **ppszTextArray ) {};

} // namespace SteamNetworkingSocketsLib

#else //defined( WIN32 )

#include <windows.h>

#include "tier0/platform.h"

#pragma comment(lib, "gdi32.lib" )

// Put everything in a namespace, so we don't violate the one definition rule
namespace SteamNetworkingSocketsLib {

static bool s_bDebugWindowActive = false;

static HWND g_hwnd;

const LONG kMargin = 10;

int g_nCols;
char **g_ppszText;

static void PrintError()
{ 
    // Retrieve the system error message for the last-error code
    LPVOID lpMsgBuf;
    DWORD dw = GetLastError(); 

    FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | 
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        dw,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPTSTR) &lpMsgBuf,
        0, NULL );

    // Display the error message and exit the process
	OutputDebugString( (char *)lpMsgBuf );
    LocalFree(lpMsgBuf);
}

static void UpdateText( HWND hwnd )
{
	PAINTSTRUCT ps;
	HDC hdc = BeginPaint (hwnd, &ps);

	RECT rect;
	GetClientRect(hwnd, &rect);

	//paint client area white to clear last text
	FillRect( hdc, &rect, (HBRUSH)GetStockObject( WHITE_BRUSH ) );//easy as we dont have to clean up a stock object

	if ( g_nCols )
	{
		HFONT hFont = (HFONT)GetStockObject( ANSI_FIXED_FONT );
		SelectObject( hdc, hFont );

		SetTextColor( hdc, 0x00000000 );
		SetBkMode( hdc, TRANSPARENT );

		int nColWidth = ( rect.right - rect.left + 1 ) / g_nCols;

		for ( int i = 0; i < g_nCols; ++i )
		{
			RECT textRect = rect;
			textRect.left = rect.left + nColWidth * i;
			textRect.right = rect.left + nColWidth * ( i + 1 );

			textRect.left += kMargin;
			textRect.top += kMargin;
			textRect.right -= kMargin;
			textRect.bottom -= kMargin;

			DrawText( hdc, g_ppszText[ i ], (int)strlen( g_ppszText[ i ] ), &textRect, DT_WORDBREAK );
		}
	}

	EndPaint (hwnd, &ps);
}

void SetSNPDebugText( int nCols, const char **ppszTextArray )
{
	// free current text
	for ( int i = 0; i < g_nCols; ++i )
	{
		delete [] g_ppszText[i];
	}
	delete []g_ppszText;

	g_nCols = nCols;

	// Allocate and copy the text
	g_ppszText = new char *[nCols];
	for ( int i = 0; i < nCols; ++i )
	{
		auto len = strlen( ppszTextArray[i] );
		g_ppszText[i] = new char[len+1];
		strcpy( g_ppszText[i], ppszTextArray[i] );
	}
	
	RECT rect;
	GetClientRect( g_hwnd, &rect );
	InvalidateRect(g_hwnd, &rect, TRUE );
	UpdateWindow( g_hwnd);		
}

static LRESULT CALLBACK WndProc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
	case WM_CLOSE:
        DestroyWindow (hwnd);
		hwnd = NULL;
        break;

	case WM_DESTROY:
        PostQuitMessage (0);
        break;
        
	case WM_PAINT:
		{
			UpdateText( hwnd );
        }
        break;

	case WM_ERASEBKGND:
		return 1;
    }
    return DefWindowProc (hwnd, msg, wParam, lParam);
}

void InitSNPDebugWindow()
{
	if ( s_bDebugWindowActive )
	{
		return;
	}

	s_bDebugWindowActive = true;
	
	WNDCLASSEX WindowClass;
	WindowClass.cbClsExtra = 0;
	WindowClass.cbWndExtra = 0;
	WindowClass.cbSize = sizeof (WNDCLASSEX);
	WindowClass.lpszClassName = "DebugWindow";
	WindowClass.lpszMenuName = NULL;
	WindowClass.lpfnWndProc = WndProc;
	WindowClass.hIcon = LoadIcon (NULL, IDI_APPLICATION);
	WindowClass.hIconSm = LoadIcon (NULL, IDI_APPLICATION);
	WindowClass.hCursor = LoadCursor (NULL, IDC_ARROW);
	WindowClass.style = 0;
	WindowClass.hbrBackground = (HBRUSH) (COLOR_WINDOW + 1);
	WindowClass.hInstance = GetModuleHandle(NULL);

	if ( !RegisterClassEx(&WindowClass) )
	{
		PrintError();
	}

	g_hwnd = CreateWindowEx (WS_EX_CLIENTEDGE | WS_EX_COMPOSITED,
						   "DebugWindow",
						   "Steam Network Protocol Debug",
						   WS_OVERLAPPEDWINDOW,
						   315, 115,
						   640, 480,
						   NULL,
						   NULL,
						   GetModuleHandle( NULL ),
						   NULL);

	if ( !g_hwnd )
	{
		PrintError();
	}

	ShowWindow (g_hwnd, SW_SHOWNORMAL);
}

void ShutdownSNPDebugWindow()
{
	if ( !s_bDebugWindowActive )
	{
		return;
	}

	s_bDebugWindowActive = false;

	DestroyWindow( g_hwnd );
	g_hwnd = nullptr;
}

void RunFrameSNPDebugWindow()
{
	if ( s_bDebugWindowActive && g_hwnd )
	{
		MSG msg;
		while (PeekMessage (&msg, g_hwnd, 0, 0, PM_REMOVE) > 0)
		{
			TranslateMessage (&msg);
			DispatchMessage (&msg);
		}
	}
}

} // namespace SteamNetworkingSocketsLib

#endif //defined( PLATFORM_WINDOWS )

