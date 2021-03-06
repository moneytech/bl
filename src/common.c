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
#include "assembly.h"
#include <time.h>

#ifndef BL_COMPILER_MSVC
#include "unistd.h"
#endif

#ifdef BL_PLATFORM_MACOS
#include <mach-o/dyld.h>
#endif

#ifdef BL_PLATFORM_WIN
#include <windows.h>
#endif

u64 main_thread_id = 0;

bool
get_current_exec_path(char *buf, usize buf_size)
{
#if defined(BL_PLATFORM_WIN)
	return (bool)GetModuleFileNameA(NULL, buf, (DWORD)buf_size);
#elif defined(BL_PLATFORM_LINUX)
	return readlink("/proc/self/exe", buf, buf_size) != -1;
#elif defined(BL_PLATFORM_MACOS)
	return _NSGetExecutablePath(buf, (u32 *)&buf_size) != -1;
#endif
	return false;
}

bool
get_current_exec_dir(char *buf, usize buf_size)
{
	char tmp[PATH_MAX] = {0};
	if (!get_current_exec_path(tmp, PATH_MAX)) return false;
	if (!get_dir_from_filepath(buf, buf_size, tmp)) return false;

	return true;
}

void
id_init(ID *id, const char *str)
{
	BL_ASSERT(id);
	id->hash = thash_from_str(str);
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
brealpath(const char *file, char *out, s32 out_len)
{
	const char *resolved = NULL;
	BL_ASSERT(out);
	BL_ASSERT(out_len);
	if (!file) return resolved;

#if defined(BL_PLATFORM_WIN)
	if (GetFullPathNameA(file, out_len, out, NULL) && file_exists(out)) return &out[0];
	return NULL;
#else
	return realpath(file, out);
#endif
}

void
date_time(char *buf, s32 len, const char *format)
{
	BL_ASSERT(buf && len);
	time_t     timer;
	struct tm *tm_info;

	time(&timer);
	tm_info = localtime(&timer);

	strftime(buf, len, format, tm_info);
}

bool
is_aligned(const void *p, usize alignment)
{
	return (uintptr_t)p % alignment == 0;
}

void
align_ptr_up(void **p, usize alignment, ptrdiff_t *adjustment)
{
	ptrdiff_t adj;
	if (is_aligned(*p, alignment)) {
		if (adjustment) *adjustment = 0;
		return;
	}

	const usize mask = alignment - 1;
	BL_ASSERT((alignment & mask) == 0 && "wrong alignemet"); // pwr of 2
	const uintptr_t i_unaligned  = (uintptr_t)(*p);
	const uintptr_t misalignment = i_unaligned & mask;

	adj = alignment - misalignment;
	*p  = (void *)(i_unaligned + adj);
	if (adjustment) *adjustment = adj;
}

void
print_bits(s32 const size, void const *const ptr)
{
	unsigned char *b = (unsigned char *)ptr;
	unsigned char  byte;
	s32            i, j;

	for (i = size - 1; i >= 0; i--) {
		for (j = 7; j >= 0; j--) {
			byte = (b[i] >> j) & 1;
			printf("%u", byte);
		}
	}
	puts("");
}

int
count_bits(u64 n)
{
	int count = 0;
	while (n) {
		count++;
		n = n >> 1;
	}
	return count;
}

bool
get_dir_from_filepath(char *buf, const usize l, const char *filepath)
{
	if (!filepath) return false;

	char *ptr = strrchr(filepath, PATH_SEPARATORC);
	if (!ptr) return false;
	if (filepath == ptr) {
		strncpy(buf, filepath, strlen(filepath));
		return true;
	}

	usize len = ptr - filepath;
	if (len + 1 > l) BL_ABORT("path too long!!!");
	strncpy(buf, filepath, len);

	return true;
}

bool
get_filename_from_filepath(char *buf, const usize l, const char *filepath)
{
	if (!filepath) return false;

	char *ptr = strrchr(filepath, PATH_SEPARATORC);
	if (!ptr || filepath == ptr) {
		strncpy(buf, filepath, strlen(filepath));
		return true;
	}

	usize len = strlen(filepath) - (ptr - filepath);
	if (len + 1 > l) BL_ABORT("path too long!!!");
	strncpy(buf, ptr + 1, len);

	return true;
}

void
platform_lib_name(const char *name, char *buffer, usize max_len)
{
	if (!name) return;

#ifdef BL_PLATFORM_MACOS
	snprintf(buffer, max_len, "lib%s.dylib", name);
#elif defined(BL_PLATFORM_LINUX)
	snprintf(buffer, max_len, "lib%s.so", name);
#elif defined(BL_PLATFORM_WIN)
	snprintf(buffer, max_len, "%s.dll", name);
#else
	BL_ABORT("Unknown dynamic library format.");
#endif
}

TArray *
create_arr(Assembly *assembly, usize size)
{
	TArray **tmp = arena_alloc(&assembly->arenas.array);
	*tmp         = tarray_new(size);
	return *tmp;
}

void *
_create_sarr(Assembly *assembly, usize arr_size)
{
	BL_ASSERT(arr_size <= assembly->arenas.small_array.elem_size_in_bytes &&
	          "SmallArray is too big to be allocated inside arena, make array smaller or arena "
	          "bigger.");

	TSmallArrayAny *tmp = arena_alloc(&assembly->arenas.small_array);
	tsa_init(tmp);
	return tmp;
}

u32
next_pow_2(u32 n)
{
	u32 p = 1;
	if (n && !(n & (n - 1))) return n;

	while (p < n)
		p <<= 1;

	return p;
}
