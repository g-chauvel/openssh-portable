/*
 * Round-trip helper for test_build_commandline_string.
 * See argv_roundtrip.c for details.
 */
#ifndef ARGV_ROUNDTRIP_H
#define ARGV_ROUNDTRIP_H

void assert_argv_roundtrip(char *const argv[]);
void assert_cmdline_roundtrip(const char *cmd, int prepend_module_path, char *const expected[]);

#define ASSERT_ARGV_ROUNDTRIP(argv) assert_argv_roundtrip(argv)
#define ASSERT_CMDLINE_ROUNDTRIP(cmd, prepend_module_path, expected) \
	assert_cmdline_roundtrip(cmd, prepend_module_path, expected)

#endif
