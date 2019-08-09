//************************************************************************************************
// bl
//
// File:   common.c
// Author: Martin Dorazil
// Date:   11.8.18
//
// Copyright 2018 Martin Dorazil
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//************************************************************************************************

#include "common.h"
#include <bobject/containers/hash.h>
#include <time.h>

#ifndef BL_COMPILER_MSVC
#include "unistd.h"
#endif

#ifdef BL_PLATFORM_MACOS
#include <mach-o/dyld.h>
#endif

bool
get_current_exec_path(char *buf, size_t buf_size)
{
#if defined(BL_PLATFORM_WIN)
	return (bool)GetModuleFileNameA(NULL, buf, (DWORD) buf_size);
#elif defined(BL_PLATFORM_LINUX)
	return readlink("/proc/self/exe", buf, buf_size) != -1;
#elif defined(BL_PLATFORM_MACOS)
	return _NSGetExecutablePath(buf, (uint32_t *)&buf_size) != -1;
#endif
	return false;
}

bool
get_current_exec_dir(char *buf, size_t buf_size)
{
	char tmp[PATH_MAX] = {0};
	if (!get_current_exec_path(tmp, PATH_MAX)) return false;
	if (!get_dir_from_filepath(buf, buf_size, tmp)) return false;

	return true;
}

void
id_init(ID *id, const char *str)
{
	assert(id);
	id->hash = bo_hash_from_str(str);
	id->str  = str;
}

bool
file_exists(const char *filepath)
{
#if defined(BL_PLATFORM_WIN)
	return (bool)PathFileExistsA(filepath);
#else
	return access(filepath, F_OK) != -1;
#endif
}

const char *
brealpath(const char *file, char *out, int32_t out_len)
{
	const char *resolved = NULL;
	assert(out);
	assert(out_len);
	if (!file) return resolved;

#if defined(BL_PLATFORM_WIN)
	if (GetFullPathNameA(file, out_len, out, NULL) && file_exists(out)) return &out[0];
	return NULL;
#else
	return realpath(file, out);
#endif
}

void
date_time(char *buf, int32_t len, const char *format)
{
	assert(buf && len);
	time_t     timer;
	struct tm *tm_info;

	time(&timer);
	tm_info = localtime(&timer);

	strftime(buf, len, format, tm_info);
}

bool
is_aligned(const void *p, size_t alignment)
{
	return (uintptr_t)p % alignment == 0;
}

void
align_ptr_up(void **p, size_t alignment, ptrdiff_t *adjustment)
{
	ptrdiff_t adj;
	if (is_aligned(*p, alignment)) {
		if (adjustment) *adjustment = 0;
		return;
	}

	const size_t mask = alignment - 1;
	assert((alignment & mask) == 0 && "wrong alignemet"); // pwr of 2
	const uintptr_t i_unaligned  = (uintptr_t)(*p);
	const uintptr_t misalignment = i_unaligned & mask;

	adj = alignment - misalignment;
	*p  = (void *)(i_unaligned + adj);
	if (adjustment) *adjustment = adj;
}

void
print_bits(int32_t const size, void const *const ptr)
{
	unsigned char *b = (unsigned char *)ptr;
	unsigned char  byte;
	int32_t        i, j;

	for (i = size - 1; i >= 0; i--) {
		for (j = 7; j >= 0; j--) {
			byte = (b[i] >> j) & 1;
			printf("%u", byte);
		}
	}
	puts("");
}

bool
get_dir_from_filepath(char *buf, const size_t l, const char *filepath)
{
	if (!filepath) return false;

	char *ptr = strrchr(filepath, PATH_SEPARATORC);
	if (!ptr) return false;
	if (filepath == ptr) return strdup(filepath);

	size_t len = ptr - filepath;
	if (len + 1 > l) bl_abort("path too long!!!");
	strncpy(buf, filepath, len);

	return true;
}

bool
search_file(const char *filepath, char **out_filepath, char **out_dirpath, const char *wdir)
{
	if (filepath == NULL) goto NOT_FOUND;

	char        tmp[PATH_MAX] = {0};
	const char *rpath         = tmp;

	/* Lookup in working directory. */
	if (wdir) {
		strncpy(tmp, wdir, PATH_MAX);
		strcat(tmp, PATH_SEPARATOR);
		strcat(tmp, filepath);

		if (file_exists(tmp)) {
			goto FOUND;
		}
	}

	rpath = brealpath(filepath, tmp, PATH_MAX);

	if (rpath != NULL) {
		goto FOUND;
	}

	/* file has not been found in current working direcotry -> search in LIB_DIR */
	if (ENV_LIB_DIR) {
		char tmp_lib_dir[PATH_MAX];

		strcpy(tmp_lib_dir, ENV_LIB_DIR);
		strcat(tmp_lib_dir, PATH_SEPARATOR);
		strcat(tmp_lib_dir, filepath);

		rpath = brealpath(tmp_lib_dir, tmp, PATH_MAX);

		if (rpath != NULL) {
			goto FOUND;
		}
	}

	/* file has not been found in current working direcotry -> search in PATH */
	{
		char   tmp_env[PATH_MAX];
		char * env          = strdup(getenv(ENV_PATH));
		char * s            = env;
		char * p            = NULL;
		size_t filepath_len = strlen(filepath);

		do {
			p = strchr(s, ENVPATH_SEPARATOR);
			if (p != NULL) {
				p[0] = 0;
			}

			if (strlen(s) + filepath_len + strlen(PATH_SEPARATOR) >= PATH_MAX)
				bl_abort("path too long");

			strcpy(&tmp_env[0], s);
			strcat(&tmp_env[0], PATH_SEPARATOR);
			strcat(&tmp_env[0], filepath);

			rpath = brealpath(&tmp_env[0], tmp, PATH_MAX);

			s = p + 1;
		} while (p != NULL && rpath == NULL);

		free(env);
		if (rpath) {
			goto FOUND;
		}
	}

NOT_FOUND:
	return false;

FOUND:
	/* Absolute file path. */
	*out_filepath = strdup(rpath);

	/* Absolute directory path. */
	memset(tmp, 0, array_size(tmp));
	if (get_dir_from_filepath(tmp, PATH_MAX, *out_filepath)) {
		*out_dirpath = strdup(tmp);
	}

	return true;
}
