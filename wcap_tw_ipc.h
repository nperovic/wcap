#pragma once

#include "wcap.h"

#define WCAP_TW_PIPE_NAME L"\\\\.\\pipe\\wcap-tw.control"
#define WCAP_TW_RECORDING_EVENT_NAME L"Local\\wcap-tw.is-recording"

typedef enum WcapTwCaptureMode
{
	WCAP_TW_CAPTURE_NONE,
	WCAP_TW_CAPTURE_WINDOW,
	WCAP_TW_CAPTURE_MONITOR,
	WCAP_TW_CAPTURE_REGION,
}
WcapTwCaptureMode;

typedef enum WcapTwIpcCommand
{
	WCAP_TW_IPC_STATUS,
	WCAP_TW_IPC_WATCH,
	WCAP_TW_IPC_START,
	WCAP_TW_IPC_STOP,
}
WcapTwIpcCommand;

typedef struct WcapTwIpcStatus
{
	BOOL Recording;
	WcapTwCaptureMode Mode;
	HWND Window;
	WCHAR Path[MAX_PATH];
	DWORD Width;
	DWORD Height;
	DWORD FramerateNum;
	DWORD FramerateDen;
	DWORD DurationMsec;
	DWORD BitrateKbps;
	UINT64 SizeBytes;
	DWORD DroppedFrames;
}
WcapTwIpcStatus;

typedef struct WcapTwIpcRequest
{
	WcapTwIpcCommand Command;
	WcapTwCaptureMode Mode;
	HWND Window;
	WcapTwIpcStatus Status;
	DWORD ExitCode;
	HANDLE Completed;
}
WcapTwIpcRequest;

BOOL WcapTwIpc_RunClientCommandLine(void);
BOOL WcapTwIpc_Init(HWND Window, UINT CommandMessage);
BOOL WcapTwIpc_StartServer(void);
void WcapTwIpc_StopServer(void);
void WcapTwIpc_CompleteRequest(WcapTwIpcRequest* Request, DWORD ExitCode, const WcapTwIpcStatus* Status);
void WcapTwIpc_NotifyRecordingStarted(const WcapTwIpcStatus* Status);
void WcapTwIpc_NotifyRecordingStopped(LPCWSTR Path);
