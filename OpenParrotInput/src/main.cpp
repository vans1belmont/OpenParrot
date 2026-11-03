// Serves the TeknoParrotPipe input block that FastIoEmu reads, for launches
// without TeknoParrotUI. Must be running before the game starts, because the
// game only tries to open the pipe once.
//
// OpenParrotInput.exe                       map keys and a gamepad using OpenParrotInput.ini
// OpenParrotInput.exe --probe [delay] [hold] [gap]    walk input bits 0-31 (ms)
// OpenParrotInput.exe --pulse <byte> <mask> [delay]   repeatedly press one input
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <Xinput.h>
#include <atomic>
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
static const DWORD kPadScanIntervalMs = 1000;

// A gamepad's inputs as one bitfield: buttons 1-28, then the four directions.
static const int kPadButtonCount = 28;
static const uint32_t kPadUp = 1u << 28;
static const uint32_t kPadDown = 1u << 29;
static const uint32_t kPadLeft = 1u << 30;
static const uint32_t kPadRight = 1u << 31;
static const SHORT kStickThreshold = 16000;

struct Binding
{
	int vk;
	uint32_t pad;
	int index;
	uint8_t mask;
};

static uint8_t g_state[kStateSize];
static CRITICAL_SECTION g_stateLock;
static CRITICAL_SECTION g_logLock;
static FILE* g_log;

static void Log(const char* format, ...)
{
	if (g_log == nullptr)
		return;

	EnterCriticalSection(&g_logLock);

	SYSTEMTIME now;
	GetLocalTime(&now);
	fprintf(g_log, "%02d:%02d:%02d.%03d ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);

	va_list args;
	va_start(args, format);
	vfprintf(g_log, format, args);
	va_end(args);

	fputc('\n', g_log);
	fflush(g_log);

	LeaveCriticalSection(&g_logLock);
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

typedef DWORD (WINAPI* XInputGetStateFn)(DWORD, XINPUT_STATE*);

static std::atomic<uint32_t> g_padInputs;

static XInputGetStateFn LoadXInput()
{
	for (const char* name : { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" })
	{
		HMODULE module = LoadLibraryA(name);
		if (module != nullptr)
			return reinterpret_cast<XInputGetStateFn>(GetProcAddress(module, "XInputGetState"));
	}

	return nullptr;
}

static uint32_t TranslateXInput(const XINPUT_GAMEPAD& pad)
{
	// Numbered the way the joystick API reports these buttons, so one layout fits both.
	static const WORD kButtons[] = {
		XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
		XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER,
		XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_START,
		XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB,
	};

	uint32_t inputs = 0;
	for (int i = 0; i < ARRAYSIZE(kButtons); i++)
	{
		if (pad.wButtons & kButtons[i])
			inputs |= 1u << i;
	}

	if ((pad.wButtons & XINPUT_GAMEPAD_DPAD_UP) || pad.sThumbLY > kStickThreshold)
		inputs |= kPadUp;
	if ((pad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) || pad.sThumbLY < -kStickThreshold)
		inputs |= kPadDown;
	if ((pad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) || pad.sThumbLX < -kStickThreshold)
		inputs |= kPadLeft;
	if ((pad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) || pad.sThumbLX > kStickThreshold)
		inputs |= kPadRight;

	return inputs;
}

static bool ReadJoystick(UINT id, const JOYCAPSA& caps, uint32_t* inputs)
{
	JOYINFOEX info = {};
	info.dwSize = sizeof(info);
	info.dwFlags = JOY_RETURNBUTTONS | JOY_RETURNX | JOY_RETURNY | JOY_RETURNPOVCTS;
	if (joyGetPosEx(id, &info) != JOYERR_NOERROR)
		return false;

	*inputs = info.dwButtons & ((1u << kPadButtonCount) - 1);

	const DWORD xZone = (caps.wXmax - caps.wXmin) / 4;
	const DWORD yZone = (caps.wYmax - caps.wYmin) / 4;
	if (info.dwYpos < caps.wYmin + yZone)
		*inputs |= kPadUp;
	if (info.dwYpos > caps.wYmax - yZone)
		*inputs |= kPadDown;
	if (info.dwXpos < caps.wXmin + xZone)
		*inputs |= kPadLeft;
	if (info.dwXpos > caps.wXmax - xZone)
		*inputs |= kPadRight;

	// The hat reports hundredths of a degree clockwise from up.
	const DWORD hat = info.dwPOV;
	if ((caps.wCaps & JOYCAPS_HASPOV) && hat != JOY_POVCENTERED)
	{
		if (hat >= 31500 || hat <= 4500)
			*inputs |= kPadUp;
		if (hat >= 4500 && hat <= 13500)
			*inputs |= kPadRight;
		if (hat >= 13500 && hat <= 22500)
			*inputs |= kPadDown;
		if (hat >= 22500 && hat <= 31500)
			*inputs |= kPadLeft;
	}

	return true;
}

static void LogPadInputs(uint32_t inputs)
{
	std::string text;
	for (int i = 0; i < kPadButtonCount; i++)
	{
		if (inputs & (1u << i))
			text += " " + std::to_string(i + 1);
	}

	if (inputs & kPadUp)
		text += " up";
	if (inputs & kPadDown)
		text += " down";
	if (inputs & kPadLeft)
		text += " left";
	if (inputs & kPadRight)
		text += " right";

	Log("pad:%s", text.c_str());
}

// Polled apart from the keyboard so that scanning for a pad can't delay key input.
static DWORD WINAPI PadThread(LPVOID)
{
	XInputGetStateFn xinputGetState = LoadXInput();
	Log("XInput %s", xinputGetState != nullptr ? "loaded" : "not available");

	enum { kNoPad, kXInputPad, kJoystickPad } type = kNoPad;
	DWORD id = 0;
	JOYCAPSA caps = {};
	uint32_t previous = 0;

	while (true)
	{
		uint32_t inputs = 0;

		if (type == kNoPad)
		{
			// A pad usually shows up through both APIs, so only one is read.
			for (DWORD slot = 0; type == kNoPad && xinputGetState != nullptr && slot < XUSER_MAX_COUNT; slot++)
			{
				XINPUT_STATE state = {};
				if (xinputGetState(slot, &state) == ERROR_SUCCESS)
				{
					type = kXInputPad;
					id = slot;
					Log("using XInput pad %u", slot);
				}
			}

			for (UINT slot = 0; type == kNoPad && slot < joyGetNumDevs(); slot++)
			{
				if (joyGetDevCapsA(slot, &caps, sizeof(caps)) == JOYERR_NOERROR && ReadJoystick(slot, caps, &inputs))
				{
					type = kJoystickPad;
					id = slot;
					Log("using joystick %u: %s, %u buttons", slot, caps.szPname, caps.wNumButtons);
				}
			}

			if (type == kNoPad)
			{
				Sleep(kPadScanIntervalMs);
				continue;
			}
		}

		bool connected;
		if (type == kXInputPad)
		{
			XINPUT_STATE state = {};
			connected = xinputGetState(id, &state) == ERROR_SUCCESS;
			inputs = connected ? TranslateXInput(state.Gamepad) : 0;
		}
		else
		{
			connected = ReadJoystick(id, caps, &inputs);
		}

		if (!connected)
		{
			Log("pad disconnected");
			type = kNoPad;
			inputs = 0;
		}

		g_padInputs = inputs;
		if (inputs != previous)
		{
			LogPadInputs(inputs);
			previous = inputs;
		}

		Sleep(kWriteIntervalMs);
	}
}

// Returns the bit for a pad input name from the layout file, or 0 if there is none.
static uint32_t PadBit(const char* name)
{
	if (strcmp(name, "up") == 0)
		return kPadUp;
	if (strcmp(name, "down") == 0)
		return kPadDown;
	if (strcmp(name, "left") == 0)
		return kPadLeft;
	if (strcmp(name, "right") == 0)
		return kPadRight;

	const int button = atoi(name);
	return button >= 1 && button <= kPadButtonCount ? 1u << (button - 1) : 0;
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

	// Each line is "<virtual key> <byte index> <bit mask>" or
	// "pad <button number or direction> <byte index> <bit mask>"; ';' starts a comment.
	char line[256];
	while (fgets(line, sizeof(line), file) != nullptr)
	{
		int vk = 0;
		uint32_t pad = 0;
		int index = 0;
		int mask = 0;
		char padName[16] = {};
		if (sscanf(line, "pad %15s %i %i", padName, &index, &mask) == 3)
		{
			pad = PadBit(padName);
			if (pad == 0)
			{
				Log("ignoring unknown pad input: %s", line);
				continue;
			}
		}
		else if (line[0] == ';' || sscanf(line, "%i %i %i", &vk, &index, &mask) != 3)
		{
			continue;
		}

		if (index < 0 || index >= kStateSize || mask <= 0 || mask > 0xFF)
		{
			Log("ignoring invalid binding: %s", line);
			continue;
		}

		bindings.push_back({ vk, pad, index, static_cast<uint8_t>(mask) });
	}

	fclose(file);
	return bindings;
}

static void RunBindings()
{
	std::vector<Binding> bindings = LoadBindings(SiblingPath("OpenParrotInput.ini"));
	Log("loaded %d bindings", static_cast<int>(bindings.size()));

	CloseHandle(CreateThread(nullptr, 0, PadThread, nullptr, 0, nullptr));

	uint8_t previous[kStateSize] = {};
	while (true)
	{
		const uint32_t pad = g_padInputs;

		uint8_t state[kStateSize] = {};
		for (const Binding& binding : bindings)
		{
			const bool pressed = binding.pad != 0 ? (pad & binding.pad) != 0 : (GetAsyncKeyState(binding.vk) & 0x8000) != 0;
			if (pressed)
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
	InitializeCriticalSection(&g_logLock);
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
		RunBindings();
	}

	return 0;
}
