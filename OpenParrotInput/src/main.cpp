// Serves the TeknoParrotPipe input block that FastIoEmu reads, for launches
// without TeknoParrotUI. Must be running before the game starts, because the
// game only tries to open the pipe once.
//
// OpenParrotInput.exe                       map keys using OpenParrotInput.ini
// OpenParrotInput.exe --probe [delay] [hold] [gap]    walk input bits 0-31 (ms)
// OpenParrotInput.exe --pulse <byte> <mask> [delay]   repeatedly press one input
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static const char* kPipeName = "\\\\.\\pipe\\TeknoParrotPipe";
static const int kStateSize = 64;
static const DWORD kWriteIntervalMs = 4;

struct Binding
{
	int vk;
	int index;
	uint8_t mask;
};

static uint8_t g_state[kStateSize];
static CRITICAL_SECTION g_stateLock;
static FILE* g_log;

static void Log(const char* format, ...)
{
	if (g_log == nullptr)
		return;

	SYSTEMTIME now;
	GetLocalTime(&now);
	fprintf(g_log, "%02d:%02d:%02d.%03d ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);

	va_list args;
	va_start(args, format);
	vfprintf(g_log, format, args);
	va_end(args);

	fputc('\n', g_log);
	fflush(g_log);
}

static std::string SiblingPath(const char* fileName)
{
	char path[MAX_PATH] = {};
	GetModuleFileNameA(nullptr, path, MAX_PATH);

	char* separator = strrchr(path, '\\');
	if (separator != nullptr)
		separator[1] = '\0';

	return std::string(path) + fileName;
}

static void SetState(const uint8_t* state)
{
	EnterCriticalSection(&g_stateLock);
	memcpy(g_state, state, kStateSize);
	LeaveCriticalSection(&g_stateLock);
}

static DWORD WINAPI ClientThread(LPVOID parameter)
{
	HANDLE pipe = parameter;
	uint8_t state[kStateSize];

	while (true)
	{
		EnterCriticalSection(&g_stateLock);
		memcpy(state, g_state, kStateSize);
		LeaveCriticalSection(&g_stateLock);

		DWORD written = 0;
		if (!WriteFile(pipe, state, kStateSize, &written, nullptr))
			break;

		Sleep(kWriteIntervalMs);
	}

	Log("client disconnected (0x%X)", GetLastError());
	DisconnectNamedPipe(pipe);
	CloseHandle(pipe);
	return 0;
}

static HANDLE g_clientConnected;

static DWORD WINAPI ServerThread(LPVOID)
{
	while (true)
	{
		HANDLE pipe = CreateNamedPipeA(kPipeName, PIPE_ACCESS_OUTBOUND, PIPE_TYPE_BYTE | PIPE_WAIT,
			PIPE_UNLIMITED_INSTANCES, kStateSize, 0, 0, nullptr);
		if (pipe == INVALID_HANDLE_VALUE)
		{
			Log("CreateNamedPipe failed (0x%X)", GetLastError());
			return 1;
		}

		if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED)
		{
			Log("client connected");
			SetEvent(g_clientConnected);
			CloseHandle(CreateThread(nullptr, 0, ClientThread, pipe, 0, nullptr));
		}
		else
		{
			CloseHandle(pipe);
		}
	}
}

static std::vector<Binding> LoadBindings(const std::string& path)
{
	std::vector<Binding> bindings;

	FILE* file = fopen(path.c_str(), "r");
	if (file == nullptr)
	{
		Log("could not open %s", path.c_str());
		return bindings;
	}

	// Each line is "<virtual key> <byte index> <bit mask>"; ';' starts a comment.
	char line[256];
	while (fgets(line, sizeof(line), file) != nullptr)
	{
		int vk = 0;
		int index = 0;
		int mask = 0;
		if (line[0] == ';' || sscanf(line, "%i %i %i", &vk, &index, &mask) != 3)
			continue;

		if (index < 0 || index >= kStateSize || mask <= 0 || mask > 0xFF)
		{
			Log("ignoring invalid binding: %s", line);
			continue;
		}

		bindings.push_back({ vk, index, static_cast<uint8_t>(mask) });
	}

	fclose(file);
	return bindings;
}

static void RunKeyboard()
{
	std::vector<Binding> bindings = LoadBindings(SiblingPath("OpenParrotInput.ini"));
	Log("loaded %d key bindings", static_cast<int>(bindings.size()));

	uint8_t previous[kStateSize] = {};
	while (true)
	{
		uint8_t state[kStateSize] = {};
		for (const Binding& binding : bindings)
		{
			if (GetAsyncKeyState(binding.vk) & 0x8000)
				state[binding.index] |= binding.mask;
		}

		if (memcmp(state, previous, kStateSize) != 0)
		{
			Log("state %02X %02X %02X %02X coin %02X", state[0], state[1], state[2], state[3], state[4]);
			memcpy(previous, state, kStateSize);
		}

		SetState(state);
		Sleep(kWriteIntervalMs);
	}
}

static void RunProbe(DWORD delay, DWORD hold, DWORD gap)
{
	WaitForSingleObject(g_clientConnected, INFINITE);
	Sleep(delay);

	for (int index = 0; index < 4; index++)
	{
		for (int bit = 0; bit < 8; bit++)
		{
			uint8_t state[kStateSize] = {};
			state[index] = static_cast<uint8_t>(1 << bit);

			Log("press byte %d bit %d", index, bit);
			SetState(state);
			Sleep(hold);

			memset(state, 0, kStateSize);
			Log("release");
			SetState(state);
			Sleep(gap);
		}
	}

	Log("probe finished");
	Sleep(INFINITE);
}

static void RunPulse(int index, uint8_t mask, DWORD delay)
{
	WaitForSingleObject(g_clientConnected, INFINITE);
	Sleep(delay);

	while (true)
	{
		uint8_t state[kStateSize] = {};
		state[index] = mask;

		Log("press byte %d mask %02X", index, mask);
		SetState(state);
		Sleep(300);

		memset(state, 0, kStateSize);
		SetState(state);
		Sleep(1700);
	}
}

static DWORD ArgOr(int argc, char* argv[], int position, DWORD fallback)
{
	return position < argc ? strtoul(argv[position], nullptr, 0) : fallback;
}

int main(int argc, char* argv[])
{
	g_log = fopen(SiblingPath("OpenParrotInput.log").c_str(), "w");

	InitializeCriticalSection(&g_stateLock);
	g_clientConnected = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	CloseHandle(CreateThread(nullptr, 0, ServerThread, nullptr, 0, nullptr));

	if (argc > 1 && strcmp(argv[1], "--probe") == 0)
	{
		RunProbe(ArgOr(argc, argv, 2, 15000), ArgOr(argc, argv, 3, 1500), ArgOr(argc, argv, 4, 2500));
	}
	else if (argc > 3 && strcmp(argv[1], "--pulse") == 0)
	{
		int index = strtol(argv[2], nullptr, 0);
		if (index < 0 || index >= kStateSize)
			return 1;

		RunPulse(index, static_cast<uint8_t>(strtoul(argv[3], nullptr, 0)), ArgOr(argc, argv, 4, 15000));
	}
	else
	{
		RunKeyboard();
	}

	return 0;
}
