#include "wcap_tw_ipc.h"

#include <shellapi.h>
#include <stdarg.h>
#include <stdlib.h>
#include <wchar.h>

#define WCAP_TW_IPC_MAGIC 0x57544350u
#define WCAP_TW_IPC_VERSION 1u
#define WCAP_TW_IPC_MAX_JSON 4096u
#define WCAP_TW_IPC_EVENT_QUEUE_SIZE 64u

typedef struct WcapTwIpcWireRequest
{
	DWORD Magic;
	DWORD Version;
	DWORD Command;
	DWORD Mode;
	UINT64 Window;
}
WcapTwIpcWireRequest;

typedef struct WcapTwIpcWireResponse
{
	DWORD Magic;
	DWORD ExitCode;
	DWORD Length;
}
WcapTwIpcWireResponse;

static HWND gWcapTwIpcWindow;
static UINT gWcapTwIpcCommandMessage;
static HANDLE gWcapTwIpcServerThread;
static HANDLE gWcapTwIpcStopEvent;
static HANDLE gWcapTwIpcChangeEvent;
static HANDLE gWcapTwIpcRecordingEvent;
static CRITICAL_SECTION gWcapTwIpcEventLock;
static volatile LONG gWcapTwIpcEventGeneration;
static char gWcapTwIpcEventJson[WCAP_TW_IPC_EVENT_QUEUE_SIZE][WCAP_TW_IPC_MAX_JSON];
static DWORD gWcapTwIpcEventJsonLength[WCAP_TW_IPC_EVENT_QUEUE_SIZE];

static BOOL WcapTwIpc_ReadAll(HANDLE File, void* Buffer, DWORD Size)
{
	BYTE* Bytes = Buffer;
	while (Size != 0)
	{
		DWORD Read;
		if (!ReadFile(File, Bytes, Size, &Read, NULL) || Read == 0)
		{
			return FALSE;
		}
		Bytes += Read;
		Size -= Read;
	}
	return TRUE;
}

static BOOL WcapTwIpc_WriteAll(HANDLE File, const void* Buffer, DWORD Size)
{
	const BYTE* Bytes = Buffer;
	while (Size != 0)
	{
		DWORD Written;
		if (!WriteFile(File, Bytes, Size, &Written, NULL) || Written == 0)
		{
			return FALSE;
		}
		Bytes += Written;
		Size -= Written;
	}
	return TRUE;
}

static BOOL WcapTwIpc_Append(char* Buffer, DWORD Capacity, DWORD* Length, const char* Text)
{
	DWORD TextLength = (DWORD)lstrlenA(Text);
	if (*Length + TextLength >= Capacity)
	{
		return FALSE;
	}
	CopyMemory(Buffer + *Length, Text, TextLength);
	*Length += TextLength;
	Buffer[*Length] = 0;
	return TRUE;
}

static BOOL WcapTwIpc_AppendFormat(char* Buffer, DWORD Capacity, DWORD* Length, const char* Format, ...)
{
	if (*Length >= Capacity)
	{
		return FALSE;
	}

	va_list Args;
	va_start(Args, Format);
	int Count = _vsnprintf(Buffer + *Length, Capacity - *Length, Format, Args);
	va_end(Args);
	if (Count < 0 || (DWORD)Count >= Capacity - *Length)
	{
		return FALSE;
	}
	*Length += (DWORD)Count;
	return TRUE;
}

static BOOL WcapTwIpc_AppendJsonString(char* Buffer, DWORD Capacity, DWORD* Length, LPCWSTR Text)
{
	if (Text == NULL)
	{
		return WcapTwIpc_Append(Buffer, Capacity, Length, "null");
	}

	char Utf8[MAX_PATH * 4];
	int Utf8Length = WideCharToMultiByte(CP_UTF8, 0, Text, -1, Utf8, sizeof(Utf8), NULL, NULL);
	if (Utf8Length <= 0 || !WcapTwIpc_Append(Buffer, Capacity, Length, "\""))
	{
		return FALSE;
	}

	for (int Index = 0; Index < Utf8Length - 1; Index++)
	{
		unsigned char Character = (unsigned char)Utf8[Index];
		if (Character == '"' || Character == '\\')
		{
			char Escaped[] = { '\\', (char)Character, 0 };
			if (!WcapTwIpc_Append(Buffer, Capacity, Length, Escaped))
			{
				return FALSE;
			}
		}
		else if (Character < 0x20)
		{
			if (!WcapTwIpc_AppendFormat(Buffer, Capacity, Length, "\\u%04X", Character))
			{
				return FALSE;
			}
		}
		else
		{
			char Byte[] = { (char)Character, 0 };
			if (!WcapTwIpc_Append(Buffer, Capacity, Length, Byte))
			{
				return FALSE;
			}
		}
	}
	return WcapTwIpc_Append(Buffer, Capacity, Length, "\"");
}

static const char* WcapTwIpc_ModeName(WcapTwCaptureMode Mode)
{
	switch (Mode)
	{
		case WCAP_TW_CAPTURE_WINDOW: return "window";
		case WCAP_TW_CAPTURE_MONITOR: return "monitor";
		case WCAP_TW_CAPTURE_REGION: return "region";
		default: return "none";
	}
}

static DWORD WcapTwIpc_FormatStatus(const WcapTwIpcStatus* Status, char* Buffer, DWORD Capacity)
{
	DWORD Length = 0;
	if (!Status->Recording)
	{
		if (!WcapTwIpc_Append(Buffer, Capacity, &Length,
			"{\"running\":true,\"recording\":false,\"mode\":\"none\",\"hwnd\":null,\"path\":null}"))
		{
			return 0;
		}
		return Length;
	}

	if (!WcapTwIpc_AppendFormat(Buffer, Capacity, &Length,
		"{\"running\":true,\"recording\":true,\"mode\":\"%s\",\"hwnd\":",
		WcapTwIpc_ModeName(Status->Mode)))
	{
		return 0;
	}
	if (Status->Window)
	{
		if (!WcapTwIpc_AppendFormat(Buffer, Capacity, &Length, "\"0x%08llX\"", (UINT64)(UINT_PTR)Status->Window))
		{
			return 0;
		}
	}
	else if (!WcapTwIpc_Append(Buffer, Capacity, &Length, "null"))
	{
		return 0;
	}
	if (!WcapTwIpc_Append(Buffer, Capacity, &Length, ",\"path\":") ||
		!WcapTwIpc_AppendJsonString(Buffer, Capacity, &Length, Status->Path) ||
		!WcapTwIpc_AppendFormat(Buffer, Capacity, &Length,
			",\"width\":%u,\"height\":%u,\"fps\":%.3f,\"durationMs\":%u,"
			"\"bitrateKbps\":%u,\"sizeBytes\":%llu,\"droppedFrames\":%u}",
			Status->Width, Status->Height,
			Status->FramerateDen ? (double)Status->FramerateNum / (double)Status->FramerateDen : 0.0,
			Status->DurationMsec, Status->BitrateKbps, Status->SizeBytes, Status->DroppedFrames))
	{
		return 0;
	}
	return Length;
}

static DWORD WcapTwIpc_FormatEvent(BOOL Recording, const WcapTwIpcStatus* Status, LPCWSTR Path, char* Buffer, DWORD Capacity)
{
	DWORD Length = 0;
	if (!WcapTwIpc_AppendFormat(Buffer, Capacity, &Length,
		"{\"event\":\"recording-%s\",\"recording\":%s,\"mode\":\"%s\",\"hwnd\":",
		Recording ? "started" : "stopped", Recording ? "true" : "false",
		Recording ? WcapTwIpc_ModeName(Status->Mode) : "none"))
	{
		return 0;
	}
	if (Recording && Status->Window)
	{
		if (!WcapTwIpc_AppendFormat(Buffer, Capacity, &Length, "\"0x%08llX\"", (UINT64)(UINT_PTR)Status->Window))
		{
			return 0;
		}
	}
	else if (!WcapTwIpc_Append(Buffer, Capacity, &Length, "null"))
	{
		return 0;
	}
	if (!WcapTwIpc_Append(Buffer, Capacity, &Length, ",\"path\":") ||
		!WcapTwIpc_AppendJsonString(Buffer, Capacity, &Length, Recording ? Status->Path : Path) ||
		!WcapTwIpc_Append(Buffer, Capacity, &Length, "}"))
	{
		return 0;
	}
	return Length;
}

static BOOL WcapTwIpc_DispatchRequest(WcapTwIpcRequest* Request)
{
	Request->Completed = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (Request->Completed == NULL)
	{
		return FALSE;
	}
	if (!PostMessageW(gWcapTwIpcWindow, gWcapTwIpcCommandMessage, 0, (LPARAM)Request) ||
		WaitForSingleObject(Request->Completed, INFINITE) != WAIT_OBJECT_0)
	{
		CloseHandle(Request->Completed);
		Request->Completed = NULL;
		return FALSE;
	}
	CloseHandle(Request->Completed);
	Request->Completed = NULL;
	return TRUE;
}

static BOOL WcapTwIpc_SendResponse(HANDLE Pipe, DWORD ExitCode, const char* Json, DWORD Length)
{
	WcapTwIpcWireResponse Response =
	{
		.Magic = WCAP_TW_IPC_MAGIC,
		.ExitCode = ExitCode,
		.Length = Length,
	};
	return WcapTwIpc_WriteAll(Pipe, &Response, sizeof(Response)) &&
		(Length == 0 || WcapTwIpc_WriteAll(Pipe, Json, Length));
}

static DWORD WINAPI WcapTwIpc_ClientWorker(void* Parameter)
{
	HANDLE Pipe = Parameter;
	WcapTwIpcWireRequest Wire;
	if (!WcapTwIpc_ReadAll(Pipe, &Wire, sizeof(Wire)) ||
		Wire.Magic != WCAP_TW_IPC_MAGIC || Wire.Version != WCAP_TW_IPC_VERSION ||
		Wire.Command > WCAP_TW_IPC_STOP || Wire.Mode > WCAP_TW_CAPTURE_REGION)
	{
		WcapTwIpc_SendResponse(Pipe, 3, NULL, 0);
		CloseHandle(Pipe);
		return 0;
	}

	if (Wire.Command == WCAP_TW_IPC_WATCH)
	{
		LONG Generation = InterlockedCompareExchange(&gWcapTwIpcEventGeneration, 0, 0);
		if (!WcapTwIpc_SendResponse(Pipe, 0, NULL, 0))
		{
			CloseHandle(Pipe);
			return 0;
		}

		HANDLE WaitHandles[] = { gWcapTwIpcStopEvent, gWcapTwIpcChangeEvent };
		for (;;)
		{
			DWORD Wait = WaitForMultipleObjects(_countof(WaitHandles), WaitHandles, FALSE, 250);
			if (Wait == WAIT_OBJECT_0)
			{
				break;
			}

			LONG NewGeneration = InterlockedCompareExchange(&gWcapTwIpcEventGeneration, 0, 0);
			if (NewGeneration != Generation)
			{
				if (NewGeneration - Generation > (LONG)WCAP_TW_IPC_EVENT_QUEUE_SIZE)
				{
					Generation = NewGeneration - (LONG)WCAP_TW_IPC_EVENT_QUEUE_SIZE;
				}
				while (Generation < NewGeneration)
				{
					LONG EventGeneration = Generation + 1;
					DWORD EventIndex = (DWORD)EventGeneration % WCAP_TW_IPC_EVENT_QUEUE_SIZE;
					char Json[WCAP_TW_IPC_MAX_JSON];
					DWORD Length;
					EnterCriticalSection(&gWcapTwIpcEventLock);
					Length = gWcapTwIpcEventJsonLength[EventIndex];
					CopyMemory(Json, gWcapTwIpcEventJson[EventIndex], Length);
					LeaveCriticalSection(&gWcapTwIpcEventLock);

					if (!WcapTwIpc_WriteAll(Pipe, &Length, sizeof(Length)) ||
						!WcapTwIpc_WriteAll(Pipe, Json, Length))
					{
						CloseHandle(Pipe);
						return 0;
					}
					Generation = EventGeneration;
				}
			}
			if (Wait == WAIT_OBJECT_0 + 1)
			{
				ResetEvent(gWcapTwIpcChangeEvent);
			}
		}
		CloseHandle(Pipe);
		return 0;
	}

	WcapTwIpcRequest Request =
	{
		.Command = (WcapTwIpcCommand)Wire.Command,
		.Mode = (WcapTwCaptureMode)Wire.Mode,
		.Window = (HWND)(UINT_PTR)Wire.Window,
		.ExitCode = 3,
	};
	if (!WcapTwIpc_DispatchRequest(&Request))
	{
		WcapTwIpc_SendResponse(Pipe, 3, NULL, 0);
		CloseHandle(Pipe);
		return 0;
	}

	char Json[WCAP_TW_IPC_MAX_JSON];
	DWORD Length = 0;
	if (Request.Command == WCAP_TW_IPC_STATUS)
	{
		Length = WcapTwIpc_FormatStatus(&Request.Status, Json, sizeof(Json));
		if (Length == 0)
		{
			Request.ExitCode = 3;
		}
	}
	WcapTwIpc_SendResponse(Pipe, Request.ExitCode, Json, Length);
	CloseHandle(Pipe);
	return 0;
}

static DWORD WINAPI WcapTwIpc_ServerThread(void* Parameter)
{
	(void)Parameter;
	for (;;)
	{
		HANDLE Pipe = CreateNamedPipeW(
			WCAP_TW_PIPE_NAME,
			PIPE_ACCESS_DUPLEX,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
			PIPE_UNLIMITED_INSTANCES,
			4096, 4096, 0, NULL);
		if (Pipe == INVALID_HANDLE_VALUE)
		{
			break;
		}

		BOOL Connected = ConnectNamedPipe(Pipe, NULL) ?
			TRUE : GetLastError() == ERROR_PIPE_CONNECTED;
		if (WaitForSingleObject(gWcapTwIpcStopEvent, 0) == WAIT_OBJECT_0)
		{
			CloseHandle(Pipe);
			break;
		}
		if (!Connected)
		{
			CloseHandle(Pipe);
			continue;
		}

		HANDLE Thread = CreateThread(NULL, 0, WcapTwIpc_ClientWorker, Pipe, 0, NULL);
		if (Thread)
		{
			CloseHandle(Thread);
		}
		else
		{
			CloseHandle(Pipe);
		}
	}
	return 0;
}

static BOOL WcapTwIpc_ParseUnsigned(LPCWSTR Text, UINT64* Value)
{
	if (Text == NULL || *Text == 0)
	{
		return FALSE;
	}
	WCHAR* End;
	UINT64 Parsed = _wcstoui64(Text, &End, 0);
	if (*End != 0)
	{
		return FALSE;
	}
	*Value = Parsed;
	return TRUE;
}

static BOOL WcapTwIpc_ParseCommandLine(WcapTwIpcWireRequest* Request, BOOL* IsClient)
{
	int Count;
	LPWSTR* Args = CommandLineToArgvW(GetCommandLineW(), &Count);
	if (Args == NULL)
	{
		return FALSE;
	}

	*IsClient = Count >= 2 &&
		(lstrcmpiW(Args[1], L"-status") == 0 || lstrcmpiW(Args[1], L"-watch") == 0 ||
		 lstrcmpiW(Args[1], L"-start") == 0 || lstrcmpiW(Args[1], L"-stop") == 0);
	if (!*IsClient)
	{
		LocalFree(Args);
		return TRUE;
	}

	Request->Magic = WCAP_TW_IPC_MAGIC;
	Request->Version = WCAP_TW_IPC_VERSION;
	BOOL Valid = TRUE;
	if (lstrcmpiW(Args[1], L"-status") == 0)
	{
		Request->Command = WCAP_TW_IPC_STATUS;
		Valid = Count == 2;
	}
	else if (lstrcmpiW(Args[1], L"-watch") == 0)
	{
		Request->Command = WCAP_TW_IPC_WATCH;
		Valid = Count == 2;
	}
	else if (lstrcmpiW(Args[1], L"-stop") == 0)
	{
		Request->Command = WCAP_TW_IPC_STOP;
		Valid = Count == 2;
	}
	else
	{
		Request->Command = WCAP_TW_IPC_START;
		Valid = Count == 3 || Count == 5;
		if (Valid && lstrcmpiW(Args[2], L"window") == 0)
		{
			Request->Mode = WCAP_TW_CAPTURE_WINDOW;
			Request->Window = (UINT64)(UINT_PTR)GetForegroundWindow();
			if (Count == 5)
			{
				Valid = lstrcmpiW(Args[3], L"--hwnd") == 0 &&
					WcapTwIpc_ParseUnsigned(Args[4], &Request->Window);
			}
		}
		else if (Valid && Count == 3 && lstrcmpiW(Args[2], L"monitor") == 0)
		{
			Request->Mode = WCAP_TW_CAPTURE_MONITOR;
		}
		else if (Valid && Count == 3 && lstrcmpiW(Args[2], L"region") == 0)
		{
			Request->Mode = WCAP_TW_CAPTURE_REGION;
		}
		else
		{
			Valid = FALSE;
		}
	}

	LocalFree(Args);
	return Valid;
}

static HANDLE WcapTwIpc_Connect(void)
{
	for (int Attempt = 0; Attempt < 20; Attempt++)
	{
		HANDLE Pipe = CreateFileW(WCAP_TW_PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
			0, NULL, OPEN_EXISTING, 0, NULL);
		if (Pipe != INVALID_HANDLE_VALUE)
		{
			return Pipe;
		}
		if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(WCAP_TW_PIPE_NAME, 100))
		{
			break;
		}
	}
	return INVALID_HANDLE_VALUE;
}

static BOOL WcapTwIpc_WriteStdout(const void* Buffer, DWORD Length)
{
	HANDLE Output = GetStdHandle(STD_OUTPUT_HANDLE);
	return Output != NULL && Output != INVALID_HANDLE_VALUE &&
		WcapTwIpc_WriteAll(Output, Buffer, Length);
}

BOOL WcapTwIpc_RunClientCommandLine(void)
{
	WcapTwIpcWireRequest Request = { 0 };
	BOOL IsClient = FALSE;
	if (!WcapTwIpc_ParseCommandLine(&Request, &IsClient))
	{
		if (IsClient)
		{
			ExitProcess(3);
		}
		return FALSE;
	}
	if (!IsClient)
	{
		return FALSE;
	}

	HANDLE Pipe = WcapTwIpc_Connect();
	if (Pipe == INVALID_HANDLE_VALUE)
	{
		ExitProcess(2);
	}
	if (!WcapTwIpc_WriteAll(Pipe, &Request, sizeof(Request)))
	{
		CloseHandle(Pipe);
		ExitProcess(3);
	}

	WcapTwIpcWireResponse Response;
	if (!WcapTwIpc_ReadAll(Pipe, &Response, sizeof(Response)) ||
		Response.Magic != WCAP_TW_IPC_MAGIC || Response.Length > WCAP_TW_IPC_MAX_JSON)
	{
		CloseHandle(Pipe);
		ExitProcess(3);
	}

	if (Response.Length)
	{
		char Json[WCAP_TW_IPC_MAX_JSON];
		if (!WcapTwIpc_ReadAll(Pipe, Json, Response.Length) ||
			!WcapTwIpc_WriteStdout(Json, Response.Length) ||
			!WcapTwIpc_WriteStdout("\r\n", 2))
		{
			CloseHandle(Pipe);
			ExitProcess(3);
		}
	}

	if (Request.Command == WCAP_TW_IPC_WATCH && Response.ExitCode == 0)
	{
		for (;;)
		{
			DWORD Length;
			char Json[WCAP_TW_IPC_MAX_JSON];
			if (!WcapTwIpc_ReadAll(Pipe, &Length, sizeof(Length)) ||
				Length == 0 || Length > sizeof(Json) ||
				!WcapTwIpc_ReadAll(Pipe, Json, Length) ||
				!WcapTwIpc_WriteStdout(Json, Length) ||
				!WcapTwIpc_WriteStdout("\r\n", 2))
			{
				CloseHandle(Pipe);
				ExitProcess(2);
			}
		}
	}

	CloseHandle(Pipe);
	ExitProcess(Response.ExitCode);
}

BOOL WcapTwIpc_Init(HWND Window, UINT CommandMessage)
{
	gWcapTwIpcWindow = Window;
	gWcapTwIpcCommandMessage = CommandMessage;
	gWcapTwIpcStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	gWcapTwIpcChangeEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	gWcapTwIpcRecordingEvent = CreateEventW(NULL, TRUE, FALSE, WCAP_TW_RECORDING_EVENT_NAME);
	if (!gWcapTwIpcStopEvent || !gWcapTwIpcChangeEvent || !gWcapTwIpcRecordingEvent)
	{
		return FALSE;
	}
	ResetEvent(gWcapTwIpcRecordingEvent);
	InitializeCriticalSection(&gWcapTwIpcEventLock);
	return TRUE;
}

BOOL WcapTwIpc_StartServer(void)
{
	gWcapTwIpcServerThread = CreateThread(NULL, 0, WcapTwIpc_ServerThread, NULL, 0, NULL);
	return gWcapTwIpcServerThread != NULL;
}

void WcapTwIpc_StopServer(void)
{
	if (gWcapTwIpcStopEvent)
	{
		SetEvent(gWcapTwIpcStopEvent);
		SetEvent(gWcapTwIpcChangeEvent);
		HANDLE Wake = CreateFileW(WCAP_TW_PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
			0, NULL, OPEN_EXISTING, 0, NULL);
		if (Wake != INVALID_HANDLE_VALUE)
		{
			CloseHandle(Wake);
		}
	}
	if (gWcapTwIpcServerThread)
	{
		WaitForSingleObject(gWcapTwIpcServerThread, 2000);
		CloseHandle(gWcapTwIpcServerThread);
		gWcapTwIpcServerThread = NULL;
	}
}

void WcapTwIpc_CompleteRequest(WcapTwIpcRequest* Request, DWORD ExitCode, const WcapTwIpcStatus* Status)
{
	Request->ExitCode = ExitCode;
	if (Status)
	{
		Request->Status = *Status;
	}
	SetEvent(Request->Completed);
}

void WcapTwIpc_NotifyRecordingStarted(const WcapTwIpcStatus* Status)
{
	SetEvent(gWcapTwIpcRecordingEvent);
	EnterCriticalSection(&gWcapTwIpcEventLock);
	LONG Generation = InterlockedIncrement(&gWcapTwIpcEventGeneration);
	DWORD Index = (DWORD)Generation % WCAP_TW_IPC_EVENT_QUEUE_SIZE;
	gWcapTwIpcEventJsonLength[Index] = WcapTwIpc_FormatEvent(TRUE, Status, NULL,
		gWcapTwIpcEventJson[Index], sizeof(gWcapTwIpcEventJson[Index]));
	SetEvent(gWcapTwIpcChangeEvent);
	LeaveCriticalSection(&gWcapTwIpcEventLock);
}

void WcapTwIpc_NotifyRecordingStopped(LPCWSTR Path)
{
	WcapTwIpcStatus Status = { 0 };
	ResetEvent(gWcapTwIpcRecordingEvent);
	EnterCriticalSection(&gWcapTwIpcEventLock);
	LONG Generation = InterlockedIncrement(&gWcapTwIpcEventGeneration);
	DWORD Index = (DWORD)Generation % WCAP_TW_IPC_EVENT_QUEUE_SIZE;
	gWcapTwIpcEventJsonLength[Index] = WcapTwIpc_FormatEvent(FALSE, &Status, Path,
		gWcapTwIpcEventJson[Index], sizeof(gWcapTwIpcEventJson[Index]));
	SetEvent(gWcapTwIpcChangeEvent);
	LeaveCriticalSection(&gWcapTwIpcEventLock);
}
