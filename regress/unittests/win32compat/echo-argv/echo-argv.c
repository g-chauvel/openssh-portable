/*
 * Test helper used by test_build_commandline_string round-trip tests.
 * Prints its own argv on stdout in the form:
 *     ARG[0]=<...>
 *     ARG[1]=<...>
 *     ...
 * The angle brackets always appear at the start/end of each line; if an
 * argv entry itself contains '<' or '>' they are emitted verbatim and
 * the parser disambiguates by anchoring on "ARG[i]=<" and on the
 * trailing ">\n".
 *
 * stdout is set to binary mode so a '\n' inside an argv entry stays a
 * lone LF (not silently converted to CRLF by the CRT in text mode).
 */
#include <stdio.h>
#include <io.h>
#include <fcntl.h>

int main(int argc, char **argv)
{
	_setmode(_fileno(stdout), _O_BINARY);
	for (int i = 0; i < argc; i++)
		printf("ARG[%d]=<%s>\n", i, argv[i]);
	return 0;
}
