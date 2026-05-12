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
 */
#include "includes.h"
#include <windows.h>
#include <misc_internal.h>

#include "../test_helper/test_helper.h"
#include "argv_roundtrip.h"

/* Locate echo-argv.exe living next to the parent dir of unittest-win32compat.exe.
 * The unittest binary lives in bin\<plat>\<conf>\unittest-win32compat\
 * and the helper lives in bin\<plat>\<conf>\echo-argv.exe (one level up). */
static char *
find_echo_argv(void)
{
	static char path[MAX_PATH];
	char self[MAX_PATH];
	DWORD n = GetModuleFileNameA(NULL, self, MAX_PATH);
	if (n == 0 || n >= MAX_PATH)
		return NULL;
	char *p = strrchr(self, '\\');
	if (!p) return NULL;
	*p = '\0';
	p = strrchr(self, '\\');
	if (!p) return NULL;
	*p = '\0';
	snprintf(path, sizeof(path), "%s\\echo-argv.exe", self);
	return path;
}

/* Read everything from a HANDLE pipe until EOF. */
static char *
slurp_pipe(HANDLE h, size_t *out_len)
{
	size_t cap = 4096, len = 0;
	char *buf = malloc(cap);
	if (!buf) return NULL;
	for (;;) {
		if (len + 1024 > cap) {
			cap *= 2;
			char *nb = realloc(buf, cap);
			if (!nb) { free(buf); return NULL; }
			buf = nb;
		}
		DWORD got = 0;
		BOOL ok = ReadFile(h, buf + len, (DWORD)(cap - len - 1), &got, NULL);
		if (!ok || got == 0) break;
		len += got;
	}
	buf[len] = '\0';
	if (out_len) *out_len = len;
	return buf;
}

/* Parse echo-argv stdout: each line is "ARG[i]=<...>\r?\n".
 * For each "ARG[" occurrence, take everything between the first '<' after
 * the '=' and the last '>' on that "line" (until the next "ARG[" or EOF,
 * after trimming trailing CR/LF). */
static char **
parse_echo_argv_output(const char *buf, int *out_argc)
{
	*out_argc = 0;
	int cap = 16, n = 0;
	char **argv = malloc(cap * sizeof(char *));
	if (!argv) return NULL;
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
		char *s = malloc(len + 1);
		if (!s) break;
		memcpy(s, content, len);
		s[len] = '\0';
		if (n >= cap) {
			cap *= 2;
			char **na = realloc(argv, cap * sizeof(char *));
			if (!na) { free(s); break; }
			argv = na;
		}
		argv[n++] = s;
		p = next ? next : end;
	}
	*out_argc = n;
	return argv;
}

static void
free_parsed_argv(char **argv, int argc)
{
	if (!argv) return;
	for (int i = 0; i < argc; i++) free(argv[i]);
	free(argv);
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
	wchar_t *wcmdline = malloc(wlen * sizeof(wchar_t));
	if (!wcmdline) { free(cmdline); return NULL; }
	MultiByteToWideChar(CP_UTF8, 0, cmdline, -1, wcmdline, wlen);

	HANDLE rd = NULL, wr = NULL;
	SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
	if (!CreatePipe(&rd, &wr, &sa, 0)) { free(wcmdline); free(cmdline); return NULL; }
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

	if (!CreateProcessW(NULL, wcmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
		CloseHandle(rd); CloseHandle(wr); free(wcmdline); free(cmdline);
		return NULL;
	}
	CloseHandle(wr);
	size_t len;
	char *output = slurp_pipe(rd, &len);
	WaitForSingleObject(pi.hProcess, INFINITE);
	CloseHandle(rd);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	free(wcmdline);
	free(cmdline);
	if (!output) return NULL;
	char **received = parse_echo_argv_output(output, out_argc);
	free(output);
	return received;
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
	free_parsed_argv(recv, recv_argc);
}
