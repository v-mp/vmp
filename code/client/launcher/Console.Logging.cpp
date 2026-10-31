#include <StdInc.h>
#include <concurrentqueue.h>

#include <filesystem>
#include <fmt/chrono.h>
#include <io.h>
#include <new.h>
#include <signal.h>
#include <Psapi.h>

#include <HostSharedData.h>
#include "TickCountData.h"

#ifndef STATUS_HEAP_CORRUPTION
#define STATUS_HEAP_CORRUPTION ((DWORD)0xC0000374L)
#endif
#ifndef STATUS_STACK_BUFFER_OVERRUN
#define STATUS_STACK_BUFFER_OVERRUN ((DWORD)0xC0000409L)
#endif
#ifndef STATUS_FATAL_APP_EXIT
#define STATUS_FATAL_APP_EXIT ((DWORD)0x40000015L)
#endif
#ifndef STATUS_NO_MEMORY
#define STATUS_NO_MEMORY ((DWORD)0xC0000017L)
#endif

static constexpr uint64_t kLogRetentionTicks = 2 * 24 * 60 * 60 * uint64_t(10000000);

static std::string GetProcessName()
{
	wchar_t fn[256];
	GetModuleFileName(GetModuleHandle(NULL), fn, std::size(fn));

	wchar_t* fnPtr = fn;

	if (wcschr(fn, L'_'))
	{
		fnPtr = wcschr(fn, L'_') + 1;
	}

	if (wcsrchr(fnPtr, L'\\'))
	{
		fnPtr = wcsrchr(fnPtr, L'\\') + 1;
	}

	auto dotPtr = wcsrchr(fnPtr, L'.');

	if (dotPtr)
	{
		dotPtr[0] = '\0';
	}

	std::string rv = ToNarrow(fnPtr);

	if (rv.size() > 14)
	{
		rv = rv.substr(0, 14);
	}

	return rv;
}

static auto _GetThreadDescription = (decltype(&GetThreadDescription))GetProcAddress(GetModuleHandle(L"kernel32.dll"), "GetThreadDescription");

static std::string GetThreadName()
{
	std::string rv = std::to_string(GetCurrentThreadId());

	if (_GetThreadDescription)
	{
		PWSTR str;
		HRESULT hr = _GetThreadDescription(GetCurrentThread(), &str);

		if (SUCCEEDED(hr))
		{
			if (isalnum(str[0]) || str[0] == '[')
			{
				rv = ToNarrow(str);
			}

			LocalFree(str);
		}
	}

	if (rv.size() > 20)
	{
		rv = rv.substr(0, 20);
	}

	return rv;
}

static TickCountData* GetInitTickCount()
{
	static TickCountData initTickCountBacking = ([]()
	{
		HostSharedData<TickCountData> initTickCountRef("CFX_SharedTickCount");
		return *initTickCountRef;
	})();

	return &initTickCountBacking;
}

static fwPlatformString* GetLogFilePath()
{
	auto initTickCount = GetInitTickCount();

	static fwPlatformString* fp = new fwPlatformString(MakeRelativeCitPath(fmt::sprintf(L"logs/CitizenFX_log_%04d-%02d-%02dT%02d%02d%02d.log",
		initTickCount->initTime.wYear, initTickCount->initTime.wMonth, initTickCount->initTime.wDay,
		initTickCount->initTime.wHour, initTickCount->initTime.wMinute, initTickCount->initTime.wSecond)));

	return fp;
}

static FILE* g_logFile = nullptr;
static HANDLE g_logFileHandle = INVALID_HANDLE_VALUE;
static SRWLOCK g_fileLogMutex = SRWLOCK_INIT;
static std::vector<char>* g_lineBuffer = new std::vector<char>(8192);
static size_t g_lineIndex = 0;
static std::string* g_processName = nullptr;

static void EnsureLogFileOpen_Locked()
{
	if (g_logFile)
	{
		return;
	}

	CreateDirectoryW(MakeRelativeCitPath(L"logs/").c_str(), nullptr);

	auto* fp = GetLogFilePath();
	g_logFile = _wfopen(fp->c_str(), L"ab");

	if (g_logFile)
	{
		setvbuf(g_logFile, nullptr, _IOFBF, 64 * 1024);
		g_logFileHandle = (HANDLE)_get_osfhandle(_fileno(g_logFile));
	}
}

static void WriteLogLine_Locked(const char* threadName, const char* line)
{
	EnsureLogFileOpen_Locked();

	if (!g_logFile)
	{
		return;
	}

	if (!g_processName)
	{
		g_processName = new std::string(GetProcessName());
	}

	auto initTickCount = GetInitTickCount();
	fmt::fprintf(g_logFile, "[%10lld] [%14s] %20s/ %s\r\n",
		GetTickCount64() - initTickCount->tickCount,
		*g_processName,
		threadName,
		line);
}

static void FlushLogFile_Locked()
{
	if (g_logFile)
	{
		fflush(g_logFile);

		if (g_logFileHandle != INVALID_HANDLE_VALUE)
		{
			FlushFileBuffers(g_logFileHandle);
		}
	}
}

static void PerformFileLog(const std::tuple<std::string, std::string>& pair)
{
	AcquireSRWLockExclusive(&g_fileLogMutex);

	for (const char* p = std::get<1>(pair).c_str(); *p; ++p)
	{
		if (*p == '\n')
		{
			(*g_lineBuffer)[g_lineIndex] = '\0';
			WriteLogLine_Locked(std::get<0>(pair).c_str(), g_lineBuffer->data());
			g_lineIndex = 0;
			continue;
		}

		(*g_lineBuffer)[g_lineIndex] = *p;
		++g_lineIndex;

		if (g_lineIndex >= (g_lineBuffer->size() - 1))
		{
			g_lineBuffer->resize(g_lineBuffer->size() * 2);
		}
	}

	ReleaseSRWLockExclusive(&g_fileLogMutex);
}

static CONDITION_VARIABLE g_logCondVar = CONDITION_VARIABLE_INIT;
static SRWLOCK g_logMutex = SRWLOCK_INIT;

static moodycamel::ConcurrentQueue<std::tuple<std::string, std::string>>* g_logPrintQueue = new moodycamel::ConcurrentQueue<std::tuple<std::string, std::string>>();

static void DrainLogQueue()
{
	std::tuple<std::string, std::string> str;

	while (g_logPrintQueue->try_dequeue(str))
	{
		PerformFileLog(str);
	}
}

struct LoggerInit
{
	LoggerInit()
	{
		CreateThread(
		NULL, 0, [](LPVOID) -> DWORD
		{
			SetThreadName(-1, "[Cfx] File Log Thread");

			while (true)
			{
				AcquireSRWLockExclusive(&g_logMutex);
				SleepConditionVariableSRW(&g_logCondVar, &g_logMutex, INFINITE, 0);
				ReleaseSRWLockExclusive(&g_logMutex);

				DrainLogQueue();
			}

			return 0;
		},
		NULL, 0, NULL);
	}
};

static LoggerInit logger;

extern "C" DLL_EXPORT void AsyncTrace(const char* string)
{
	std::string threadName = GetThreadName();

	g_logPrintQueue->enqueue({ threadName, string });
	WakeAllConditionVariable(&g_logCondVar);
}

extern "C" DLL_EXPORT void SyncCriticalTrace(const char* string)
{
	if (!string)
	{
		return;
	}

	std::string threadName = GetThreadName();

	AcquireSRWLockExclusive(&g_fileLogMutex);

	const char* start = string;
	for (const char* p = string; ; ++p)
	{
		if (*p == '\n' || *p == '\0')
		{
			if (p > start)
			{
				std::string line(start, p - start);
				WriteLogLine_Locked(threadName.c_str(), line.c_str());
			}

			if (*p == '\0')
			{
				break;
			}

			start = p + 1;
		}
	}

	FlushLogFile_Locked();
	ReleaseSRWLockExclusive(&g_fileLogMutex);
}

extern "C" DLL_EXPORT void FlushCrashLog()
{
	DrainLogQueue();

	AcquireSRWLockExclusive(&g_fileLogMutex);

	if (g_lineIndex > 0)
	{
		(*g_lineBuffer)[g_lineIndex] = '\0';

		if (!g_processName)
		{
			g_processName = new std::string(GetProcessName());
		}

		WriteLogLine_Locked(GetThreadName().c_str(), g_lineBuffer->data());
		g_lineIndex = 0;
	}

	FlushLogFile_Locked();
	ReleaseSRWLockExclusive(&g_fileLogMutex);
}

static void PruneOldLogs()
{
	SYSTEMTIME curTime;
	GetSystemTime(&curTime);

	std::error_code ec;
	auto logsPath = MakeRelativeCitPath(L"logs/");

	if (!std::filesystem::exists(logsPath, ec))
	{
		return;
	}

	for (auto& f : std::filesystem::directory_iterator{ logsPath, ec })
	{
		if (ec)
		{
			break;
		}

		int year, month, day, hour, minute, second;
		int fields = swscanf(f.path().filename().c_str(), L"CitizenFX_log_%04d-%02d-%02dT%02d%02d%02d.log", &year, &month, &day, &hour, &minute, &second);

		if (fields == 6)
		{
			SYSTEMTIME fakeTime = { 0 };
			fakeTime.wSecond = second;
			fakeTime.wMinute = minute;
			fakeTime.wHour = hour;
			fakeTime.wDay = day;
			fakeTime.wMonth = month;
			fakeTime.wYear = year;

			FILETIME curStamp, fakeStamp;
			SystemTimeToFileTime(&curTime, &curStamp);
			SystemTimeToFileTime(&fakeTime, &fakeStamp);

			uint64_t curT = ((ULARGE_INTEGER*)&curStamp)->QuadPart;
			uint64_t fileT = ((ULARGE_INTEGER*)&fakeStamp)->QuadPart;

			if (curT > fileT && (curT - fileT) > kLogRetentionTicks)
			{
				std::filesystem::remove(f.path(), ec);
			}
		}
	}

	WIN32_FIND_DATAW findData;
	HANDLE hFind = FindFirstFileW((MakeRelativeCitPath(L"crashes") + L"\\*").c_str(), &findData);

	if (hFind == INVALID_HANDLE_VALUE)
	{
		return;
	}

	FILETIME curStamp;
	GetSystemTimeAsFileTime(&curStamp);
	ULARGE_INTEGER curT;
	curT.LowPart = curStamp.dwLowDateTime;
	curT.HighPart = curStamp.dwHighDateTime;

	do
	{
		if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			continue;
		}

		std::wstring name = findData.cFileName;
		auto dot = name.find_last_of(L'.');
		if (dot == std::wstring::npos)
		{
			continue;
		}

		std::wstring ext = name.substr(dot);
		if (ext != L".dmp" && ext != L".zip" && ext != L".gamelog" && ext != L".json" && name.find(L".dmp.") == std::wstring::npos)
		{
			continue;
		}

		ULARGE_INTEGER fileT;
		fileT.LowPart = findData.ftLastWriteTime.dwLowDateTime;
		fileT.HighPart = findData.ftLastWriteTime.dwHighDateTime;

		if (curT.QuadPart > fileT.QuadPart && (curT.QuadPart - fileT.QuadPart) > kLogRetentionTicks)
		{
			DeleteFileW((MakeRelativeCitPath(L"crashes") + L"\\" + name).c_str());
		}
	} while (FindNextFileW(hFind, &findData));

	FindClose(hFind);
}

static void EmergencyWrite(const char* msg)
{
	if (!msg)
	{
		return;
	}

	AcquireSRWLockExclusive(&g_fileLogMutex);
	EnsureLogFileOpen_Locked();

	if (g_logFileHandle != INVALID_HANDLE_VALUE)
	{
		DWORD written = 0;
		WriteFile(g_logFileHandle, msg, static_cast<DWORD>(strlen(msg)), &written, nullptr);
		FlushFileBuffers(g_logFileHandle);
	}

	ReleaseSRWLockExclusive(&g_fileLogMutex);
}

static void LogMemorySnapshot(const char* tag)
{
	MEMORYSTATUSEX status = { sizeof(status) };
	GlobalMemoryStatusEx(&status);

	PROCESS_MEMORY_COUNTERS_EX pmc = {};
	pmc.cb = sizeof(pmc);
	GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));

	char line[640];
	_snprintf_s(line, _TRUNCATE,
		"--- MEM %s load=%lu%% physAvailMB=%llu physTotalMB=%llu pageAvailMB=%llu workingSetMB=%llu privateMB=%llu ---\n",
		tag ? tag : "?",
		status.dwMemoryLoad,
		status.ullAvailPhys / (1024ull * 1024ull),
		status.ullTotalPhys / (1024ull * 1024ull),
		status.ullAvailPageFile / (1024ull * 1024ull),
		static_cast<unsigned long long>(pmc.WorkingSetSize / (1024ull * 1024ull)),
		static_cast<unsigned long long>(pmc.PrivateUsage / (1024ull * 1024ull)));

	SyncCriticalTrace(line);
}

static LONG CALLBACK CrashDiagnosticsVEH(PEXCEPTION_POINTERS info)
{
	if (!info || !info->ExceptionRecord)
	{
		return EXCEPTION_CONTINUE_SEARCH;
	}

	const DWORD code = info->ExceptionRecord->ExceptionCode;
	const char* name = nullptr;

	switch (code)
	{
		case EXCEPTION_STACK_OVERFLOW:
			name = "STACK_OVERFLOW";
			break;
		case EXCEPTION_IN_PAGE_ERROR:
			name = "IN_PAGE_ERROR";
			break;
		case STATUS_HEAP_CORRUPTION:
			name = "HEAP_CORRUPTION";
			break;
		case STATUS_STACK_BUFFER_OVERRUN:
			name = "STACK_BUFFER_OVERRUN_FASTFAIL";
			break;
		case STATUS_NO_MEMORY:
			name = "NO_MEMORY";
			break;
		case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
			name = "ARRAY_BOUNDS";
			break;
		case EXCEPTION_ILLEGAL_INSTRUCTION:
			name = "ILLEGAL_INSTRUCTION";
			break;
		case EXCEPTION_PRIV_INSTRUCTION:
			name = "PRIV_INSTRUCTION";
			break;
		case EXCEPTION_DATATYPE_MISALIGNMENT:
			name = "DATATYPE_MISALIGNMENT";
			break;
		case STATUS_FATAL_APP_EXIT:
			name = "FATAL_APP_EXIT";
			break;
		default:
			return EXCEPTION_CONTINUE_SEARCH;
	}

	char line[512];
	_snprintf_s(line, _TRUNCATE,
		"--- CRASH %s code=0x%08lX addr=%p tid=%lu ---\r\n",
		name,
		code,
		info->ExceptionRecord->ExceptionAddress,
		GetCurrentThreadId());

	if (code == EXCEPTION_STACK_OVERFLOW)
	{
		EmergencyWrite(line);
	}
	else
	{
		if (code == STATUS_NO_MEMORY || code == EXCEPTION_IN_PAGE_ERROR)
		{
			LogMemorySnapshot(name);
		}

		SyncCriticalTrace(line);
		FlushCrashLog();
	}

	return EXCEPTION_CONTINUE_SEARCH;
}

static int OnNewFailed(size_t)
{
	LogMemorySnapshot("OOM_new_handler");
	SyncCriticalTrace("--- CRASH OOM: operator new/malloc allocation failed ---\n");
	FlushCrashLog();
	return 0;
}

static void OnInvalidParameter(const wchar_t* expression, const wchar_t* function, const wchar_t* file, unsigned int line, uintptr_t)
{
	const auto exprN = expression ? ToNarrow(expression) : std::string{ "(null)" };
	const auto funcN = function ? ToNarrow(function) : std::string{ "(null)" };
	const auto fileN = file ? ToNarrow(file) : std::string{ "(null)" };

	char buf[768];
	_snprintf_s(buf, _TRUNCATE,
		"--- CRASH CRT invalid_parameter expr=%s func=%s file=%s line=%u ---\n",
		exprN.c_str(),
		funcN.c_str(),
		fileN.c_str(),
		line);
	SyncCriticalTrace(buf);
	FlushCrashLog();
}

static void OnPureCall()
{
	SyncCriticalTrace("--- CRASH CRT pure virtual call ---\n");
	FlushCrashLog();
}

static void OnAbortSignal(int)
{
	SyncCriticalTrace("--- CRASH SIGABRT/abort ---\n");
	FlushCrashLog();
}

static DWORD WINAPI MemoryWatchThread(LPVOID)
{
	SetThreadName(-1, "[Cfx] MemWatch");

	LogMemorySnapshot("startup");

	HANDLE lowMem = CreateMemoryResourceNotification(LowMemoryResourceNotification);
	ULONGLONG lastHeartbeat = GetTickCount64();
	ULONGLONG lastHighLoadLog = 0;

	for (;;)
	{
		DWORD waitResult = WAIT_TIMEOUT;

		if (lowMem)
		{
			waitResult = WaitForSingleObject(lowMem, 30000);
		}
		else
		{
			Sleep(30000);
		}

		if (waitResult == WAIT_OBJECT_0)
		{
			LogMemorySnapshot("LOW_MEMORY_WARNING");
			SyncCriticalTrace("--- WARN system low-memory notification (OOM/commit risk) ---\n");
			FlushCrashLog();
			Sleep(5000);
		}

		MEMORYSTATUSEX status = { sizeof(status) };
		GlobalMemoryStatusEx(&status);

		const ULONGLONG now = GetTickCount64();

		if (status.dwMemoryLoad >= 92 && (now - lastHighLoadLog) > 60000)
		{
			LogMemorySnapshot("HIGH_MEMORY_LOAD");
			FlushCrashLog();
			lastHighLoadLog = now;
		}

		if ((now - lastHeartbeat) >= (5ull * 60ull * 1000ull))
		{
			LogMemorySnapshot("heartbeat");
			lastHeartbeat = now;
		}
	}

	return 0;
}

static void InstallExtendedCrashDiagnostics()
{
	static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
	BOOL pending = FALSE;

	if (!InitOnceBeginInitialize(&once, 0, &pending, nullptr))
	{
		return;
	}

	if (!pending)
	{
		return;
	}

	AddVectoredExceptionHandler(1, CrashDiagnosticsVEH);

	_set_new_handler(OnNewFailed);
	_set_new_mode(1);
	_set_invalid_parameter_handler(OnInvalidParameter);
	_set_purecall_handler(OnPureCall);
	signal(SIGABRT, OnAbortSignal);

	CreateThread(nullptr, 0, MemoryWatchThread, nullptr, 0, nullptr);

	AsyncTrace("--- Extended crash diagnostics enabled (OOM/low-mem/stack/heap/fastfail/CRT/abort + 5m mem heartbeat) ---\n");

	InitOnceComplete(&once, 0, nullptr);
}

void InitLogging()
{
	SetThreadName(-1, "MainThrd");

	InstallExtendedCrashDiagnostics();

	if (OpenMutex(SYNCHRONIZE, FALSE, L"CitizenFX_LogMutex_Mod") == nullptr)
	{
		CreateMutex(nullptr, TRUE, L"CitizenFX_LogMutex_Mod");

		CreateDirectory(MakeRelativeCitPath(L"logs/").c_str(), NULL);

#if defined(LAUNCHER_PERSONALITY_MAIN)
		PruneOldLogs();
#endif

		AsyncTrace(fmt::format("--- BEGIN LOGGING AT {:%c} ---\n", fmt::gmtime(time(NULL))).c_str());
		AsyncTrace("--- Detailed crash logging enabled (sync flush on fatal/exception/terminate, 2-day retention) ---\n");
	}
}
