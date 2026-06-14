#include "wcap.h"
#include "wcap_config.h"
#include "wcap_audio_capture.h"
#include "wcap_screen_capture.h"
#include "wcap_encoder.h"

#include <dxgi1_6.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <windowsx.h>

#pragma comment (lib, "ntdll")
#pragma comment (lib, "kernel32")
#pragma comment (lib, "user32")
#pragma comment (lib, "gdi32")
#pragma comment (lib, "msimg32")
#pragma comment (lib, "dxgi")
#pragma comment (lib, "d3dcompiler")
#pragma comment (lib, "d3d11")
#pragma comment (lib, "dwmapi")
#pragma comment (lib, "shell32")
#pragma comment (lib, "shlwapi")
#pragma comment (lib, "mfplat")
#pragma comment (lib, "mfuuid")
#pragma comment (lib, "mfreadwrite")
#pragma comment (lib, "evr")
#pragma comment (lib, "strmiids")
#pragma comment (lib, "ksuser")
#pragma comment (lib, "mmdevapi")
#pragma comment (lib, "ole32")
#pragma comment (lib, "wmcodecdspuuid")
#pragma comment (lib, "avrt")
#pragma comment (lib, "uxtheme")
#pragma comment (lib, "OneCore")
#pragma comment (lib, "CoreMessaging")

#if defined(_M_AMD64)
// this is needed to be able to use Nvidia Media Foundation encoders on Optimus systems
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
#endif

#define WM_WCAP_ALREADY_RUNNING (WM_USER+1)
#define WM_WCAP_STOP_CAPTURE    (WM_USER+2)
#define WM_WCAP_TRAY_TITLE      (WM_USER+3)
#define WM_WCAP_COMMAND         (WM_USER+4)

#define WCAP_AUDIO_CAPTURE_TIMER    1
#define WCAP_AUDIO_CAPTURE_INTERVAL 100 // msec

#define WCAP_VIDEO_UPDATE_TIMER     2
#define WCAP_VIDEO_UPDATE_INTERVAL  100 // msec

#define CMD_WCAP           1
#define CMD_QUIT           2
#define CMD_SETTINGS       3
#define CMD_RECORD_MONITOR 4
#define CMD_RECORD_WINDOW  5
#define CMD_RECORD_REGION  6
#define CMD_STOP           7

#define HOT_RECORD_WINDOW  1
#define HOT_RECORD_MONITOR 2
#define HOT_RECORD_REGION  3

#define WCAP_RESIZE_NONE 0
#define WCAP_RESIZE_TL   1
#define WCAP_RESIZE_T    2
#define WCAP_RESIZE_TR   3
#define WCAP_RESIZE_L    4
#define WCAP_RESIZE_M    5
#define WCAP_RESIZE_R    6
#define WCAP_RESIZE_BL   7
#define WCAP_RESIZE_B    8
#define WCAP_RESIZE_BR   9

#define WCAP_UI_FONT      L"Segoe UI"
#define WCAP_UI_FONT_SIZE 16

#define WCAP_RECT_BORDER 2

// constants
static WCHAR gConfigPath[MAX_PATH];
static LARGE_INTEGER gTickFreq;
static HICON gIcon1;
static HICON gIcon2;
static UINT WM_TASKBARCREATED;
static HCURSOR gCursorArrow;
static HCURSOR gCursorClick;
static HCURSOR gCursorResize[10];
static HFONT gFont;
static HFONT gFontBold;

// recording state
static BOOL gRecordingStarted;
static BOOL gRecording;
static DWORD gRecordingLimitFramerate;
static DWORD gRecordingDroppedFrames;
static UINT64 gRecordingLastFrame;
static UINT64 gRecordingNextEncode;
static UINT64 gRecordingNextTooltip;
static EXECUTION_STATE gRecordingState;
static WCHAR gRecordingPath[MAX_PATH];

// when selecting rectangle to record
static HMONITOR gRectMonitor;
static HDC gRectContext;
static HDC gRectDarkContext;
static HBITMAP gRectBitmap;
static HBITMAP gRectDarkBitmap;
static DWORD gRectWidth;
static DWORD gRectHeight;
static BOOL gRectSelected;
static POINT gRectSelection[2];
static POINT gRectMousePos;
static int gRectResize;
static int gRectSetSize[2];
static BOOL gRectSetSizeClick;
static BOOL gSelectingWindow;
static HWND gSelectedWindow;
static HWND gWindowSelectionClick;
static RECT gSelectedWindowRect;
static POINT gSelectionOrigin;

// globals
static HWND gWindow;
static Config gConfig;
static AudioCapture gAudio;
static ScreenCapture gCapture;
static Encoder gEncoder;

static void ShowNotification(LPCWSTR Message, LPCWSTR Title, DWORD Flags)
{
	NOTIFYICONDATAW Data =
	{
		.cbSize = sizeof(Data),
		.hWnd = gWindow,
		.uFlags = NIF_INFO | NIF_TIP,
		.dwInfoFlags = Flags, // NIIF_INFO, NIIF_WARNING, NIIF_ERROR
	};
	StrCpyNW(Data.szTip, WCAP_TITLE, _countof(Data.szTip));
	StrCpyNW(Data.szInfo, Message, _countof(Data.szInfo));
	StrCpyNW(Data.szInfoTitle, Title ? Title : WCAP_TITLE, _countof(Data.szInfoTitle));
	Shell_NotifyIconW(NIM_MODIFY, &Data);
}

static void UpdateTrayTitle(LPCWSTR Title)
{
	NOTIFYICONDATAW Data =
	{
		.cbSize = sizeof(Data),
		.hWnd = gWindow,
		.uFlags = NIF_TIP,
	};
	StrCpyNW(Data.szTip, Title, _countof(Data.szTip));
	Shell_NotifyIconW(NIM_MODIFY, &Data);
}

static void UpdateTrayIcon(HICON Icon)
{
	NOTIFYICONDATAW Data =
	{
		.cbSize = sizeof(Data),
		.hWnd = gWindow,
		.uFlags = NIF_ICON,
		.hIcon = Icon,
	};
	Shell_NotifyIconW(NIM_MODIFY, &Data);
}

static void AddTrayIcon(HWND Window)
{
	NOTIFYICONDATAW Data =
	{
		.cbSize = sizeof(Data),
		.hWnd = Window,
		.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP,
		.uCallbackMessage = WM_WCAP_COMMAND,
		.hIcon = gIcon1,
	};
	StrCpyNW(Data.szTip, WCAP_TITLE, _countof(Data.szTip));
	Shell_NotifyIconW(NIM_ADD, &Data);
}

static void RemoveTrayIcon(HWND Window)
{
	NOTIFYICONDATAW Data =
	{
		.cbSize = sizeof(Data),
		.hWnd = Window,
	};
	Shell_NotifyIconW(NIM_DELETE, &Data);
}

static void ShowFileInFolder(LPCWSTR Filename)
{
	SFGAOF Flags;
	PIDLIST_ABSOLUTE List;
	if (Filename[0] && SUCCEEDED(SHParseDisplayName(Filename, NULL, &List, 0, &Flags)))
	{
		HR(SHOpenFolderAndSelectItems(List, 0, NULL, 0));
		CoTaskMemFree(List);
	}
}

static void StartRecording(ID3D11Device* Device, HWND Window)
{
	SYSTEMTIME Time;
	GetLocalTime(&Time);

	int Error = SHCreateDirectoryExW(NULL, gConfig.OutputFolder, NULL);
	if (Error != ERROR_SUCCESS && Error != ERROR_FILE_EXISTS && Error != ERROR_ALREADY_EXISTS)
	{
		ShowNotification(Config_IsTaiwan(&gConfig) ? L"無法建立輸出資料夾。" : L"Cannot create output folder!",
			Config_IsTaiwan(&gConfig) ? L"無法開始錄影" : L"Cannot Start Recording", NIIF_WARNING);
		ScreenCapture_Stop(&gCapture);
		ID3D11Device_Release(Device);
		return;
	}

	WCHAR Filename[256];
	StrFormat(Filename, L"%04u%02u%02u_%02u%02u%02u.mp4", Time.wYear, Time.wMonth, Time.wDay, Time.wHour, Time.wMinute, Time.wSecond);

	StrCpyW(gRecordingPath, gConfig.OutputFolder);
	PathAppendW(gRecordingPath, Filename);

	DWM_TIMING_INFO Info = { .cbSize = sizeof(Info) };
	HR(DwmGetCompositionTimingInfo(NULL, &Info));

	DWORD FramerateNum = Info.rateCompose.uiNumerator;
	DWORD FramerateDen = Info.rateCompose.uiDenominator;
	if (gConfig.VideoMaxFramerate > 0 && gConfig.VideoMaxFramerate * FramerateDen < FramerateNum)
	{
		// limit rate only if max framerate is specified and it is lower than compositor framerate
		gRecordingLimitFramerate = gConfig.VideoMaxFramerate;
		FramerateNum = gConfig.VideoMaxFramerate;
		FramerateDen = 1;
	}
	else
	{
		gRecordingLimitFramerate = 0;
	}

	EncoderConfig EncConfig =
	{
		.Width = gCapture.Rect.right - gCapture.Rect.left,
		.Height = gCapture.Rect.bottom - gCapture.Rect.top,
		.FramerateNum = FramerateNum,
		.FramerateDen = FramerateDen,
		.Config = &gConfig,
	};

	if (gConfig.CaptureAudio)
	{
		HWND ApplicationWindow = gConfig.ApplicationLocalAudio && AudioCapture_CanCaptureApplicationLocal() ? Window : NULL;
		if (!AudioCapture_Start(&gAudio, ApplicationWindow))
		{
			ShowNotification(Config_IsTaiwan(&gConfig) ? L"無法擷取音訊。" : L"Cannot capture audio!",
				Config_IsTaiwan(&gConfig) ? L"無法開始錄影" : L"Cannot Start Recording", NIIF_WARNING);
			ScreenCapture_Stop(&gCapture);
			ID3D11Device_Release(Device);
			return;
		}
		EncConfig.AudioFormat = gAudio.Format;
	}

	if (!Encoder_Start(&gEncoder, Device, gRecordingPath, &EncConfig))
	{
		if (gConfig.CaptureAudio)
		{
			AudioCapture_Stop(&gAudio);
		}
		ScreenCapture_Stop(&gCapture);
		ID3D11Device_Release(Device);
		return;
	}

	gRecordingNextTooltip = 0;
	gRecordingNextEncode = 0;
	gRecordingLastFrame = 0;
	gRecordingDroppedFrames = 0;
	ScreenCapture_Start(&gCapture, gConfig.MouseCursor, gConfig.ShowRecordingBorder, gConfig.IncludeSecondaryWindows);

	if (gConfig.CaptureAudio)
	{
		SetTimer(gWindow, WCAP_AUDIO_CAPTURE_TIMER, WCAP_AUDIO_CAPTURE_INTERVAL, NULL);
	}
	SetTimer(gWindow, WCAP_VIDEO_UPDATE_TIMER, WCAP_VIDEO_UPDATE_INTERVAL, NULL);

	UpdateTrayIcon(gIcon2);
	gRecordingState = SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
	gRecording = TRUE;

	ID3D11Device_Release(Device);
}

static void EncodeCapturedAudio(void)
{
	if (gEncoder.StartTime == 0)
	{
		// we don't know when first video frame starts yet
		return;
	}

	AudioCaptureData Data;
	while (AudioCapture_GetData(&gAudio, &Data, gEncoder.StartTime))
	{
		UINT32 FramesToEncode = (UINT32)Data.Count;
		if (Data.Time < gEncoder.StartTime)
		{
			const UINT32 SampleRate = gAudio.Format->nSamplesPerSec;
			const UINT32 BytesPerFrame = gAudio.Format->nBlockAlign;

			// figure out how much time (100nsec units) and frame count to skip from current buffer
			UINT64 TimeToSkip = gEncoder.StartTime - Data.Time;
			UINT32 FramesToSkip = (UINT32)((TimeToSkip * SampleRate - 1) / MF_UNITS_PER_SECOND + 1);
			if (FramesToSkip < FramesToEncode)
			{
				// need to skip part of captured data
				Data.Time += FramesToSkip * MF_UNITS_PER_SECOND / SampleRate;
				FramesToEncode -= FramesToSkip;
				if (Data.Samples)
				{
					Data.Samples = (BYTE*)Data.Samples + FramesToSkip * BytesPerFrame;
				}
			}
			else
			{
				// need to skip all of captured data
				FramesToEncode = 0;
			}
		}
		if (FramesToEncode != 0)
		{
			Assert(Data.Time >= gEncoder.StartTime);
			Encoder_NewSamples(&gEncoder, Data.Samples, FramesToEncode, Data.Time, gTickFreq.QuadPart);
		}
		AudioCapture_ReleaseData(&gAudio, &Data);
	}
}

static void StopRecording(void)
{
	gRecording = FALSE;
	SetThreadExecutionState(gRecordingState);

	if (gConfig.CaptureAudio)
	{
		KillTimer(gWindow, WCAP_AUDIO_CAPTURE_TIMER);
		AudioCapture_Flush(&gAudio);
		EncodeCapturedAudio();
		AudioCapture_Stop(&gAudio);
	}
	KillTimer(gWindow, WCAP_VIDEO_UPDATE_TIMER);

	ScreenCapture_Stop(&gCapture);
	Encoder_Stop(&gEncoder);
	if (gConfig.OpenFolder)
	{
		ShowFileInFolder(gRecordingPath);
	}

	SetWindowPos(gWindow, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE);
	SetWindowLongW(gWindow, GWL_EXSTYLE, 0);

	UpdateTrayIcon(gIcon1);
	UpdateTrayTitle(WCAP_TITLE);
}

static ID3D11Device* CreateDevice(void)
{
	IDXGIAdapter* Adapter = NULL;

	if (gConfig.HardwareEncoder)
	{
		IDXGIFactory* Factory;
		if (SUCCEEDED(CreateDXGIFactory(&IID_IDXGIFactory, (void**)&Factory)))
		{
			IDXGIFactory6* Factory6;
			if (SUCCEEDED(IDXGIFactory_QueryInterface(Factory, &IID_IDXGIFactory6, (void**)&Factory6)))
			{
				DXGI_GPU_PREFERENCE Preference = gConfig.HardwarePreferIntegrated ? DXGI_GPU_PREFERENCE_MINIMUM_POWER : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
				if (FAILED(IDXGIFactory6_EnumAdapterByGpuPreference(Factory6, 0, Preference, &IID_IDXGIAdapter, &Adapter)))
				{
					// just to be safe
					Adapter = NULL;
				}
				IDXGIFactory6_Release(Factory6);
			}
			IDXGIFactory_Release(Factory);
		}
	}

	ID3D11Device* Device;

	UINT flags = 0;
#ifndef NDEBUG
	flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
	// if adapter is selected then driver type must be unknown
	D3D_DRIVER_TYPE Driver = Adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
	if (FAILED(D3D11CreateDevice(Adapter, Driver, NULL, flags, (D3D_FEATURE_LEVEL[]) { D3D_FEATURE_LEVEL_11_0 }, 1, D3D11_SDK_VERSION, &Device, NULL, NULL)))
	{
		ShowNotification(Config_IsTaiwan(&gConfig) ? L"無法建立 D3D11 裝置。" : L"Cannot create D3D11 device!",
			Config_IsTaiwan(&gConfig) ? L"錯誤" : L"Error", NIIF_ERROR);
		Device = NULL;
	}
	if (Adapter)
	{
		IDXGIAdapter_Release(Adapter);
	}

	if (flags & D3D11_CREATE_DEVICE_DEBUG)
	{
		ID3D11InfoQueue* Info;
		if (SUCCEEDED(ID3D11Device_QueryInterface(Device, &IID_ID3D11InfoQueue, &Info)))
		{
			ID3D11InfoQueue_SetBreakOnSeverity(Info, D3D11_MESSAGE_SEVERITY_CORRUPTION, TRUE);
			ID3D11InfoQueue_SetBreakOnSeverity(Info, D3D11_MESSAGE_SEVERITY_ERROR, TRUE);
			ID3D11InfoQueue_Release(Info);
		}
	}

	return Device;
}

static void CaptureWindow(HWND Window)
{
	if (Window == NULL)
	{
		ShowNotification(Config_IsTaiwan(&gConfig) ? L"尚未選取視窗。" : L"No window is selected!",
			Config_IsTaiwan(&gConfig) ? L"無法開始錄影" : L"Cannot Start Recording", NIIF_WARNING);
		return;
	}

	// figure out who is owner of child window if somehow child window is selected (happens for fancy winamp skins)
	HWND Parent = GetParent(Window);
	while (Parent != NULL)
	{
		Window = Parent;
		Parent = GetParent(Window);
	}

	DWORD Affinity;
	BOOL Success = GetWindowDisplayAffinity(Window, &Affinity);
	Assert(Success);

	if (Affinity != WDA_NONE)
	{
		ShowNotification(Config_IsTaiwan(&gConfig) ? L"此視窗已設為不允許擷取。" : L"Window is excluded from capture!",
			Config_IsTaiwan(&gConfig) ? L"無法開始錄影" : L"Cannot Start Recording", NIIF_WARNING);
		return;
	}

	LONG ExStyle = GetWindowLongW(Window, GWL_EXSTYLE);
	if (ExStyle & WS_EX_TOOLWINDOW)
	{
		ShowNotification(Config_IsTaiwan(&gConfig) ? L"無法擷取工具列視窗。" : L"Cannot capture toolbar window!",
			Config_IsTaiwan(&gConfig) ? L"無法開始錄影" : L"Cannot Start Recording", NIIF_WARNING);
		return;
	}

	ID3D11Device* Device = CreateDevice();
	if (!Device)
	{
		return;
	}

	if (!ScreenCapture_CreateForWindow(&gCapture, Device, Window, gConfig.OnlyClientArea, !gConfig.KeepRoundedWindowCorners))
	{
		ID3D11Device_Release(Device);
		ShowNotification(Config_IsTaiwan(&gConfig) ? L"無法錄製選取的視窗。" : L"Cannot record selected window!",
			Config_IsTaiwan(&gConfig) ? L"錯誤" : L"Error", NIIF_WARNING);
		return;
	}

	StartRecording(Device, Window);
}

typedef struct WindowAtPoint
{
	POINT Point;
	HWND Window;
	RECT Rect;
}
WindowAtPoint;

static BOOL CALLBACK FindWindowAtPoint(HWND Window, LPARAM Parameter)
{
	WindowAtPoint* Selection = (WindowAtPoint*)Parameter;
	if (Window == gWindow || !IsWindowVisible(Window) || IsIconic(Window))
	{
		return TRUE;
	}

	DWORD Cloaked = 0;
	if (SUCCEEDED(DwmGetWindowAttribute(Window, DWMWA_CLOAKED, &Cloaked, sizeof(Cloaked))) && Cloaked)
	{
		return TRUE;
	}

	RECT Rect;
	if (FAILED(DwmGetWindowAttribute(Window, DWMWA_EXTENDED_FRAME_BOUNDS, &Rect, sizeof(Rect))))
	{
		GetWindowRect(Window, &Rect);
	}

	if (PtInRect(&Rect, Selection->Point))
	{
		Selection->Window = Window;
		Selection->Rect = Rect;
		return FALSE;
	}
	return TRUE;
}

static void UpdateWindowSelection(void)
{
	POINT Mouse;
	GetCursorPos(&Mouse);

	WindowAtPoint Selection = { .Point = Mouse };
	EnumWindows(&FindWindowAtPoint, (LPARAM)&Selection);
	if (Selection.Window)
	{
		OffsetRect(&Selection.Rect, -gSelectionOrigin.x, -gSelectionOrigin.y);
	}

	if (Selection.Window != gSelectedWindow ||
		!EqualRect(&Selection.Rect, &gSelectedWindowRect))
	{
		gSelectedWindow = Selection.Window;
		if (Selection.Window)
		{
			gSelectedWindowRect = Selection.Rect;
		}
		else
		{
			SetRectEmpty(&gSelectedWindowRect);
		}
		InvalidateRect(gWindow, NULL, FALSE);
	}
}

static void CaptureWindowSelectionInit(void)
{
	int X = GetSystemMetrics(SM_XVIRTUALSCREEN);
	int Y = GetSystemMetrics(SM_YVIRTUALSCREEN);
	DWORD Width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
	DWORD Height = GetSystemMetrics(SM_CYVIRTUALSCREEN);

	HDC DeviceContext = GetDC(NULL);
	if (DeviceContext == NULL)
	{
		ShowNotification(Config_IsTaiwan(&gConfig) ? L"無法取得螢幕的 HDC。" : L"Error getting desktop HDC!",
			Config_IsTaiwan(&gConfig) ? L"無法選取視窗" : L"Cannot Select Window", NIIF_WARNING);
		return;
	}

	HDC MemoryContext = CreateCompatibleDC(DeviceContext);
	HBITMAP MemoryBitmap = CreateCompatibleBitmap(DeviceContext, Width, Height);
	Assert(MemoryContext && MemoryBitmap);
	SelectObject(MemoryContext, MemoryBitmap);
	BitBlt(MemoryContext, 0, 0, Width, Height, DeviceContext, X, Y, SRCCOPY);

	HDC MemoryDarkContext = CreateCompatibleDC(DeviceContext);
	HBITMAP MemoryDarkBitmap = CreateCompatibleBitmap(DeviceContext, Width, Height);
	Assert(MemoryDarkContext && MemoryDarkBitmap);
	SelectObject(MemoryDarkContext, MemoryDarkBitmap);

	BLENDFUNCTION Blend =
	{
		.BlendOp = AC_SRC_OVER,
		.SourceConstantAlpha = 0x40,
	};
	AlphaBlend(MemoryDarkContext, 0, 0, Width, Height, MemoryContext, 0, 0, Width, Height, Blend);
	ReleaseDC(NULL, DeviceContext);

	gRectContext = MemoryContext;
	gRectDarkContext = MemoryDarkContext;
	gRectBitmap = MemoryBitmap;
	gRectDarkBitmap = MemoryDarkBitmap;
	gRectWidth = Width;
	gRectHeight = Height;
	gSelectingWindow = TRUE;
	gSelectedWindow = NULL;
	gWindowSelectionClick = NULL;
	SetRectEmpty(&gSelectedWindowRect);
	gSelectionOrigin = (POINT){ X, Y };

	SetCursor(gCursorClick);
	SetWindowPos(gWindow, HWND_TOPMOST, X, Y, Width, Height, SWP_SHOWWINDOW);
	SetForegroundWindow(gWindow);
	UpdateWindowSelection();
}

static void CaptureMonitor(void)
{
	POINT Mouse;
	GetCursorPos(&Mouse);

	HMONITOR Monitor = MonitorFromPoint(Mouse, MONITOR_DEFAULTTONULL);
	if (Monitor == NULL)
	{
		ShowNotification(Config_IsTaiwan(&gConfig) ? L"找不到指定的螢幕。" : L"Unknown monitor!",
			Config_IsTaiwan(&gConfig) ? L"無法開始錄影" : L"Cannot Start Recording", NIIF_WARNING);
		return;
	}

	ID3D11Device* Device = CreateDevice();
	if (!Device)
	{
		return;
	}

	if (!ScreenCapture_CreateForMonitor(&gCapture, Device, Monitor, NULL))
	{
		ShowNotification(Config_IsTaiwan(&gConfig) ? L"無法錄製選取的螢幕。" : L"Cannot record selected monitor!",
			Config_IsTaiwan(&gConfig) ? L"錯誤" : L"Error", NIIF_WARNING);
		return;
	}

	StartRecording(Device, NULL);
}

static void CaptureRegionInit(void)
{
	POINT Mouse;
	GetCursorPos(&Mouse);

	HMONITOR Monitor = MonitorFromPoint(Mouse, MONITOR_DEFAULTTONULL);
	if (Monitor == NULL)
	{
		ShowNotification(Config_IsTaiwan(&gConfig) ? L"找不到指定的螢幕。" : L"Unknown monitor!",
			Config_IsTaiwan(&gConfig) ? L"無法開始錄影" : L"Cannot Start Recording", NIIF_WARNING);
		return;
	}

	MONITORINFOEXW Info = { .cbSize = sizeof(Info) };
	GetMonitorInfoW(Monitor, (LPMONITORINFO)&Info);

	HDC DeviceContext = CreateDCW(L"DISPLAY", Info.szDevice, NULL, NULL);
	if (DeviceContext == NULL)
	{
		ShowNotification(Config_IsTaiwan(&gConfig) ? L"無法取得螢幕的 HDC。" : L"Error getting HDC of monitor!",
			Config_IsTaiwan(&gConfig) ? L"無法開始錄影" : L"Cannot Start Recording", NIIF_WARNING);
		return;
	}

	DWORD Width = Info.rcMonitor.right - Info.rcMonitor.left;
	DWORD Height = Info.rcMonitor.bottom - Info.rcMonitor.top;

	// capture image from desktop

	HDC MemoryContext = CreateCompatibleDC(DeviceContext);
	Assert(MemoryContext);

	HBITMAP MemoryBitmap = CreateCompatibleBitmap(DeviceContext, Width, Height);
	Assert(MemoryBitmap);

	SelectObject(MemoryContext, MemoryBitmap);
	BitBlt(MemoryContext, 0, 0, Width, Height, DeviceContext, 0, 0, SRCCOPY);

	// prepare darkened image by doing alpha blend

	HDC MemoryDarkContext = CreateCompatibleDC(DeviceContext);
	Assert(MemoryDarkContext);

	HBITMAP MemoryDarkBitmap = CreateCompatibleBitmap(DeviceContext, Width, Height);
	Assert(MemoryDarkBitmap);

	BLENDFUNCTION Blend =
	{
		.BlendOp = AC_SRC_OVER,
		.SourceConstantAlpha = 0x40,
	};

	SelectObject(MemoryDarkContext, MemoryDarkBitmap);
	AlphaBlend(MemoryDarkContext, 0, 0, Width, Height, MemoryContext, 0, 0, Width, Height, Blend);

	// done

	DeleteDC(DeviceContext);

	gRectMonitor = Monitor;
	gRectContext = MemoryContext;
	gRectDarkContext = MemoryDarkContext;
	gRectBitmap = MemoryBitmap;
	gRectDarkBitmap = MemoryDarkBitmap;
	gRectWidth = Width;
	gRectHeight = Height;
	gRectSelected = FALSE;
	gRectResize = WCAP_RESIZE_NONE;
	gRectSetSize[0] = gRectSetSize[1] = 0;
	gRectSetSizeClick = FALSE;

	SetCursor(gCursorResize[WCAP_RESIZE_NONE]);
	SetWindowPos(gWindow, HWND_TOPMOST, Info.rcMonitor.left, Info.rcMonitor.top, Width, Height, SWP_SHOWWINDOW);
	SetForegroundWindow(gWindow);
	InvalidateRect(gWindow, NULL, FALSE);
}

static void CaptureRegionRelease(void)
{
	SetWindowPos(gWindow, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE);
	SetWindowLongW(gWindow, GWL_EXSTYLE, 0);
	gSelectingWindow = FALSE;
	gSelectedWindow = NULL;
	gWindowSelectionClick = NULL;
	SetRectEmpty(&gSelectedWindowRect);

	if (gRectContext)
	{
		DeleteDC(gRectContext);
		gRectContext = NULL;

		DeleteObject(gRectBitmap);
		gRectBitmap = NULL;

		DeleteDC(gRectDarkContext);
		gRectDarkContext = NULL;

		DeleteObject(gRectDarkBitmap);
		gRectDarkBitmap = NULL;
	}
}

static void CaptureRegionDone(void)
{
	SetCursor(gCursorArrow);
	ReleaseCapture();

	CaptureRegionRelease();
}

static void CaptureRegion(void)
{
	CaptureRegionDone();

	MONITORINFO Info = { .cbSize = sizeof(Info) };
	GetMonitorInfoW(gRectMonitor, &Info);

	RECT Rect =
	{
		.left   = gRectSelection[0].x,
		.right  = gRectSelection[1].x,
		.top    = gRectSelection[0].y,
		.bottom = gRectSelection[1].y,
	};

	LONG ExStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT;
	SetWindowLongW(gWindow, GWL_EXSTYLE, ExStyle);
	SetLayeredWindowAttributes(gWindow, RGB(255, 0, 255), 0, LWA_COLORKEY);

	ID3D11Device* Device = CreateDevice();
	if (!Device)
	{
		CaptureRegionRelease();
		return;
	}

	if (!ScreenCapture_CreateForMonitor(&gCapture, Device, gRectMonitor, &Rect))
	{
		ShowNotification(Config_IsTaiwan(&gConfig) ? L"無法錄製螢幕。" : L"Cannot record monitor!",
			Config_IsTaiwan(&gConfig) ? L"錯誤" : L"Error", NIIF_WARNING);
		CaptureRegionRelease();
		return;
	}

	StartRecording(Device, NULL);

	if (gRecording)
	{
		int X = Info.rcMonitor.left + Rect.left - (WCAP_RECT_BORDER + 1);
		int Y = Info.rcMonitor.top + Rect.top - (WCAP_RECT_BORDER + 1);
		int W = Rect.right - Rect.left + 2 * (WCAP_RECT_BORDER + 1);
		int H = Rect.bottom - Rect.top + 2 * (WCAP_RECT_BORDER + 1);
		SetWindowPos(gWindow, HWND_TOPMOST, X, Y, W, H, SWP_SHOWWINDOW);
		InvalidateRect(gWindow, NULL, FALSE);
	}
	else
	{
		CaptureRegionRelease();
	}
}

static int GetPointResize(int X, int Y)
{
	int BorderX = GetSystemMetrics(SM_CXSIZEFRAME);
	int BorderY = GetSystemMetrics(SM_CYSIZEFRAME);

	int X0 = min(gRectSelection[0].x, gRectSelection[1].x);
	int Y0 = min(gRectSelection[0].y, gRectSelection[1].y);
	int X1 = max(gRectSelection[0].x, gRectSelection[1].x);
	int Y1 = max(gRectSelection[0].y, gRectSelection[1].y);

	POINT P = { X, Y };

	RECT TL = { X0 - BorderX, Y0 - BorderY, X0 + BorderX, Y0 + BorderY };
	if (PtInRect(&TL, P)) return WCAP_RESIZE_TL;

	RECT TR = { X1 - BorderX, Y0 - BorderY, X1 + BorderX, Y0 + BorderY };
	if (PtInRect(&TR, P)) return WCAP_RESIZE_TR;

	RECT BL = { X0 - BorderX, Y1 - BorderY, X0 + BorderX, Y1 + BorderY };
	if (PtInRect(&BL, P)) return WCAP_RESIZE_BL;

	RECT BR = { X1 - BorderX, Y1 - BorderY, X1 + BorderX, Y1 + BorderY };
	if (PtInRect(&BR, P)) return WCAP_RESIZE_BR;

	RECT T = { X0, Y0 - BorderY, X1, Y0 + BorderY };
	if (PtInRect(&T, P)) return WCAP_RESIZE_T;

	RECT B = { X0, Y1 - BorderY, X1, Y1 + BorderY };
	if (PtInRect(&B, P)) return WCAP_RESIZE_B;

	RECT L = { X0 - BorderX, Y0, X0 + BorderX, Y1 };
	if (PtInRect(&L, P)) return WCAP_RESIZE_L;

	RECT R = { X1 - BorderX, Y0, X1 + BorderX, Y1 };
	if (PtInRect(&R, P)) return WCAP_RESIZE_R;

	RECT M = { X0, Y0, X1, Y1 };
	if (PtInRect(&M, P)) return WCAP_RESIZE_M;

	return WCAP_RESIZE_NONE;
}

void DisableHotKeys(void)
{
	UnregisterHotKey(gWindow, HOT_RECORD_MONITOR);
	UnregisterHotKey(gWindow, HOT_RECORD_WINDOW);
	UnregisterHotKey(gWindow, HOT_RECORD_REGION);
}

BOOL EnableHotKeys(void)
{
	BOOL Success = TRUE;
	if (gConfig.ShortcutMonitor)
	{
		Success = Success && RegisterHotKey(gWindow, HOT_RECORD_MONITOR, HOT_GET_MOD(gConfig.ShortcutMonitor), HOT_GET_KEY(gConfig.ShortcutMonitor));
	}
	if (gConfig.ShortcutWindow)
	{
		Success = Success && RegisterHotKey(gWindow, HOT_RECORD_WINDOW, HOT_GET_MOD(gConfig.ShortcutWindow), HOT_GET_KEY(gConfig.ShortcutWindow));
	}
	if (gConfig.ShortcutRegion)
	{
		Success = Success && RegisterHotKey(gWindow, HOT_RECORD_REGION, HOT_GET_MOD(gConfig.ShortcutRegion), HOT_GET_KEY(gConfig.ShortcutRegion));
	}
	return Success;
}

static void AdjustRectSizeMultipleOf2(int Adjust, int Ref)
{
	int W = gRectSelection[Ref].x - gRectSelection[Adjust].x;
	W = (W + (W > 0)) & ~1;
	gRectSelection[Adjust].x = gRectSelection[Ref].x - W;

	int H = gRectSelection[Ref].y - gRectSelection[Adjust].y;
	H = (H + (H > 0)) & ~1;
	gRectSelection[Adjust].y = gRectSelection[Ref].y - H;
}

static LRESULT CALLBACK WindowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
	if (Message == WM_CREATE)
	{
		HR(BufferedPaintInit());
		AddTrayIcon(Window);
		return 0;
	}
	else if (Message == WM_DESTROY)
	{
		if (gRecording)
		{
			StopRecording();
		}
		RemoveTrayIcon(Window);
		PostQuitMessage(0);
		return 0;
	}
	else if (Message == WM_CLOSE)
	{
		if (gRectContext)
		{
			CaptureRegionDone();
		}
		return 0;
	}
	else if (Message == WM_ACTIVATEAPP)
	{
		if (gRectContext)
		{
			if (WParam == FALSE)
			{
				CaptureRegionDone();
				return 0;
			}
		}
	}
	else if (Message == WM_KEYDOWN)
	{
		if (gRectContext)
		{
			if (WParam == VK_ESCAPE)
			{
				CaptureRegionDone();
				return 0;
			}
			else if (WParam == VK_RETURN)
			{
				if (gSelectingWindow && gSelectedWindow)
				{
					HWND SelectedWindow = gSelectedWindow;
					CaptureRegionDone();
					gRecordingStarted = TRUE;
					CaptureWindow(SelectedWindow);
					gRecordingStarted = FALSE;
				}
				else if (gRectSelected)
				{
					CaptureRegion();
				}
				return 0;
			}
		}
	}
	else if (Message == WM_LBUTTONDOWN)
	{
		if (gRectContext)
		{
			if (gSelectingWindow)
			{
				gWindowSelectionClick = gSelectedWindow;
				SetCapture(Window);
			}
			else if (gRectSetSize[0])
			{
				gRectSetSizeClick = TRUE;
				gRectSelection[1].x = gRectSelection[0].x + gRectSetSize[0];
				gRectSelection[1].y = gRectSelection[0].y + gRectSetSize[1];
				InvalidateRect(Window, NULL, FALSE);
			}
			else
			{
				int X = GET_X_LPARAM(LParam);
				int Y = GET_Y_LPARAM(LParam);

				int Resize = gRectSelected ? GetPointResize(X, Y) : WCAP_RESIZE_NONE;
				if (Resize == WCAP_RESIZE_NONE)
				{
					// inital rectangle will be empty
					gRectSelection[0].x = gRectSelection[1].x = X;
					gRectSelection[0].y = gRectSelection[1].y = Y;
					gRectSelected = FALSE;

					InvalidateRect(Window, NULL, FALSE);
				}
				else
				{
					// resizing direction
					gRectMousePos = (POINT){ X, Y };
				}

				gRectResize = Resize;
				SetCapture(Window);
			}
			return 0;
		}
	}
	else if (Message == WM_LBUTTONUP)
	{
		if (gRectContext)
		{
			if (gSelectingWindow)
			{
				HWND SelectedWindow = gWindowSelectionClick;
				gWindowSelectionClick = NULL;
				ReleaseCapture();
				if (SelectedWindow)
				{
					CaptureRegionDone();
					gRecordingStarted = TRUE;
					CaptureWindow(SelectedWindow);
					gRecordingStarted = FALSE;
				}
			}
			else if (gRectSetSizeClick)
			{
				gRectSetSizeClick = FALSE;
			}
			else
			{
				if (gRectSelected)
				{
					// fix the selected rectangle coordinates, so next resizing starts on the correct side
					int X0 = min(gRectSelection[0].x, gRectSelection[1].x);
					int Y0 = min(gRectSelection[0].y, gRectSelection[1].y);
					int X1 = max(gRectSelection[0].x, gRectSelection[1].x);
					int Y1 = max(gRectSelection[0].y, gRectSelection[1].y);
					gRectSelection[0] = (POINT){ X0, Y0 };
					gRectSelection[1] = (POINT){ X1, Y1 };
				}
				ReleaseCapture();
			}
			return 0;
		}
	}
	else if (Message == WM_MOUSEMOVE)
	{
		if (gRectContext)
		{
			if (gSelectingWindow)
			{
				SetCursor(gCursorClick);
				UpdateWindowSelection();
				return 0;
			}

			int X = GET_X_LPARAM(LParam);
			int Y = GET_Y_LPARAM(LParam);

			if (gRectSetSize[0])
			{
				SetCursor(gCursorClick);
				InvalidateRect(Window, NULL, FALSE);
			}
			else if (gRectSetSizeClick)
			{
				InvalidateRect(Window, NULL, FALSE);
			}
			else if (WParam & MK_LBUTTON)
			{
				BOOL Update = FALSE;

				if (gRectResize == WCAP_RESIZE_TL || gRectResize == WCAP_RESIZE_L || gRectResize == WCAP_RESIZE_BL)
				{
					// left moved
					gRectSelection[0].x = X;
					AdjustRectSizeMultipleOf2(0, 1);
					Update = TRUE;
				}
				else if (gRectResize == WCAP_RESIZE_TR || gRectResize == WCAP_RESIZE_R || gRectResize == WCAP_RESIZE_BR)
				{
					// right moved
					gRectSelection[1].x = X;
					AdjustRectSizeMultipleOf2(1, 0);
					Update = TRUE;
				}

				if (gRectResize == WCAP_RESIZE_TL || gRectResize == WCAP_RESIZE_T || gRectResize == WCAP_RESIZE_TR)
				{
					// top moved
					gRectSelection[0].y = Y;
					AdjustRectSizeMultipleOf2(0, 1);
					Update = TRUE;
				}
				else if (gRectResize == WCAP_RESIZE_BL || gRectResize == WCAP_RESIZE_B || gRectResize == WCAP_RESIZE_BR)
				{
					// bottom moved
					gRectSelection[1].y = Y;
					AdjustRectSizeMultipleOf2(1, 0);
					Update = TRUE;
				}

				if (gRectResize == WCAP_RESIZE_M)
				{
					// if moving whole rectangle update both
					int DX = X - gRectMousePos.x;
					int DY = Y - gRectMousePos.y;
					gRectMousePos = (POINT){ X, Y };

					gRectSelection[0].x += DX;
					gRectSelection[0].y += DY;
					gRectSelection[1].x += DX;
					gRectSelection[1].y += DY;

					Update = TRUE;
				}
				else if (gRectResize == WCAP_RESIZE_NONE)
				{
					// no resize means we're selecting initial rectangle
					gRectSelection[1].x = X;
					gRectSelection[1].y = Y;
					AdjustRectSizeMultipleOf2(1, 0);
					if (gRectSelection[0].x != gRectSelection[1].x && gRectSelection[0].y != gRectSelection[1].y)
					{
						// when we have non-zero size rectangle, we're good with initial stage
						gRectSelected = TRUE;
						Update = TRUE;
					}
				}

				if (Update)
				{
					InvalidateRect(Window, NULL, FALSE);
				}
			}
			else
			{
				int Resize = gRectSelected ? GetPointResize(X, Y) : WCAP_RESIZE_NONE;
				SetCursor(gCursorResize[Resize]);

				if (Resize == WCAP_RESIZE_NONE)
				{
					// in case hovering over resize text
					InvalidateRect(Window, NULL, FALSE);
				}
			}

			return 0;
		}
	}
	else if (Message == WM_TIMER)
	{
		if (gRecording)
		{
			if (WParam == WCAP_AUDIO_CAPTURE_TIMER)
			{
				EncodeCapturedAudio();
				return 0;
			}
			else if (WParam == WCAP_VIDEO_UPDATE_TIMER)
			{
				LARGE_INTEGER Time;
				QueryPerformanceCounter(&Time);
				Encoder_Update(&gEncoder, Time.QuadPart, gTickFreq.QuadPart);
				return 0;
			}
		}
	}
	else if (Message == WM_POWERBROADCAST)
	{
		if (WParam == PBT_APMQUERYSUSPEND)
		{
			if (gRecording)
			{
				if (LParam & 1)
				{
					// reject request to suspend when recording
					return BROADCAST_QUERY_DENY;
				}
				else
				{
					// if cannot prevent suspend, need to stop recording
					StopRecording();
				}
			}
			else
			{
				// allow to suspend when not recording
			}
		}
		return TRUE;
	}
	else if (Message == WM_WCAP_COMMAND)
	{
		if (LOWORD(LParam) == WM_RBUTTONUP)
		{
			HMENU Menu = CreatePopupMenu();
			Assert(Menu);

			AppendMenuW(Menu, MF_STRING, CMD_WCAP, WCAP_TITLE);
			AppendMenuW(Menu, MF_SEPARATOR, 0, NULL);
			if (gRecording)
			{
				AppendMenuW(Menu, MF_STRING, CMD_STOP,
					Config_IsTaiwan(&gConfig) ? L"停止錄影" : L"Stop Recording");
			}
			else
			{
				AppendMenuW(Menu, MF_STRING, CMD_RECORD_MONITOR,
					Config_IsTaiwan(&gConfig) ? L"開始錄製螢幕" : L"Start Monitor Recording");
				AppendMenuW(Menu, MF_STRING, CMD_RECORD_WINDOW,
					Config_IsTaiwan(&gConfig) ? L"開始錄製視窗" : L"Start Window Recording");
				AppendMenuW(Menu, MF_STRING, CMD_RECORD_REGION,
					Config_IsTaiwan(&gConfig) ? L"開始錄製區域" : L"Start Region Recording");
			}
			AppendMenuW(Menu, MF_SEPARATOR, 0, NULL);
			AppendMenuW(Menu, MF_STRING | (gRecording ? MF_DISABLED : 0), CMD_SETTINGS,
				Config_IsTaiwan(&gConfig) ? L"設定" : L"Settings");
			AppendMenuW(Menu, MF_STRING, CMD_QUIT, Config_IsTaiwan(&gConfig) ? L"結束" : L"Exit");

			POINT Mouse;
			GetCursorPos(&Mouse);

			Config_RefreshDarkMenus(Window);
			SetForegroundWindow(Window);
			int Command = TrackPopupMenu(Menu, TPM_RETURNCMD | TPM_NONOTIFY, Mouse.x, Mouse.y, 0, Window, NULL);
			if (Command == CMD_WCAP)
			{
				ShellExecuteW(NULL, L"open", WCAP_URL, NULL, NULL, SW_SHOWNORMAL);
			}
			else if (Command == CMD_RECORD_MONITOR)
			{
				gRecordingStarted = TRUE;
				CaptureMonitor();
				gRecordingStarted = FALSE;
			}
			else if (Command == CMD_RECORD_WINDOW)
			{
				CaptureWindowSelectionInit();
			}
			else if (Command == CMD_RECORD_REGION)
			{
				gRecordingStarted = TRUE;
				CaptureRegionInit();
				gRecordingStarted = FALSE;
			}
			else if (Command == CMD_STOP)
			{
				StopRecording();
			}
			else if (Command == CMD_QUIT)
			{
				DestroyWindow(Window);
			}
			else if (Command == CMD_SETTINGS)
			{
				if (Config_ShowDialog(&gConfig))
				{
					Config_Save(&gConfig, gConfigPath);
					DisableHotKeys();
					EnableHotKeys();
				}
			}

			DestroyMenu(Menu);
		}
		else if (LOWORD(LParam) == WM_LBUTTONDBLCLK)
		{
			if (!gRecording)
			{
				if (Config_ShowDialog(&gConfig))
				{
					Config_Save(&gConfig, gConfigPath);
					DisableHotKeys();
					EnableHotKeys();
				}
			}
		}
		else if (LOWORD(LParam) == NIN_BALLOONUSERCLICK)
		{
			// TODO: no idea how to prevent this happening for right-click on tray icon...
			ShowFileInFolder(gRecordingPath);
		}
		return 0;
	}
	else if (Message == WM_HOTKEY)
	{
		if (gRecording)
		{
			StopRecording();
		}
		else if (!gRecordingStarted)
		{
			if (gRectContext == NULL)
			{
				if (WParam == HOT_RECORD_WINDOW)
				{
					gRecordingStarted = TRUE;
					CaptureWindow(GetForegroundWindow());
					gRecordingStarted = FALSE;
				}
				else if (WParam == HOT_RECORD_MONITOR)
				{
					gRecordingStarted = TRUE;
					CaptureMonitor();
					gRecordingStarted = FALSE;
				}
				else if (WParam == HOT_RECORD_REGION)
				{
					gRecordingStarted = TRUE;
					CaptureRegionInit();
					gRecordingStarted = FALSE;
				}
			}
		}
		return 0;
	}
	else if (Message == WM_WCAP_TRAY_TITLE)
	{
		if (gRecording)
		{
			UINT64 FileSize;
			DWORD Bitrate, LengthMsec;
			Encoder_GetStats(&gEncoder, &Bitrate, &LengthMsec, &FileSize);

			WCHAR LengthText[128];
			StrFromTimeIntervalW(LengthText, _countof(LengthText), LengthMsec, 6);

			WCHAR SizeText[128];
			StrFormatByteSizeW(FileSize, SizeText, _countof(SizeText));

			WCHAR Text[1024];
			StrFormat(Text, Config_IsTaiwan(&gConfig)
				? L"錄影中：%dx%d @ %.2f\n長度：%ls\n位元率：%u kbit/s\n大小：%ls\n捨棄影格數：%u"
				: L"Recording: %dx%d @ %.2f\nLength: %ls\nBitrate: %u kbit/s\nSize: %ls\nDropped Frames: %u",
				gEncoder.OutputWidth, gEncoder.OutputHeight,
				(float)gEncoder.FramerateNum / (float)gEncoder.FramerateDen,
				LengthText,
				Bitrate,
				SizeText,
				gRecordingDroppedFrames);

			UpdateTrayTitle(Text);
		}
		return 0;
	}
	else if (Message == WM_WCAP_STOP_CAPTURE)
	{
		if (gRecording)
		{
			StopRecording();
		}
		return 0;
	}
	else if (Message == WM_WCAP_ALREADY_RUNNING)
	{
		ShowNotification(Config_IsTaiwan(&gConfig) ? L"wcap-tw 已在執行中。" : L"wcap-tw is already running!", NULL, NIIF_INFO);
		return 0;
	}
	else if (Message == WM_TASKBARCREATED)
	{
		// in case taskbar was re-created (explorer.exe crashed) add our icon back
		AddTrayIcon(Window);
		return 0;
	}
	else if (Message == WM_ERASEBKGND)
	{
		return 1;
	}
	else if (Message == WM_PAINT)
	{
		PAINTSTRUCT Paint;
		HDC PaintContext = BeginPaint(Window, &Paint);

		HDC Context;
		HPAINTBUFFER BufferedPaint = BeginBufferedPaint(PaintContext, &Paint.rcPaint, BPBF_COMPATIBLEBITMAP, NULL, &Context);
		if (BufferedPaint)
		{
			if (gRectContext)
			{
				{
					int X = Paint.rcPaint.left;
					int Y = Paint.rcPaint.top;
					int W = Paint.rcPaint.right - Paint.rcPaint.left;
					int H = Paint.rcPaint.bottom - Paint.rcPaint.top;

					// draw darkened screenshot
					BitBlt(Context, X, Y, W, H, gRectDarkContext, X, Y, SRCCOPY);
				}

				if (gSelectingWindow)
				{
					if (gSelectedWindow)
					{
						RECT Rect = gSelectedWindowRect;
						IntersectRect(&Rect, &Rect, &(RECT){ 0, 0, (LONG)gRectWidth, (LONG)gRectHeight });
						BitBlt(Context, Rect.left, Rect.top, Rect.right - Rect.left, Rect.bottom - Rect.top,
							gRectContext, Rect.left, Rect.top, SRCCOPY);

						RECT Border = Rect;
						InflateRect(&Border, 2, 2);
						HBRUSH BorderBrush = CreateSolidBrush(RGB(255, 255, 0));
						FrameRect(Context, &Border, BorderBrush);
						DeleteObject(BorderBrush);
					}

					SelectObject(Context, gFontBold);
					SetTextAlign(Context, TA_TOP | TA_CENTER);
					SetTextColor(Context, RGB(255, 255, 0));
					SetBkMode(Context, TRANSPARENT);
					LPCWSTR Text = Config_IsTaiwan(&gConfig)
						? L"在要錄製的視窗上按一下左鍵；按 Esc 取消。"
						: L"Left-click the window to record; press Esc to cancel.";
					ExtTextOutW(Context, gRectWidth / 2, 24, 0, NULL, Text, lstrlenW(Text), NULL);
				}
				else if (gRectSelected)
				{
					// draw selected rectangle
					int X0 = min(gRectSelection[0].x, gRectSelection[1].x);
					int Y0 = min(gRectSelection[0].y, gRectSelection[1].y);
					int X1 = max(gRectSelection[0].x, gRectSelection[1].x);
					int Y1 = max(gRectSelection[0].y, gRectSelection[1].y);
					BitBlt(Context, X0, Y0, X1 - X0, Y1 - Y0, gRectContext, X0, Y0, SRCCOPY);

					RECT Rect = { X0 - 1, Y0 - 1, X1 + 1, Y1 + 1 };
					FrameRect(Context, &Rect, GetStockObject(WHITE_BRUSH));

					WCHAR Text[128];
					int TextLength = StrFormat(Text, L"%d x %d", X1 - X0, Y1 - Y0);

					SelectObject(Context, gFontBold);
					SetTextAlign(Context, TA_TOP | TA_RIGHT);
					SetTextColor(Context, RGB(255, 255, 255));
					SetBkMode(Context, TRANSPARENT);
					ExtTextOutW(Context, X1, Y1, 0, NULL, Text, TextLength, NULL);

					SelectObject(Context, gFontBold);
					SetTextAlign(Context, TA_BOTTOM | TA_LEFT);
					SetTextColor(Context, RGB(255, 255, 255));

					LPCWSTR TextResize = Config_IsTaiwan(&gConfig) ? L"調整大小：  " : L"Resize:  ";

					SIZE Size;
					int TextResizeLength = lstrlenW(TextResize);
					GetTextExtentPoint32W(Context, TextResize, TextResizeLength, &Size);
					ExtTextOutW(Context, X0, Y0, 0, NULL, TextResize, TextResizeLength, NULL);

					int X = X0;
					SelectObject(Context, gFont);

					POINT CursorPos;
					GetCursorPos(&CursorPos);
					ScreenToClient(Window, &CursorPos);

					gRectSetSize[0] = gRectSetSize[1] = 0;

					int Sizes[][2] = { { 800, 600 }, { 1280, 720 }, { 1920, 1080 }, { 2560, 1440 } };
					for (int i=0; i<_countof(Sizes); i++)
					{
						X += Size.cx;

						TextLength = StrFormat(Text, L"%dx%d  ", Sizes[i][0], Sizes[i][1]);
						GetTextExtentPoint32W(Context, Text, TextLength, &Size);

						RECT Rect = { X, Y0 - Size.cy, X + Size.cx, Y0 };
						BOOL Hovering = PtInRect(&Rect, CursorPos);
						SetTextColor(Context, Hovering ? RGB(255, 255, 255) : RGB(192, 192, 192));
						ExtTextOutW(Context, X, Y0, 0, NULL, Text, TextLength, NULL);

						if (Hovering)
						{
							gRectSetSize[0] = Sizes[i][0];
							gRectSetSize[1] = Sizes[i][1];
							SetCursor(gCursorClick);
						}
					}
				}
				else
				{
					// draw initial message when no rectangle is selected
					SelectObject(Context, gFont);
					SelectObject(Context, GetStockObject(DC_PEN));
					SelectObject(Context, GetStockObject(DC_BRUSH));

					LPCWSTR Line1 = Config_IsTaiwan(&gConfig)
						? L"請用滑鼠選取區域，然後按 Enter 開始錄影。"
						: L"Select region with the mouse and press ENTER to start capture.";
					LPCWSTR Line2 = Config_IsTaiwan(&gConfig) ? L"按 Esc 取消。" : L"Press ESC to cancel.";

					const WCHAR* Lines[] = { Line1, Line2 };
					const int LineLengths[] = { lstrlenW(Line1), lstrlenW(Line2) };
					int Widths[_countof(Lines)];
					int Height;

					int TotalWidth = 0;
					int TotalHeight = 0;
					for (int i = 0; i < _countof(Lines); i++)
					{
						SIZE Size;
						GetTextExtentPoint32W(Context, Lines[i], LineLengths[i], &Size);
						Widths[i] = Size.cx;
						Height = Size.cy;
						TotalWidth = max(TotalWidth, Size.cx);
						TotalHeight += Size.cy;
					}
					TotalWidth += 2 * Height;
					TotalHeight += Height;

					int MsgX = (gRectWidth - TotalWidth) / 2;
					int MsgY = (gRectHeight - TotalHeight) / 2;

					SetDCPenColor(Context, RGB(255, 255, 255));
					SetDCBrushColor(Context, RGB(0, 0, 128));
					Rectangle(Context, MsgX, MsgY, MsgX + TotalWidth, MsgY + TotalHeight);

					SetTextAlign(Context, TA_TOP | TA_CENTER);
					SetTextColor(Context, RGB(255, 255, 0));
					SetBkMode(Context, TRANSPARENT);
					int Y = MsgY + Height / 2;
					int X = gRectWidth / 2;
					for (int i = 0; i < _countof(Lines); i++)
					{
						ExtTextOutW(Context, X, Y, 0, NULL, Lines[i], LineLengths[i], NULL);
						Y += Height;
					}
				}
			}
			else
			{
				RECT Rect;
				GetClientRect(Window, &Rect);

				HBRUSH BorderBrush = CreateSolidBrush(RGB(255, 255, 0));
				Assert(BorderBrush);
				FillRect(Context, &Rect, BorderBrush);
				DeleteObject(BorderBrush);

				Rect.left += WCAP_RECT_BORDER;
				Rect.top += WCAP_RECT_BORDER;
				Rect.right -= WCAP_RECT_BORDER;
				Rect.bottom -= WCAP_RECT_BORDER;

				HBRUSH ColorKeyBrush = CreateSolidBrush(RGB(255, 0, 255));
				Assert(ColorKeyBrush);
				FillRect(Context, &Rect, ColorKeyBrush);
				DeleteObject(ColorKeyBrush);

				FrameRect(Context, &Rect, GetStockObject(BLACK_BRUSH));
			}

			EndBufferedPaint(BufferedPaint, TRUE);
		}

		EndPaint(Window, &Paint);
		return 0;
	}

	return DefWindowProcW(Window, Message, WParam, LParam);
}

static bool OnCaptureFrame(ScreenCapture* Capture, ScreenCaptureFrame* Frame)
{
	if (Frame == NULL)
	{
		PostMessageW(gWindow, WM_WCAP_STOP_CAPTURE, 0, 0);
		return true;
	}

	BOOL DoEncode = TRUE;
	DWORD LimitFramerate = gRecordingLimitFramerate;
	if (LimitFramerate != 0)
	{
		if (Frame->Time * LimitFramerate < gRecordingNextEncode)
		{
			DoEncode = FALSE;
		}
		else
		{
			if (gRecordingNextEncode == 0)
			{
				gRecordingNextEncode = Frame->Time * LimitFramerate;
			}
			gRecordingNextEncode += gTickFreq.QuadPart;
		}
	}

	// ignore frames if it comes from the past
	if (Frame->Time <= gRecordingLastFrame)
	{
		DoEncode = FALSE;
	}
	gRecordingLastFrame = Frame->Time;

	if (DoEncode)
	{
		if (!Encoder_NewFrame(&gEncoder, Frame->Texture, Frame->Rect, Frame->Time, gTickFreq.QuadPart))
		{
			// TODO: maybe highlight tray icon when droppped frames are increasing too much?
			gRecordingDroppedFrames++;
		}
	}

	if (gConfig.EnableLimitLength || gConfig.EnableLimitSize)
	{
		BOOL Stop = FALSE;

		if (gConfig.EnableLimitLength)
		{
			if (Frame->Time - gEncoder.StartTime >= (UINT64)(gConfig.LimitLength * gTickFreq.QuadPart))
			{
				Stop = TRUE;
			}
		}
		if (gConfig.EnableLimitSize && !Stop)
		{
			UINT64 FileSize;
			DWORD Bitrate, LengthMsec;
			Encoder_GetStats(&gEncoder, &Bitrate, &LengthMsec, &FileSize);

			// reserve 0.5% for mp4 format overhead (probably an overestimate)
			if (1000 * FileSize >= (995ULL * gConfig.LimitSize) << 20)
			{
				Stop = TRUE;
			}
		}

		if (Stop)
		{
			PostMessageW(gWindow, WM_WCAP_STOP_CAPTURE, 0, 0);
			return true;
		}
	}

	// update tray title with stats once every second
	if (gRecordingNextTooltip == 0)
	{
		gRecordingNextTooltip = Frame->Time + gTickFreq.QuadPart;
	}
	else if (Frame->Time >= gRecordingNextTooltip)
	{
		gRecordingNextTooltip += gTickFreq.QuadPart;

		// do the update, but not from frame callback to minimize time when texture is used
		PostMessageW(gWindow, WM_WCAP_TRAY_TITLE, 0, 0);
	}

	return true;
}

#ifndef NDEBUG
int WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR cmdline, int cmdshow)
#else
void WinMainCRTStartup()
#endif
{
	WNDCLASSEXW WindowClass =
	{
		.cbSize = sizeof(WindowClass),
		.lpfnWndProc = WindowProc,
		.hInstance = GetModuleHandleW(NULL),
		.lpszClassName = L"wcap_tw_window_class",
	};

	HWND Existing = FindWindowW(WindowClass.lpszClassName, NULL);
	if (Existing)
	{
		PostMessageW(Existing, WM_WCAP_ALREADY_RUNNING, 0, 0);
		ExitProcess(0);
	}

	if (!ScreenCapture_IsSupported())
	{
		MessageBoxW(NULL, L"Windows 10 Version 1903, May 2019 Update (19H1) or newer is required!\n\n需要 Windows 10 版本 1903（2019 年 5 月更新，19H1）或更新版本。", WCAP_TITLE, MB_ICONEXCLAMATION);
		ExitProcess(0);
	}

	Config_EnableDarkMode();

	GetModuleFileNameW(NULL, gConfigPath, _countof(gConfigPath));
	PathRenameExtensionW(gConfigPath, L".ini");

	HR(CoInitializeEx(0, COINIT_APARTMENTTHREADED));

	Config_Defaults(&gConfig);
	if (!PathFileExistsW(gConfigPath))
	{
		WCHAR LegacyConfigPath[MAX_PATH];
		StrCpyW(LegacyConfigPath, gConfigPath);
		StrCpyW(PathFindFileNameW(LegacyConfigPath), WCAP_LEGACY_CONFIG_FILENAME);
		if (PathFileExistsW(LegacyConfigPath))
		{
			Config_Load(&gConfig, LegacyConfigPath);
			Config_Save(&gConfig, gConfigPath);
		}
		else
		{
			Config_Load(&gConfig, gConfigPath);
		}
	}
	else
	{
		Config_Load(&gConfig, gConfigPath);
	}
	ScreenCapture_Create(&gCapture, &OnCaptureFrame, false);
	Encoder_Init(&gEncoder);

	QueryPerformanceFrequency(&gTickFreq);

	gCursorArrow = LoadCursor(NULL, IDC_ARROW);
	gCursorClick = LoadCursor(NULL, IDC_HAND);
	gCursorResize[WCAP_RESIZE_NONE] = LoadCursor(NULL, IDC_CROSS);
	gCursorResize[WCAP_RESIZE_M]    = LoadCursor(NULL, IDC_SIZEALL);
	gCursorResize[WCAP_RESIZE_T]    = gCursorResize[WCAP_RESIZE_B]  = LoadCursor(NULL, IDC_SIZENS);
	gCursorResize[WCAP_RESIZE_L]    = gCursorResize[WCAP_RESIZE_R]  = LoadCursor(NULL, IDC_SIZEWE);
	gCursorResize[WCAP_RESIZE_TL]   = gCursorResize[WCAP_RESIZE_BR] = LoadCursor(NULL, IDC_SIZENWSE);
	gCursorResize[WCAP_RESIZE_TR]   = gCursorResize[WCAP_RESIZE_BL] = LoadCursor(NULL, IDC_SIZENESW);

	gFont = CreateFontW(-WCAP_UI_FONT_SIZE, 0, 0, 0, FW_NORMAL,
		FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH, WCAP_UI_FONT);
	Assert(gFont);

	gFontBold = CreateFontW(-WCAP_UI_FONT_SIZE, 0, 0, 0, FW_BOLD,
		FALSE, FALSE, FALSE,DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH, WCAP_UI_FONT);
	Assert(gFontBold);

	gIcon1 = LoadIconW(WindowClass.hInstance, MAKEINTRESOURCEW(1));
	gIcon2 = LoadIconW(WindowClass.hInstance, MAKEINTRESOURCEW(2));
	Assert(gIcon1 && gIcon2);

	WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");
	Assert(WM_TASKBARCREATED);

	ATOM Atom = RegisterClassExW(&WindowClass);
	Assert(Atom);

	gWindow = CreateWindowExW(
		0, WindowClass.lpszClassName, WCAP_TITLE, WS_POPUP,
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
		NULL, NULL, WindowClass.hInstance, NULL);
	if (!gWindow)
	{
		ExitProcess(0);
	}
	if (!EnableHotKeys())
	{
		MessageBoxW(NULL,
			Config_IsTaiwan(&gConfig)
				? L"無法註冊 wcap-tw 快捷鍵。\n其他應用程式可能已使用相同的快捷鍵。\n請檢查並調整設定。"
				: L"Cannot register wcap-tw keyboard shortcuts.\nSome other application might already use shortcuts.\nPlease check & adjust the settings!",
			WCAP_TITLE, MB_ICONEXCLAMATION);
	}

	for (;;)
	{
		MSG Message;
		BOOL Result = GetMessageW(&Message, NULL, 0, 0);
		if (Result == 0)
		{
			ExitProcess(0);
		}
		Assert(Result > 0);

		TranslateMessage(&Message);
		DispatchMessageW(&Message);
	}
}
