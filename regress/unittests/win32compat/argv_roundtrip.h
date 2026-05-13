/*
 * Round-trip helper for test_build_commandline_string.
 * See argv_roundtrip.c for details.
 */
#ifndef ARGV_ROUNDTRIP_H
#define ARGV_ROUNDTRIP_H

void assert_argv_roundtrip(char *const argv[]);

#define ASSERT_ARGV_ROUNDTRIP(argv) assert_argv_roundtrip(argv)

#endif
