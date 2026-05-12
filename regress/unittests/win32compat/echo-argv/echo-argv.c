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
 */
#include <stdio.h>

int main(int argc, char **argv)
{
	for (int i = 0; i < argc; i++)
		printf("ARG[%d]=<%s>\n", i, argv[i]);
	return 0;
}
