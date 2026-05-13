/*
 * Test helper used by test_build_commandline_string round-trip tests.
 * Prints its own argv on stdout, in UTF-8, in the form:
 *     ARG[0]=<...>
 *     ARG[1]=<...>
 *     ...
 * wmain receives the arguments in UTF-16, so the output does not depend on
 * the ANSI code page of the machine (the narrow argv of main() is converted
 * to it, which loses every character that code page cannot represent).
 * The angle brackets always appear at the start/end of each line; if an
 * argv entry itself contains '<' or '>' they are emitted verbatim and
 * the parser disambiguates by anchoring on "ARG[i]=<" and on the
 * trailing ">\n".
 *
 * stdout is set to binary mode so a '\n' inside an argv entry stays a
 * lone LF (not silently converted to CRLF by the CRT in text mode).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <io.h>
#include <fcntl.h>

static void
put_utf8(const wchar_t *w)
{
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	char *s = n > 0 ? malloc(n) : NULL;
	if (s != NULL && WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) > 0)
		fputs(s, stdout);
	free(s);
}

int
wmain(int argc, wchar_t **argv)
{
	_setmode(_fileno(stdout), _O_BINARY);
	for (int i = 0; i < argc; i++) {
		printf("ARG[%d]=<", i);
		put_utf8(argv[i]);
		fputs(">\n", stdout);
	}
	return 0;
}
