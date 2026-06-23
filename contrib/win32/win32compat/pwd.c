/*
 * Author: NoMachine <developers@nomachine.com>
 *
 * Author: Bryan Berns <berns@uwalumni.com>
 *   Normalized and optimized login routines and added support for
 *   internet-linked accounts.
 *
 * Copyright (c) 2009, 2011 NoMachine
 * All rights reserved
 *
 * Support functions and system calls' replacements needed to let the
 * software run on Win32 based operating systems.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <Windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <LM.h>
#include <sddl.h>
#include <DsGetDC.h>
#include <shellapi.h>
#define SECURITY_WIN32
#include <security.h>

#include "inc\pwd.h"
#include "inc\grp.h"
#include "inc\utf.h"
#include "misc_internal.h"
#include "debug.h"

static struct passwd pw;
static char* pw_shellpath = NULL;
char* shell_command_option = NULL;
char** shell_command_option_argv = NULL;   /* tokenized DefaultShellCommandOption, NULL-terminated */
int shell_command_option_argc = 0;          /* number of tokens (excludes terminating NULL) */
char* shell_arguments = NULL;
BOOLEAN arg_escape = TRUE;

/*
 * Tokenize a registry-value command-line string into UTF-8 argv[].
 * Uses CommandLineToArgvW with a dummy program name prefix to dodge its
 * special argv[0] parsing rule (which does NOT honor backslash-quote escapes
 * — see https://learn.microsoft.com/en-us/cpp/c-language/parsing-c-command-line-arguments).
 * Returns NULL-terminated argv[] (caller-allocated; must be freed via
 * free_tokenized_default_shell_command_option()), sets *out_argc to the
 * number of tokens (excluding the NULL terminator).
 * Returns NULL on failure (or empty input) with *out_argc = 0.
 */
char**
tokenize_default_shell_command_option(const wchar_t* value, int* out_argc)
{
	if (out_argc == NULL)
		return NULL;
	*out_argc = 0;
	if (value == NULL || value[0] == L'\0')
		return NULL;

	/* Prepend dummy program name so CommandLineToArgvW applies standard parsing
	 * to every real token (the special argv[0] rule consumes "x" instead). */
	size_t value_len = wcslen(value);
	wchar_t* prefixed = malloc((value_len + 3) * sizeof(wchar_t));   /* "x " + value + L'\0' */
	if (prefixed == NULL)
		return NULL;
	prefixed[0] = L'x';
	prefixed[1] = L' ';
	memcpy(prefixed + 2, value, (value_len + 1) * sizeof(wchar_t));

	int wargc = 0;
	LPWSTR* wargv = CommandLineToArgvW(prefixed, &wargc);
	free(prefixed);
	if (wargv == NULL || wargc < 1)
		return NULL;

	/* Skip the dummy at wargv[0]; convert wargv[1..wargc-1] to UTF-8. */
	int real_argc = wargc - 1;
	char** argv = calloc(real_argc + 1, sizeof(char*));   /* +1 for NULL terminator */
	if (argv == NULL) {
		LocalFree(wargv);
		return NULL;
	}
	for (int i = 0; i < real_argc; i++) {
		argv[i] = utf16_to_utf8(wargv[i + 1]);
		if (argv[i] == NULL) {
			for (int j = 0; j < i; j++)
				free(argv[j]);
			free(argv);
			LocalFree(wargv);
			return NULL;
		}
	}
	LocalFree(wargv);
	*out_argc = real_argc;
	return argv;
}

/* Free a NULL-terminated argv returned by tokenize_default_shell_command_option. */
void
free_tokenized_default_shell_command_option(char** argv)
{
	if (argv == NULL)
		return;
	for (int i = 0; argv[i] != NULL; i++)
		free(argv[i]);
	free(argv);
}

/*
 * Shell configuration from a dedicated file, used in place of the registry.
 *
 * This is deliberately a SEPARATE file, NOT sshd_config: parsing sshd_config
 * keywords would require touching the base OpenSSH layer (servconf.c), whereas
 * everything here stays inside the Windows adaptation layer (contrib\win32).
 *
 * File location (first match wins):
 *   1. %SSHD_SHELL_CONFIG%                  explicit path - ideal for user-level
 *                                           foreground testing (no admin needed)
 *   2. <dir of the running binary>\shell_config
 *
 * Format: one "Keyword value" per line, '#' starts a comment, blank lines are
 * ignored. Keywords reuse the registry names (case-insensitive):
 *   DefaultShell                 C:\Windows\System32\cmd.exe
 *   DefaultShellCommandOption    /c
 *   DefaultShellArguments        -NoLogo -NoProfile
 *   DefaultShellEscapeArguments  yes        (yes/no | true/false | 1/0)
 *
 * The value is the remainder of the line (so unquoted paths with spaces work);
 * a single pair of surrounding double quotes, if present, is stripped.
 *
 * Returns 1 only if DefaultShell was found, in which case the file provides the
 * shell config EXCLUSIVELY and the registry is not consulted. Returns 0 (with
 * the output buffers left empty) if the file is absent, unreadable, or has no
 * DefaultShell, so the caller falls back to the registry.
 */
static int
read_shell_config_file(wchar_t *path_buf, size_t path_buf_cch,
    wchar_t *option_buf, size_t option_buf_cch,
    wchar_t *arg_buf, size_t arg_buf_cch,
    BOOLEAN *escape, BOOLEAN *escape_set)
{
	wchar_t file_path[PATH_MAX];
	DWORD env_len;
	FILE *f = NULL;
	char *content = NULL, *line, *ctx = NULL;
	long file_size;
	size_t read_len;
	int found_shell = 0;

	path_buf[0] = option_buf[0] = arg_buf[0] = L'\0';
	*escape_set = FALSE;

	/* resolve the file path: %SSHD_SHELL_CONFIG% override, else next to the
	 * running binary. GetEnvironmentVariableW returns 0 if unset and the needed
	 * length (>= buffer size) if it would not fit. */
	env_len = GetEnvironmentVariableW(L"SSHD_SHELL_CONFIG", file_path,
	    (DWORD)_countof(file_path));
	if (env_len == 0 || env_len >= _countof(file_path)) {
		if (__wprogdir == NULL ||
		    _snwprintf_s(file_path, _countof(file_path), _TRUNCATE,
		        L"%s\\shell_config", __wprogdir) < 0)
			return 0;
	}

	if (_wfopen_s(&f, file_path, L"rb") != 0 || f == NULL)
		return 0;	/* no file -> caller uses the registry */

	{
		char *fp_utf8 = utf16_to_utf8(file_path);
		debug3("%s: reading shell config from %s", __func__,
		    fp_utf8 != NULL ? fp_utf8 : "(?)");
		if (fp_utf8 != NULL)
			free(fp_utf8);
	}

	if (fseek(f, 0, SEEK_END) != 0 ||
	    (file_size = ftell(f)) <= 0 || file_size > 1024 * 1024 ||
	    fseek(f, 0, SEEK_SET) != 0)
		goto done;

	if ((content = malloc((size_t)file_size + 1)) == NULL)
		goto done;
	read_len = fread(content, 1, (size_t)file_size, f);
	content[read_len] = '\0';

	/* skip a UTF-8 BOM if present */
	line = content;
	if (read_len >= 3 && (unsigned char)content[0] == 0xEF &&
	    (unsigned char)content[1] == 0xBB && (unsigned char)content[2] == 0xBF)
		line += 3;

	for (line = strtok_s(line, "\r\n", &ctx); line != NULL;
	     line = strtok_s(NULL, "\r\n", &ctx)) {
		char *key, *val, *end;
		wchar_t *val_w, *dst = NULL;
		size_t dst_cch = 0;

		/* trim leading whitespace; skip blank lines and comments */
		while (*line == ' ' || *line == '\t')
			line++;
		if (*line == '\0' || *line == '#')
			continue;

		/* split key / value on the first run of whitespace */
		key = line;
		while (*line != '\0' && *line != ' ' && *line != '\t')
			line++;
		if (*line != '\0') {
			*line++ = '\0';
			while (*line == ' ' || *line == '\t')
				line++;
		}
		val = line;

		/* trim trailing whitespace */
		end = val + strlen(val);
		while (end > val && (end[-1] == ' ' || end[-1] == '\t'))
			*--end = '\0';

		/* strip a single pair of surrounding double quotes */
		if (end - val >= 2 && val[0] == '"' && end[-1] == '"') {
			end[-1] = '\0';
			val++;
		}

		if (_stricmp(key, "DefaultShellEscapeArguments") == 0) {
			if (_stricmp(val, "yes") == 0 || _stricmp(val, "true") == 0 ||
			    strcmp(val, "1") == 0) {
				*escape = TRUE;
				*escape_set = TRUE;
			} else if (_stricmp(val, "no") == 0 || _stricmp(val, "false") == 0 ||
			    strcmp(val, "0") == 0) {
				*escape = FALSE;
				*escape_set = TRUE;
			} else
				error("%s: invalid DefaultShellEscapeArguments value '%s'",
				    __func__, val);
			continue;
		}

		if (_stricmp(key, "DefaultShell") == 0) {
			dst = path_buf;
			dst_cch = path_buf_cch;
		} else if (_stricmp(key, "DefaultShellCommandOption") == 0) {
			dst = option_buf;
			dst_cch = option_buf_cch;
		} else if (_stricmp(key, "DefaultShellArguments") == 0) {
			dst = arg_buf;
			dst_cch = arg_buf_cch;
		} else {
			debug3("%s: ignoring unknown keyword '%s'", __func__, key);
			continue;
		}

		if (*val == '\0')
			continue;
		if ((val_w = utf8_to_utf16(val)) == NULL)
			goto done;
		if (wcscpy_s(dst, dst_cch, val_w) == 0 && dst == path_buf)
			found_shell = 1;
		free(val_w);
	}

done:
	if (f != NULL)
		fclose(f);
	if (content != NULL)
		free(content);
	if (!found_shell) {
		/* leave nothing behind for the registry fallback path */
		path_buf[0] = option_buf[0] = arg_buf[0] = L'\0';
		*escape_set = FALSE;
	}
	return found_shell;
}

/* returns 0 on success, and -1 with errno set on failure */
static int
set_defaultshell()
{
	HKEY reg_key = 0;
	int tmp_len, ret = -1;
	REGSAM mask = STANDARD_RIGHTS_READ | KEY_QUERY_VALUE | KEY_WOW64_64KEY;
	wchar_t path_buf[PATH_MAX], option_buf[PATH_MAX], arg_buf[PATH_MAX];
	char *pw_shellpath_local = NULL, *command_option_local = NULL, *shell_arguments_local = NULL;
	char **command_option_argv_local = NULL;
	int command_option_argc_local = 0;
	BOOLEAN file_escape = TRUE, file_escape_set = FALSE;

	errno = 0;

	/* if already set, return success */
	if (pw_shellpath != NULL)
		return 0;

	path_buf[0] = L'\0';
	option_buf[0] = L'\0';
	arg_buf[0] = L'\0';

	tmp_len = _countof(path_buf);
	if (read_shell_config_file(path_buf, _countof(path_buf),
	    option_buf, _countof(option_buf), arg_buf, _countof(arg_buf),
	    &file_escape, &file_escape_set)) {
		/*
		 * Shell config came from the dedicated file (see
		 * read_shell_config_file). By design the registry is NOT consulted
		 * in this case; path_buf / option_buf / arg_buf are already
		 * populated and flow through the common conversion tail below.
		 */
		if (file_escape_set)
			arg_escape = (file_escape != 0) ? TRUE : FALSE;
	} else if ((RegOpenKeyExW(HKEY_LOCAL_MACHINE, SSH_REGISTRY_ROOT, 0, mask, &reg_key) == ERROR_SUCCESS) &&
	    (RegQueryValueExW(reg_key, L"DefaultShell", 0, NULL, (LPBYTE)path_buf, &tmp_len) == ERROR_SUCCESS) &&
	    (path_buf[0] != L'\0')) {
		/* fetched default shell path from registry */
		tmp_len = _countof(option_buf);
		DWORD size = sizeof(DWORD);
		DWORD escape_option = 1;
		if (RegQueryValueExW(reg_key, L"DefaultShellCommandOption", 0, NULL, (LPBYTE)option_buf, &tmp_len) != ERROR_SUCCESS)
			option_buf[0] = L'\0';

		tmp_len = _countof(arg_buf);
		if (RegQueryValueExW(reg_key, L"DefaultShellArguments", 0, NULL, (LPBYTE)arg_buf, &tmp_len) != ERROR_SUCCESS)
			arg_buf[0] = L'\0';

		if (RegQueryValueExW(reg_key, L"DefaultShellEscapeArguments", 0, NULL, (LPBYTE)&escape_option, &size) == ERROR_SUCCESS)
			arg_escape = (escape_option != 0) ? TRUE : FALSE;
	} else {
		if (!GetSystemDirectoryW(path_buf, _countof(path_buf))) {
			errno = GetLastError();
			goto cleanup;
		}
		if (wcscat_s(path_buf, _countof(path_buf), L"\\cmd.exe") != 0)
			goto cleanup;
	}

	if ((pw_shellpath_local = utf16_to_utf8(path_buf)) == NULL)
		goto cleanup;

	if (option_buf[0] != L'\0')
		if ((command_option_local = utf16_to_utf8(option_buf)) == NULL)
			goto cleanup;

	if (arg_buf[0] != L'\0')
		if ((shell_arguments_local = utf16_to_utf8(arg_buf)) == NULL)
			goto cleanup;

	/* Tokenize the option string for callers that need to forward each switch
	 * as a separate argv entry (e.g. "-NoLogo -NoProfile -Command" must reach
	 * PowerShell as three arguments, not a single quoted blob). */
	if (option_buf[0] != L'\0') {
		command_option_argv_local = tokenize_default_shell_command_option(option_buf, &command_option_argc_local);
		if (command_option_argv_local == NULL)
			goto cleanup;
	}

	convertToBackslash(pw_shellpath_local);
	to_lower_case(pw_shellpath_local);
	pw_shellpath = pw_shellpath_local;
	pw_shellpath_local = NULL;
	shell_command_option = command_option_local;
	shell_command_option_argv = command_option_argv_local;
	shell_command_option_argc = command_option_argc_local;
	shell_arguments = shell_arguments_local;
	command_option_local = NULL;
	command_option_argv_local = NULL;
	shell_arguments_local = NULL;

	ret = 0;
cleanup:
	if (pw_shellpath_local)
		free(pw_shellpath_local);

	if (command_option_local)
		free(command_option_local);

	if (command_option_argv_local)
		free_tokenized_default_shell_command_option(command_option_argv_local);

	if (shell_arguments_local)
		free(shell_arguments_local);

	return ret;
}


int
initialize_pw()
{
	if (set_defaultshell() != 0)
		return -1;

	if (pw.pw_shell != pw_shellpath) {
		memset(&pw, 0, sizeof(pw));
		pw.pw_shell = pw_shellpath;
		pw.pw_passwd = "\0";
		/* pw_uid = 0 for root on Unix and SSH code has specific restrictions for root
		 * that are not applicable in Windows */
		pw.pw_uid = 1;
	}
	return 0;
}

static void 
clean_pw()
{
	if (pw.pw_name)
		free(pw.pw_name);
	if (pw.pw_dir)
		free(pw.pw_dir);
	pw.pw_name = NULL;
	pw.pw_dir = NULL;
}

static int
reset_pw()
{
	if (initialize_pw() != 0)
		return -1;

	clean_pw();

	return 0;
}

static struct passwd*
get_passwd(const wchar_t * user_utf16, PSID sid)
{
	wchar_t user_resolved[DNLEN + 1 + UNLEN + 1];
	struct passwd *ret = NULL;
	wchar_t *sid_string = NULL, *tmp = NULL, *user_utf16_modified = NULL;
	wchar_t reg_path[PATH_MAX], profile_home[PATH_MAX], profile_home_exp[PATH_MAX];
	DWORD reg_path_len = PATH_MAX;
	HKEY reg_key = 0;	
	
	BYTE binary_sid[SECURITY_MAX_SID_SIZE];
	DWORD sid_size = ARRAYSIZE(binary_sid);
	WCHAR domain_name[DNLEN + 1] = L"";
	DWORD domain_name_size = DNLEN + 1;
	SID_NAME_USE account_type = 0;

	errno = 0;
	if (reset_pw() != 0)
		return NULL;
	
	/*
	 * We support both "domain\user" and "domain/user" formats.
	 * But win32 APIs only accept domain\user format so convert it.
	 */
	if (user_utf16) {
		user_utf16_modified = _wcsdup(user_utf16);
		if (!user_utf16_modified) {
			errno = ENOMEM;
			error("%s failed to duplicate %s", __func__, user_utf16);
			goto cleanup;
		}

		if (tmp = wcsstr(user_utf16_modified, L"/"))
			*tmp = L'\\';
	}

	/* skip forward lookup on name if sid was passed in */
	if (sid != NULL)
		CopySid(sizeof(binary_sid), binary_sid, sid);
	/* else attempt to lookup the account; this will verify the account is valid and
	 * is will return its sid and the realm that owns it */
	else if (lookup_sid(user_utf16_modified, binary_sid, &sid_size) == NULL) {
		debug("%s: lookup_sid() failed: %d.", __FUNCTION__, errno);
		goto cleanup;
	}

	/* convert the binary string to a string */
	if (ConvertSidToStringSidW((PSID) binary_sid, &sid_string) == FALSE) {
		errno = errno_from_Win32LastError();
		goto cleanup;
	}

	/* lookup the account name from the sid */
	WCHAR user_name[UNLEN + 1];
	DWORD user_name_length = ARRAYSIZE(user_name);
	domain_name_size = DNLEN + 1;
	if (LookupAccountSidW(NULL, binary_sid, user_name, &user_name_length,
	    domain_name, &domain_name_size, &account_type) == 0) {
		errno = errno_from_Win32LastError();
		debug("%s: LookupAccountSid() failed: %d.", __FUNCTION__, GetLastError());
		goto cleanup;
	}

	/* verify passed account is actually a user account */
	if (account_type != SidTypeUser) {
		errno = ENOENT;
		debug3("%s: Invalid account type: %d.", __FUNCTION__, account_type);
		goto cleanup;
	}

	/* fetch the computer name so we can determine if the specified user is local or not */
	wchar_t computer_name[CNLEN + 1];
	DWORD computer_name_size = ARRAYSIZE(computer_name);
	if (GetComputerNameW(computer_name, &computer_name_size) == 0) {
		error_f("GetComputerNameW() failed with error:%d", GetLastError());
		goto cleanup;
	}

	/* if standard local user name or system account, just use name without decoration */
	const SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
	if ((_wcsicmp(domain_name, computer_name) == 0) ||
		((memcmp(&nt_authority, GetSidIdentifierAuthority((PSID)binary_sid), sizeof(SID_IDENTIFIER_AUTHORITY)) == 0) &&
		 (((SID*)binary_sid)->SubAuthority[0] == SECURITY_LOCAL_SYSTEM_RID))) {
		wcscpy_s(user_resolved, ARRAYSIZE(user_resolved), user_name);
	}

	/* put any other format in sam compatible format */
	else
		swprintf_s(user_resolved, ARRAYSIZE(user_resolved), L"%s\\%s", domain_name, user_name);

	/* if one of below fails, set profile path to Windows directory */
	if (swprintf_s(reg_path, PATH_MAX, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList\\%ls", sid_string) == -1 ||
	    RegOpenKeyExW(HKEY_LOCAL_MACHINE, reg_path, 0, STANDARD_RIGHTS_READ | KEY_QUERY_VALUE | KEY_WOW64_64KEY, &reg_key) != 0 ||
	    RegQueryValueExW(reg_key, L"ProfileImagePath", 0, NULL, (LPBYTE)profile_home, &reg_path_len) != 0 ||
	    ExpandEnvironmentStringsW(profile_home, NULL, 0) > PATH_MAX ||
	    ExpandEnvironmentStringsW(profile_home, profile_home_exp, PATH_MAX) == 0)
		if (GetWindowsDirectoryW(profile_home_exp, PATH_MAX) == 0) {
			debug3("GetWindowsDirectoryW failed with %d", GetLastError());
			errno = EOTHER;
			goto cleanup;
		}

	/* convert to utf8, make name lowercase, and assign to output structure*/
	_wcslwr_s(user_resolved, wcslen(user_resolved) + 1);
	if ((pw.pw_name = utf16_to_utf8(user_resolved)) == NULL ||
	    (pw.pw_dir = utf16_to_utf8(profile_home_exp)) == NULL) {
		clean_pw();
		errno = ENOMEM;
		goto cleanup;
	}

	ret = &pw;

cleanup:

	if (sid_string)
		LocalFree(sid_string);
	if (reg_key)
		RegCloseKey(reg_key);

	return ret;
}

static struct passwd*
getpwnam_placeholder(const char* user) {
	wchar_t tmp_home[PATH_MAX];
	char *pw_name = NULL, *pw_dir = NULL;
	struct passwd* ret = NULL;

	if (GetWindowsDirectoryW(tmp_home, PATH_MAX) == 0) {
		debug3("GetWindowsDirectoryW failed with %d", GetLastError());
		errno = EOTHER;
		goto cleanup;
	}
	pw_name = _strdup(user);
	pw_dir = utf16_to_utf8(tmp_home);

	if (!pw_name || !pw_dir) {
		errno = ENOMEM;
		goto cleanup;
	}

	pw.pw_name = pw_name;
	pw_name = NULL;
	pw.pw_dir = pw_dir;
	pw_dir = NULL;

	ret = &pw;
cleanup:
	if (pw_name)
		free(pw_name);
	if (pw_dir)
		free(pw_dir);

	return ret;
}

char *
get_username(const PSID sid)
{
	if (!sid) {
		error_f("sid is NULL");
		return NULL;
	}

	struct passwd *p = get_passwd(NULL, sid);
	if (p && p->pw_name)
		return _strdup(p->pw_name);
	else
		return NULL;
}

struct passwd*
w32_getpwnam(const char *user_utf8)
{
	struct passwd* ret = NULL;
	wchar_t * user_utf16 = NULL;

	user_utf16 = utf8_to_utf16(user_utf8);
	if (user_utf16 == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	ret = get_passwd(user_utf16, NULL);
	if (ret != NULL)
		goto done;

	/* for unpriviliged user account, create placeholder and return*/
	if (_stricmp(user_utf8, "sshd") == 0) {
		ret = getpwnam_placeholder(user_utf8);
		goto done;
	}

	/* check if custom passwd auth is enabled */
	if (get_custom_lsa_package())
		ret = getpwnam_placeholder(user_utf8);

done:
	if (user_utf16)
		free(user_utf16);
	return ret;
}

struct passwd*
w32_getpwuid(uid_t uid)
{
	struct passwd* ret = NULL;
	PSID cur_user_sid = NULL;
	
	if ((cur_user_sid = get_sid(NULL)) == NULL)
		goto cleanup;

	ret = get_passwd(NULL, cur_user_sid);

cleanup:
	if (cur_user_sid)
		free(cur_user_sid);

	return ret;
}

char *
group_from_gid(gid_t gid, int nogroup)
{
	return "-";
}

char *
user_from_uid(uid_t uid, int nouser)
{
	return "-";
}

uid_t
w32_getuid(void)
{
	return 1;
}

gid_t
getgid(void)
{
	return 0;
}

uid_t
geteuid(void)
{
	return 1;
}

gid_t
getegid(void)
{
	return 0;
}

int
setuid(uid_t uid)
{
	return 0;
}

int
setgid(gid_t gid)
{
	return 0;
}

int
seteuid(uid_t uid)
{
	return 0;
}

int
setegid(gid_t gid)
{
	return 0;
}

struct passwd *getpwent(void)
{
	return NULL;
}

void setpwent(void)
{
	return;
}

void
endpwent(void)
{
	return;
}