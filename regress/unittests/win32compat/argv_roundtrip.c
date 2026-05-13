/*
 * Round-trip helper for test_build_commandline_string.
 *
 * Builds a cmdline via build_commandline_string(echo-argv.exe, argv),
 * launches echo-argv.exe with that cmdline through CreateProcessW,
 * captures its stdout (each argv entry the child saw, printed as
 * "ARG[i]=<...>\n") and asserts it matches the input argv exactly.
 *
 * The test thus validates the full encoder + MSVCRT-decoder round trip,
 * the way posix_spawn / CreateProcess would behave at runtime.
 *
 * Test code: deliberately uses generously-sized static buffers instead
 * of dynamic allocation to keep the code straightforward.
 */
#include "includes.h"
#include <windows.h>
#include <misc_internal.h>

#include "../test_helper/test_helper.h"
#include "argv_roundtrip.h"

#define ROUNDTRIP_MAX_ARGV   64
#define ROUNDTRIP_BUF_SIZE   (64 * 1024)
#define ROUNDTRIP_WCMD_SIZE  8192
#define ROUNDTRIP_TIMEOUT_MS 10000	/* echo-argv must exit within this delay */
#define ROUNDTRIP_KILLED     0xDEAD	/* exit code given to an echo-argv killed on timeout */

/* Static state, shared across one call to assert_argv_roundtrip(). */
static char roundtrip_output[ROUNDTRIP_BUF_SIZE];
static char roundtrip_store[ROUNDTRIP_BUF_SIZE];
static char *roundtrip_argv[ROUNDTRIP_MAX_ARGV];
static wchar_t roundtrip_wcmdline[ROUNDTRIP_WCMD_SIZE];
static char roundtrip_helper[MAX_PATH * 3];	/* UTF-8: up to 3 bytes per UTF-16 unit */

/* Locate echo-argv.exe in the folder of the running unittest-win32compat.exe:
 * echo-argv.vcxproj builds it there, so it is copied along with the unit
 * tests. The path is returned in UTF-8, the encoding run_echo_argv() converts
 * back to UTF-16 for CreateProcessW. */
static char *
find_echo_argv(void)
{
	wchar_t self[MAX_PATH];
	DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH);
	if (n == 0 || n >= MAX_PATH)
		return NULL;
	wchar_t *p = wcsrchr(self, L'\\');
	if (!p) return NULL;
	*(p + 1) = L'\0';
	if (wcslen(self) + wcslen(L"echo-argv.exe") >= MAX_PATH)
		return NULL;
	wcscat_s(self, MAX_PATH, L"echo-argv.exe");
	if (WideCharToMultiByte(CP_UTF8, 0, self, -1, roundtrip_helper,
	    (int)sizeof(roundtrip_helper), NULL, NULL) == 0)
		return NULL;
	return roundtrip_helper;
}

/* Read everything from a HANDLE pipe until EOF into the static buffer.
 * Returns pointer to roundtrip_output (NUL-terminated), or NULL on overflow. */
static char *
slurp_pipe(HANDLE h, size_t *out_len)
{
	size_t len = 0;
	for (;;) {
		if (len + 1 >= sizeof(roundtrip_output))
			return NULL;
		DWORD got = 0;
		BOOL ok = ReadFile(h, roundtrip_output + len,
			(DWORD)(sizeof(roundtrip_output) - len - 1), &got, NULL);
		if (!ok || got == 0) break;
		len += got;
	}
	roundtrip_output[len] = '\0';
	if (out_len) *out_len = len;
	return roundtrip_output;
}

/* Parse echo-argv stdout: each line is "ARG[i]=<...>\r?\n".
 * Fills roundtrip_argv[] with pointers into roundtrip_store[]. */
static char **
parse_echo_argv_output(const char *buf, int *out_argc)
{
	*out_argc = 0;
	int n = 0;
	size_t store_used = 0;
	const char *p = buf;
	while ((p = strstr(p, "ARG[")) != NULL) {
		const char *open = strchr(p, '<');
		if (!open) break;
		const char *next = strstr(open + 1, "ARG[");
		const char *end = next ? next : buf + strlen(buf);
		const char *close = end - 1;
		while (close > open && (*close == '\n' || *close == '\r'))
			close--;
		if (*close != '>') break;
		const char *content = open + 1;
		size_t len = close - content;
		if (n >= ROUNDTRIP_MAX_ARGV) break;
		if (store_used + len + 1 > sizeof(roundtrip_store)) break;
		char *s = roundtrip_store + store_used;
		memcpy(s, content, len);
		s[len] = '\0';
		store_used += len + 1;
		roundtrip_argv[n++] = s;
		p = next ? next : end;
	}
	*out_argc = n;
	return roundtrip_argv;
}

/* Watchdog of run_echo_argv(): kills echo-argv if it has not exited within
 * ROUNDTRIP_TIMEOUT_MS. Its death closes its end of the pipe, which unblocks
 * slurp_pipe(), so a hung helper fails the test instead of blocking the suite. */
static DWORD WINAPI
kill_echo_argv_on_timeout(LPVOID process)
{
	if (WaitForSingleObject((HANDLE)process, ROUNDTRIP_TIMEOUT_MS) == WAIT_TIMEOUT)
		TerminateProcess((HANDLE)process, ROUNDTRIP_KILLED);
	return 0;
}

/* Run echo-argv with build_commandline_string(echo-argv, argv) and
 * return what the child saw. *out_argc includes argv[0] (helper path).
 *
 * Mirrors what posix_spawn does in w32fd.c: convert the UTF-8 cmdline to
 * UTF-16 and call CreateProcessW directly. CreateProcessA would route
 * through the ANSI code page and silently lose non-ASCII bytes. */
static char **
run_echo_argv(char *const argv[], int *out_argc)
{
	char *helper = find_echo_argv();
	if (!helper) return NULL;
	char *cmdline = build_commandline_string(helper, argv, FALSE);
	if (!cmdline) return NULL;

	int wlen = MultiByteToWideChar(CP_UTF8, 0, cmdline, -1, NULL, 0);
	if (wlen <= 0 || (size_t)wlen > ROUNDTRIP_WCMD_SIZE) { free(cmdline); return NULL; }
	MultiByteToWideChar(CP_UTF8, 0, cmdline, -1, roundtrip_wcmdline, wlen);
	free(cmdline);

	HANDLE rd = NULL, wr = NULL;
	SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
	if (!CreatePipe(&rd, &wr, &sa, 0)) return NULL;
	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOW si;
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = wr;
	si.hStdError = wr;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	PROCESS_INFORMATION pi;
	memset(&pi, 0, sizeof(pi));

	if (!CreateProcessW(NULL, roundtrip_wcmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
		CloseHandle(rd); CloseHandle(wr);
		return NULL;
	}
	CloseHandle(wr);
	HANDLE guard = CreateThread(NULL, 0, kill_echo_argv_on_timeout, pi.hProcess, 0, NULL);
	if (guard == NULL) {
		DWORD err = GetLastError();
		/* no watchdog: do not risk an unbounded wait */
		TerminateProcess(pi.hProcess, ROUNDTRIP_KILLED);
		WaitForSingleObject(pi.hProcess, INFINITE);
		CloseHandle(rd);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		fprintf(stderr, "argv round trip: cannot create the echo-argv watchdog (error %lu)\n", err);
		return NULL;
	}
	size_t len;
	char *output = slurp_pipe(rd, &len);
	/* echo-argv has exited or is killed by the watchdog: both waits are bounded */
	WaitForSingleObject(pi.hProcess, INFINITE);
	WaitForSingleObject(guard, INFINITE);
	CloseHandle(guard);
	DWORD code = 0;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(rd);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	if (code == ROUNDTRIP_KILLED) {
		fprintf(stderr, "argv round trip: echo-argv did not exit within %d ms, killed\n", ROUNDTRIP_TIMEOUT_MS);
		return NULL;
	}
	if (!output) return NULL;
	return parse_echo_argv_output(output, out_argc);
}

/* Assert that running echo-argv with argv[] yields the same argv[] back.
 * The first received entry is argv[0] = path to echo-argv.exe; we skip it
 * and compare received[1..n] against argv[0..n-1] (the caller's array).
 * Failure is reported via ASSERT_STRING_EQ on the offending argv slot. */
void
assert_argv_roundtrip(char *const argv[])
{
	int recv_argc = 0;
	char **recv = run_echo_argv(argv, &recv_argc);
	ASSERT_PTR_NE(recv, NULL);
	int exp_argc = 0;
	while (argv[exp_argc] != NULL) exp_argc++;
	ASSERT_INT_EQ(recv_argc, exp_argc + 1);
	for (int i = 0; i < exp_argc; i++)
		ASSERT_STRING_EQ(recv[i + 1], argv[i]);
}
