/**
 * WinPR: Windows Portable Runtime
 * File Functions
 *
 * Copyright 2012 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 * Copyright 2014 Hewlett-Packard Development Company, L.P.
 * Copyright 2016 David PHAM-VAN <d.phamvan@inuvika.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <winpr/config.h>

#include <winpr/crt.h>
#include <winpr/wlog.h>
#include <winpr/string.h>
#include <winpr/path.h>
#include <winpr/file.h>

#ifdef WINPR_HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef WINPR_HAVE_FCNTL_H
#include <fcntl.h>
#endif

#include "../log.h"
#define TAG WINPR_TAG("file")

#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#else
#include <winpr/assert.h>
#include <pthread.h>
#include <dirent.h>
#include <libgen.h>
#include <errno.h>

#include <sys/un.h>
#include <sys/stat.h>
#include <sys/socket.h>

#ifdef WINPR_HAVE_AIO_H
#undef WINPR_HAVE_AIO_H /* disable for now, incomplete */
#endif

#ifdef WINPR_HAVE_AIO_H
#include <aio.h>
#endif

#ifdef ANDROID
#include <sys/vfs.h>
#else
#include <sys/statvfs.h>
#endif

#include "../handle/handle.h"

#include "../pipe/pipe.h"

#include "file.h"

/**
 * api-ms-win-core-file-l1-2-0.dll:
 *
 * CreateFileA
 * CreateFileW
 * CreateFile2
 * DeleteFileA
 * DeleteFileW
 * CreateDirectoryA
 * CreateDirectoryW
 * RemoveDirectoryA
 * RemoveDirectoryW
 * CompareFileTime
 * DefineDosDeviceW
 * DeleteVolumeMountPointW
 * FileTimeToLocalFileTime
 * LocalFileTimeToFileTime
 * FindClose
 * FindCloseChangeNotification
 * FindFirstChangeNotificationA
 * FindFirstChangeNotificationW
 * FindFirstFileA
 * FindFirstFileExA
 * FindFirstFileExW
 * FindFirstFileW
 * FindFirstVolumeW
 * FindNextChangeNotification
 * FindNextFileA
 * FindNextFileW
 * FindNextVolumeW
 * FindVolumeClose
 * GetDiskFreeSpaceA
 * GetDiskFreeSpaceExA
 * GetDiskFreeSpaceExW
 * GetDiskFreeSpaceW
 * GetDriveTypeA
 * GetDriveTypeW
 * GetFileAttributesA
 * GetFileAttributesExA
 * GetFileAttributesExW
 * GetFileAttributesW
 * GetFileInformationByHandle
 * GetFileSize
 * GetFileSizeEx
 * GetFileTime
 * GetFileType
 * GetFinalPathNameByHandleA
 * GetFinalPathNameByHandleW
 * GetFullPathNameA
 * GetFullPathNameW
 * GetLogicalDrives
 * GetLogicalDriveStringsW
 * GetLongPathNameA
 * GetLongPathNameW
 * GetShortPathNameW
 * GetTempFileNameW
 * GetTempPathW
 * GetVolumeInformationByHandleW
 * GetVolumeInformationW
 * GetVolumeNameForVolumeMountPointW
 * GetVolumePathNamesForVolumeNameW
 * GetVolumePathNameW
 * QueryDosDeviceW
 * SetFileAttributesA
 * SetFileAttributesW
 * SetFileTime
 * SetFileValidData
 * SetFileInformationByHandle
 * ReadFile
 * ReadFileEx
 * ReadFileScatter
 * WriteFile
 * WriteFileEx
 * WriteFileGather
 * FlushFileBuffers
 * SetEndOfFile
 * SetFilePointer
 * SetFilePointerEx
 * LockFile
 * LockFileEx
 * UnlockFile
 * UnlockFileEx
 */

/**
 * File System Behavior in the Microsoft Windows Environment:
 * http://download.microsoft.com/download/4/3/8/43889780-8d45-4b2e-9d3a-c696a890309f/File%20System%20Behavior%20Overview.pdf
 */

/**
 * Asynchronous I/O - The GNU C Library:
 * http://www.gnu.org/software/libc/manual/html_node/Asynchronous-I_002fO.html
 */

/**
 * aio.h - asynchronous input and output:
 * http://pubs.opengroup.org/onlinepubs/009695399/basedefs/aio.h.html
 */

/**
 * Asynchronous I/O User Guide:
 * http://code.google.com/p/kernel/wiki/AIOUserGuide
 */
static wArrayList* HandleCreators;

static pthread_once_t HandleCreatorsInitialized = PTHREAD_ONCE_INIT;

#include "../comm/comm.h"
#include "namedPipeClient.h"

static void HandleCreatorsInit(void)
{
	WINPR_ASSERT(HandleCreators == NULL);
	HandleCreators = ArrayList_New(TRUE);

	if (!HandleCreators)
		return;

	/*
	 * Register all file handle creators.
	 */
	ArrayList_Append(HandleCreators, GetNamedPipeClientHandleCreator());
	const HANDLE_CREATOR* serial = GetCommHandleCreator();
	if (serial)
		ArrayList_Append(HandleCreators, serial);
	ArrayList_Append(HandleCreators, GetFileHandleCreator());
}

#ifdef WINPR_HAVE_AIO_H

static BOOL g_AioSignalHandlerInstalled = FALSE;

void AioSignalHandler(int signum, siginfo_t* siginfo, void* arg)
{
	WLog_INFO("%d", signum);
}

int InstallAioSignalHandler()
{
	if (!g_AioSignalHandlerInstalled)
	{
		struct sigaction action;
		sigemptyset(&action.sa_mask);
		sigaddset(&action.sa_mask, SIGIO);
		action.sa_flags = SA_SIGINFO;
		action.sa_sigaction = (void*)&AioSignalHandler;
		sigaction(SIGIO, &action, NULL);
		g_AioSignalHandlerInstalled = TRUE;
	}

	return 0;
}

#endif /* WINPR_HAVE_AIO_H */

HANDLE CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                   LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                   DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
	if (!lpFileName)
		return INVALID_HANDLE_VALUE;

	if (pthread_once(&HandleCreatorsInitialized, HandleCreatorsInit) != 0)
	{
		SetLastError(ERROR_DLL_INIT_FAILED);
		return INVALID_HANDLE_VALUE;
	}

	if (HandleCreators == NULL)
	{
		SetLastError(ERROR_DLL_INIT_FAILED);
		return INVALID_HANDLE_VALUE;
	}

	ArrayList_Lock(HandleCreators);

	for (size_t i = 0; i <= ArrayList_Count(HandleCreators); i++)
	{
		const HANDLE_CREATOR* creator = ArrayList_GetItem(HandleCreators, i);

		if (creator && creator->IsHandled(lpFileName))
		{
			HANDLE newHandle =
			    creator->CreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
			                         dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
			ArrayList_Unlock(HandleCreators);
			return newHandle;
		}
	}

	ArrayList_Unlock(HandleCreators);
	return INVALID_HANDLE_VALUE;
}

HANDLE CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                   LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                   DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
	HANDLE hdl = NULL;
	if (!lpFileName)
		return NULL;
	char* lpFileNameA = ConvertWCharToUtf8Alloc(lpFileName, NULL);

	if (!lpFileNameA)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		goto fail;
	}

	hdl = CreateFileA(lpFileNameA, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
	                  dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
fail:
	free(lpFileNameA);
	return hdl;
}

BOOL DeleteFileA(LPCSTR lpFileName)
{
	int status = 0;
	status = unlink(lpFileName);
	return (status != -1) ? TRUE : FALSE;
}

BOOL DeleteFileW(LPCWSTR lpFileName)
{
	if (!lpFileName)
		return FALSE;
	LPSTR lpFileNameA = ConvertWCharToUtf8Alloc(lpFileName, NULL);
	BOOL rc = FALSE;

	if (!lpFileNameA)
		goto fail;

	rc = DeleteFileA(lpFileNameA);
fail:
	free(lpFileNameA);
	return rc;
}

BOOL ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
              LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped)
{
	ULONG Type = 0;
	WINPR_HANDLE* handle = NULL;

	if (hFile == INVALID_HANDLE_VALUE)
		return FALSE;

	/*
	 * from http://msdn.microsoft.com/en-us/library/windows/desktop/aa365467%28v=vs.85%29.aspx
	 * lpNumberOfBytesRead can be NULL only when the lpOverlapped parameter is not NULL.
	 */

	if (!lpNumberOfBytesRead && !lpOverlapped)
		return FALSE;

	if (!winpr_Handle_GetInfo(hFile, &Type, &handle))
		return FALSE;

	handle = (WINPR_HANDLE*)hFile;

	if (handle->ops->ReadFile)
		return handle->ops->ReadFile(handle, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead,
		                             lpOverlapped);

	WLog_ERR(TAG, "ReadFile operation not implemented");
	return FALSE;
}

BOOL ReadFileEx(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
                LPOVERLAPPED lpOverlapped, LPOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
	ULONG Type = 0;
	WINPR_HANDLE* handle = NULL;

	if (hFile == INVALID_HANDLE_VALUE)
		return FALSE;

	if (!winpr_Handle_GetInfo(hFile, &Type, &handle))
		return FALSE;

	handle = (WINPR_HANDLE*)hFile;

	if (handle->ops->ReadFileEx)
		return handle->ops->ReadFileEx(handle, lpBuffer, nNumberOfBytesToRead, lpOverlapped,
		                               lpCompletionRoutine);

	WLog_ERR(TAG, "ReadFileEx operation not implemented");
	return FALSE;
}

BOOL ReadFileScatter(HANDLE hFile, FILE_SEGMENT_ELEMENT aSegmentArray[], DWORD nNumberOfBytesToRead,
                     LPDWORD lpReserved, LPOVERLAPPED lpOverlapped)
{
	ULONG Type = 0;
	WINPR_HANDLE* handle = NULL;

	if (hFile == INVALID_HANDLE_VALUE)
		return FALSE;

	if (!winpr_Handle_GetInfo(hFile, &Type, &handle))
		return FALSE;

	handle = (WINPR_HANDLE*)hFile;

	if (handle->ops->ReadFileScatter)
		return handle->ops->ReadFileScatter(handle, aSegmentArray, nNumberOfBytesToRead, lpReserved,
		                                    lpOverlapped);

	WLog_ERR(TAG, "ReadFileScatter operation not implemented");
	return FALSE;
}

BOOL WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
               LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped)
{
	ULONG Type = 0;
	WINPR_HANDLE* handle = NULL;

	if (hFile == INVALID_HANDLE_VALUE)
		return FALSE;

	if (!winpr_Handle_GetInfo(hFile, &Type, &handle))
		return FALSE;

	handle = (WINPR_HANDLE*)hFile;

	if (handle->ops->WriteFile)
		return handle->ops->WriteFile(handle, lpBuffer, nNumberOfBytesToWrite,
		                              lpNumberOfBytesWritten, lpOverlapped);

	WLog_ERR(TAG, "WriteFile operation not implemented");
	return FALSE;
}

BOOL WriteFileEx(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
                 LPOVERLAPPED lpOverlapped, LPOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
	ULONG Type = 0;
	WINPR_HANDLE* handle = NULL;

	if (hFile == INVALID_HANDLE_VALUE)
		return FALSE;

	if (!winpr_Handle_GetInfo(hFile, &Type, &handle))
		return FALSE;

	handle = (WINPR_HANDLE*)hFile;

	if (handle->ops->WriteFileEx)
		return handle->ops->WriteFileEx(handle, lpBuffer, nNumberOfBytesToWrite, lpOverlapped,
		                                lpCompletionRoutine);

	WLog_ERR(TAG, "WriteFileEx operation not implemented");
	return FALSE;
}

BOOL WriteFileGather(HANDLE hFile, FILE_SEGMENT_ELEMENT aSegmentArray[],
                     DWORD nNumberOfBytesToWrite, LPDWORD lpReserved, LPOVERLAPPED lpOverlapped)
{
	ULONG Type = 0;
	WINPR_HANDLE* handle = NULL;

	if (hFile == INVALID_HANDLE_VALUE)
		return FALSE;

	if (!winpr_Handle_GetInfo(hFile, &Type, &handle))
		return FALSE;

	handle = (WINPR_HANDLE*)hFile;

	if (handle->ops->WriteFileGather)
		return handle->ops->WriteFileGather(handle, aSegmentArray, nNumberOfBytesToWrite,
		                                    lpReserved, lpOverlapped);

	WLog_ERR(TAG, "WriteFileGather operation not implemented");
	return FALSE;
}

BOOL FlushFileBuffers(HANDLE hFile)
{
	ULONG Type = 0;
	WINPR_HANDLE* handle = NULL;

	if (hFile == INVALID_HANDLE_VALUE)
		return FALSE;

	if (!winpr_Handle_GetInfo(hFile, &Type, &handle))
		return FALSE;

	handle = (WINPR_HANDLE*)hFile;

	if (handle->ops->FlushFileBuffers)
		return handle->ops->FlushFileBuffers(handle);

	WLog_ERR(TAG, "FlushFileBuffers operation not implemented");
	return FALSE;
}

BOOL WINAPI GetFileAttributesExA(LPCSTR lpFileName,
                                 WINPR_ATTR_UNUSED GET_FILEEX_INFO_LEVELS fInfoLevelId,
                                 LPVOID lpFileInformation)
{
	LPWIN32_FILE_ATTRIBUTE_DATA fd = lpFileInformation;
	WIN32_FIND_DATAA findFileData = { 0 };

	if (!fd)
		return FALSE;

	HANDLE hFind = FindFirstFileA(lpFileName, &findFileData);
	if (hFind == INVALID_HANDLE_VALUE)
		return FALSE;

	FindClose(hFind);
	fd->dwFileAttributes = findFileData.dwFileAttributes;
	fd->ftCreationTime = findFileData.ftCreationTime;
	fd->ftLastAccessTime = findFileData.ftLastAccessTime;
	fd->ftLastWriteTime = findFileData.ftLastWriteTime;
	fd->nFileSizeHigh = findFileData.nFileSizeHigh;
	fd->nFileSizeLow = findFileData.nFileSizeLow;
	return TRUE;
}

BOOL WINAPI GetFileAttributesExW(LPCWSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId,
                                 LPVOID lpFileInformation)
{
	BOOL ret = 0;
	if (!lpFileName)
		return FALSE;
	LPSTR lpCFileName = ConvertWCharToUtf8Alloc(lpFileName, NULL);

	if (!lpCFileName)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}

	ret = GetFileAttributesExA(lpCFileName, fInfoLevelId, lpFileInformation);
	free(lpCFileName);
	return ret;
}

DWORD WINAPI GetFileAttributesA(LPCSTR lpFileName)
{
	WIN32_FIND_DATAA findFileData = { 0 };
	HANDLE hFind = FindFirstFileA(lpFileName, &findFileData);

	if (hFind == INVALID_HANDLE_VALUE)
		return INVALID_FILE_ATTRIBUTES;

	FindClose(hFind);
	return findFileData.dwFileAttributes;
}

DWORD WINAPI GetFileAttributesW(LPCWSTR lpFileName)
{
	DWORD ret = 0;
	if (!lpFileName)
		return FALSE;
	LPSTR lpCFileName = ConvertWCharToUtf8Alloc(lpFileName, NULL);
	if (!lpCFileName)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}

	ret = GetFileAttributesA(lpCFileName);
	free(lpCFileName);
	return ret;
}

BOOL GetFileInformationByHandle(HANDLE hFile, LPBY_HANDLE_FILE_INFORMATION lpFileInformation)
{
	ULONG Type = 0;
	WINPR_HANDLE* handle = NULL;

	if (hFile == INVALID_HANDLE_VALUE)
		return FALSE;

	if (!winpr_Handle_GetInfo(hFile, &Type, &handle))
		return FALSE;

	handle = (WINPR_HANDLE*)hFile;

	if (handle->ops->GetFileInformationByHandle)
		return handle->ops->GetFileInformationByHandle(handle, lpFileInformation);

	WLog_ERR(TAG, "GetFileInformationByHandle operation not implemented");
	return 0;
}

static char* append(char* buffer, size_t size, const char* append)
{
	winpr_str_append(append, buffer, size, "|");
	return buffer;
}

static const char* flagsToStr(char* buffer, size_t size, DWORD flags)
{
	char strflags[32] = { 0 };
	if (flags & FILE_ATTRIBUTE_READONLY)
		append(buffer, size, "FILE_ATTRIBUTE_READONLY");
	if (flags & FILE_ATTRIBUTE_HIDDEN)
		append(buffer, size, "FILE_ATTRIBUTE_HIDDEN");
	if (flags & FILE_ATTRIBUTE_SYSTEM)
		append(buffer, size, "FILE_ATTRIBUTE_SYSTEM");
	if (flags & FILE_ATTRIBUTE_DIRECTORY)
		append(buffer, size, "FILE_ATTRIBUTE_DIRECTORY");
	if (flags & FILE_ATTRIBUTE_ARCHIVE)
		append(buffer, size, "FILE_ATTRIBUTE_ARCHIVE");
	if (flags & FILE_ATTRIBUTE_DEVICE)
		append(buffer, size, "FILE_ATTRIBUTE_DEVICE");
	if (flags & FILE_ATTRIBUTE_NORMAL)
		append(buffer, size, "FILE_ATTRIBUTE_NORMAL");
	if (flags & FILE_ATTRIBUTE_TEMPORARY)
		append(buffer, size, "FILE_ATTRIBUTE_TEMPORARY");
	if (flags & FILE_ATTRIBUTE_SPARSE_FILE)
		append(buffer, size, "FILE_ATTRIBUTE_SPARSE_FILE");
	if (flags & FILE_ATTRIBUTE_REPARSE_POINT)
		append(buffer, size, "FILE_ATTRIBUTE_REPARSE_POINT");
	if (flags & FILE_ATTRIBUTE_COMPRESSED)
		append(buffer, size, "FILE_ATTRIBUTE_COMPRESSED");
	if (flags & FILE_ATTRIBUTE_OFFLINE)
		append(buffer, size, "FILE_ATTRIBUTE_OFFLINE");
	if (flags & FILE_ATTRIBUTE_NOT_CONTENT_INDEXED)
		append(buffer, size, "FILE_ATTRIBUTE_NOT_CONTENT_INDEXED");
	if (flags & FILE_ATTRIBUTE_ENCRYPTED)
		append(buffer, size, "FILE_ATTRIBUTE_ENCRYPTED");
	if (flags & FILE_ATTRIBUTE_VIRTUAL)
		append(buffer, size, "FILE_ATTRIBUTE_VIRTUAL");

	(void)_snprintf(strflags, sizeof(strflags), " [0x%08" PRIx32 "]", flags);
	winpr_str_append(strflags, buffer, size, NULL);
	return buffer;
}

BOOL SetFileAttributesA(LPCSTR lpFileName, DWORD dwFileAttributes)
{
	BOOL rc = FALSE;
#ifdef WINPR_HAVE_FCNTL_H
	const uint32_t mask = ~(FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_NORMAL);
	if (dwFileAttributes & mask)
	{
		char buffer[8192] = { 0 };
		const char* flags = flagsToStr(buffer, sizeof(buffer), dwFileAttributes & mask);
		WLog_WARN(TAG, "Unsupported flags %s, ignoring!", flags);
	}

	int fd = open(lpFileName, O_RDONLY);
	if (fd < 0)
		return FALSE;

	struct stat st = { 0 };
	if (fstat(fd, &st) != 0)
		goto fail;

	if (dwFileAttributes & FILE_ATTRIBUTE_READONLY)
	{
		st.st_mode &= WINPR_ASSERTING_INT_CAST(mode_t, (mode_t)(~(S_IWUSR | S_IWGRP | S_IWOTH)));
	}
	else
	{
		st.st_mode |= S_IWUSR;
	}

	if (fchmod(fd, st.st_mode) != 0)
		goto fail;

	rc = TRUE;
fail:
	close(fd);
#endif
	return rc;
}

BOOL SetFileAttributesW(LPCWSTR lpFileName, DWORD dwFileAttributes)
{
	BOOL ret = 0;

	if (!lpFileName)
		return FALSE;

	char* lpCFileName = ConvertWCharToUtf8Alloc(lpFileName, NULL);
	if (!lpCFileName)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}

	ret = SetFileAttributesA(lpCFileName, dwFileAttributes);
	free(lpCFileName);
	return ret;
}

BOOL SetEndOfFile(HANDLE hFile)
{
	ULONG Type = 0;
	WINPR_HANDLE* handle = NULL;

	if (hFile == INVALID_HANDLE_VALUE)
		return FALSE;

	if (!winpr_Handle_GetInfo(hFile, &Type, &handle))
		return FALSE;

	handle = (WINPR_HANDLE*)hFile;

	if (handle->ops->SetEndOfFile)
		return handle->ops->SetEndOfFile(handle);

	WLog_ERR(TAG, "SetEndOfFile operation not implemented");
	return FALSE;
}

DWORD WINAPI GetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh)
{
	ULONG Type = 0;
	WINPR_HANDLE* handle = NULL;

	if (hFile == INVALID_HANDLE_VALUE)
		return FALSE;

	if (!winpr_Handle_GetInfo(hFile, &Type, &handle))
		return FALSE;

	handle = (WINPR_HANDLE*)hFile;

	if (handle->ops->GetFileSize)
		return handle->ops->GetFileSize(handle, lpFileSizeHigh);

	WLog_ERR(TAG, "GetFileSize operation not implemented");
	return 0;
}

DWORD SetFilePointer(HANDLE hFile, LONG lDistanceToMove, PLONG lpDistanceToMoveHigh,
                     DWORD dwMoveMethod)
{
	ULONG Type = 0;
	WINPR_HANDLE* handle = NULL;

	if (hFile == INVALID_HANDLE_VALUE)
		return FALSE;

	if (!winpr_Handle_GetInfo(hFile, &Type, &handle))
		return FALSE;

	handle = (WINPR_HANDLE*)hFile;

	if (handle->ops->SetFilePointer)
		return handle->ops->SetFilePointer(handle, lDistanceToMove, lpDistanceToMoveHigh,
		                                   dwMoveMethod);

	WLog_ERR(TAG, "SetFilePointer operation not implemented");
	return 0;
}

BOOL SetFilePointerEx(HANDLE hFile, LARGE_INTEGER liDistanceToMove, PLARGE_INTEGER lpNewFilePointer,
                      DWORD dwMoveMethod)
{
	ULONG Type = 0;
	WINPR_HANDLE* handle = NULL;

	if (hFile == INVALID_HANDLE_VALUE)
		return FALSE;

	if (!winpr_Handle_GetInfo(hFile, &Type, &handle))
		return FALSE;

	handle = (WINPR_HANDLE*)hFile;

	if (handle->ops->SetFilePointerEx)
		return handle->ops->SetFilePointerEx(handle, liDistanceToMove, lpNewFilePointer,
		                                     dwMoveMethod);

	WLog_ERR(TAG, "SetFilePointerEx operation not implemented");
	return 0;
}

BOOL LockFile(HANDLE hFile, DWORD dwFileOffsetLow, DWORD dwFileOffsetHigh,
              DWORD nNumberOfBytesToLockLow, DWORD nNumberOfBytesToLockHigh)
{
	ULONG Type = 0;
	WINPR_HANDLE* handle = NULL;

	if (hFile == INVALID_HANDLE_VALUE)
		return FALSE;

	if (!winpr_Handle_GetInfo(hFile, &Type, &handle))
		return FALSE;

	handle = (WINPR_HANDLE*)hFile;

	if (handle->ops->LockFile)
		return handle->ops->LockFile(handle, dwFileOffsetLow, dwFileOffsetHigh,
		                             nNumberOfBytesToLockLow, nNumberOfBytesToLockHigh);

	WLog_ERR(TAG, "LockFile operation not implemented");
	return FALSE;
}

BOOL LockFileEx(HANDLE hFile, DWORD dwFlags, DWORD dwReserved, DWORD nNumberOfBytesToLockLow,
                DWORD nNumberOfBytesToLockHigh, LPOVERLAPPED lpOverlapped)
{
	ULONG Type = 0;
	WINPR_HANDLE* handle = NULL;

	if (hFile == INVALID_HANDLE_VALUE)
		return FALSE;

	if (!winpr_Handle_GetInfo(hFile, &Type, &handle))
		return FALSE;

	handle = (WINPR_HANDLE*)hFile;

	if (handle->ops->LockFileEx)
		return handle->ops->LockFileEx(handle, dwFlags, dwReserved, nNumberOfBytesToLockLow,
		                               nNumberOfBytesToLockHigh, lpOverlapped);

	WLog_ERR(TAG, "LockFileEx operation not implemented");
	return FALSE;
}

BOOL UnlockFile(HANDLE hFile, DWORD dwFileOffsetLow, DWORD dwFileOffsetHigh,
                DWORD nNumberOfBytesToUnlockLow, DWORD nNumberOfBytesToUnlockHigh)
{
	ULONG Type = 0;
	WINPR_HANDLE* handle = NULL;

	if (hFile == INVALID_HANDLE_VALUE)
		return FALSE;

	if (!winpr_Handle_GetInfo(hFile, &Type, &handle))
		return FALSE;

	handle = (WINPR_HANDLE*)hFile;

	if (handle->ops->UnlockFile)
		return handle->ops->UnlockFile(handle, dwFileOffsetLow, dwFileOffsetHigh,
		                               nNumberOfBytesToUnlockLow, nNumberOfBytesToUnlockHigh);

	WLog_ERR(TAG, "UnLockFile operation not implemented");
	return FALSE;
}

BOOL UnlockFileEx(HANDLE hFile, DWORD dwReserved, DWORD nNumberOfBytesToUnlockLow,
                  DWORD nNumberOfBytesToUnlockHigh, LPOVERLAPPED lpOverlapped)
{
	ULONG Type = 0;
	WINPR_HANDLE* handle = NULL;

	if (hFile == INVALID_HANDLE_VALUE)
		return FALSE;

	if (!winpr_Handle_GetInfo(hFile, &Type, &handle))
		return FALSE;

	handle = (WINPR_HANDLE*)hFile;

	if (handle->ops->UnlockFileEx)
		return handle->ops->UnlockFileEx(handle, dwReserved, nNumberOfBytesToUnlockLow,
		                                 nNumberOfBytesToUnlockHigh, lpOverlapped);

	WLog_ERR(TAG, "UnLockFileEx operation not implemented");
	return FALSE;
}

BOOL WINAPI SetFileTime(HANDLE hFile, const FILETIME* lpCreationTime,
                        const FILETIME* lpLastAccessTime, const FILETIME* lpLastWriteTime)
{
	ULONG Type = 0;
	WINPR_HANDLE* handle = NULL;

	if (hFile == INVALID_HANDLE_VALUE)
		return FALSE;

	if (!winpr_Handle_GetInfo(hFile, &Type, &handle))
		return FALSE;

	handle = (WINPR_HANDLE*)hFile;

	if (handle->ops->SetFileTime)
		return handle->ops->SetFileTime(handle, lpCreationTime, lpLastAccessTime, lpLastWriteTime);

	WLog_ERR(TAG, "operation not implemented");
	return FALSE;
}

typedef struct
{
	char magic[16];
	LPSTR lpPath;
	LPSTR lpPattern;
	DIR* pDir;
} WIN32_FILE_SEARCH;

static const char file_search_magic[] = "file_srch_magic";

WINPR_ATTR_MALLOC(FindClose, 1)
static WIN32_FILE_SEARCH* file_search_new(const char* name, size_t namelen, const char* pattern,
                                          size_t patternlen)
{
	WIN32_FILE_SEARCH* pFileSearch = (WIN32_FILE_SEARCH*)calloc(1, sizeof(WIN32_FILE_SEARCH));
	if (!pFileSearch)
		return NULL;
	WINPR_ASSERT(sizeof(file_search_magic) == sizeof(pFileSearch->magic));
	memcpy(pFileSearch->magic, file_search_magic, sizeof(pFileSearch->magic));

	pFileSearch->lpPath = strndup(name, namelen);
	pFileSearch->lpPattern = strndup(pattern, patternlen);
	if (!pFileSearch->lpPath || !pFileSearch->lpPattern)
		goto fail;

	pFileSearch->pDir = opendir(pFileSearch->lpPath);
	if (!pFileSearch->pDir)
	{
		/* Work around for android:
		 * parent directories are not accessible, so if we have a directory without pattern
		 * try to open it directly and set pattern to '*'
		 */
		struct stat fileStat = { 0 };
		if (stat(name, &fileStat) == 0)
		{
			if (S_ISDIR(fileStat.st_mode))
			{
				pFileSearch->pDir = opendir(name);
				if (pFileSearch->pDir)
				{
					free(pFileSearch->lpPath);
					free(pFileSearch->lpPattern);
					pFileSearch->lpPath = _strdup(name);
					pFileSearch->lpPattern = _strdup("*");
					if (!pFileSearch->lpPath || !pFileSearch->lpPattern)
					{
						closedir(pFileSearch->pDir);
						pFileSearch->pDir = NULL;
					}
				}
			}
		}
	}
	if (!pFileSearch->pDir)
		goto fail;

	return pFileSearch;
fail:
	WINPR_PRAGMA_DIAG_PUSH
	WINPR_PRAGMA_DIAG_IGNORED_MISMATCHED_DEALLOC
	FindClose(pFileSearch);
	WINPR_PRAGMA_DIAG_POP
	return NULL;
}

static BOOL is_valid_file_search_handle(HANDLE handle)
{
	WIN32_FILE_SEARCH* pFileSearch = (WIN32_FILE_SEARCH*)handle;
	if (!pFileSearch)
		return FALSE;
	if (pFileSearch == INVALID_HANDLE_VALUE)
		return FALSE;
	if (strncmp(file_search_magic, pFileSearch->magic, sizeof(file_search_magic)) != 0)
		return FALSE;
	return TRUE;
}
static BOOL FindDataFromStat(const char* path, const struct stat* fileStat,
                             LPWIN32_FIND_DATAA lpFindFileData)
{
	UINT64 ft = 0;
	char* lastSep = NULL;
	lpFindFileData->dwFileAttributes = 0;

	if (S_ISDIR(fileStat->st_mode))
		lpFindFileData->dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;

	if (lpFindFileData->dwFileAttributes == 0)
		lpFindFileData->dwFileAttributes = FILE_ATTRIBUTE_ARCHIVE;

	lastSep = strrchr(path, '/');

	if (lastSep)
	{
		const char* name = lastSep + 1;
		const size_t namelen = strlen(name);

		if ((namelen > 1) && (name[0] == '.') && (name[1] != '.'))
			lpFindFileData->dwFileAttributes |= FILE_ATTRIBUTE_HIDDEN;
	}

	if (!(fileStat->st_mode & S_IWUSR))
		lpFindFileData->dwFileAttributes |= FILE_ATTRIBUTE_READONLY;

#ifdef _DARWIN_FEATURE_64_BIT_INODE
	ft = STAT_TIME_TO_FILETIME(fileStat->st_birthtime);
#else
	ft = STAT_TIME_TO_FILETIME(fileStat->st_ctime);
#endif
	lpFindFileData->ftCreationTime.dwHighDateTime = (ft) >> 32ULL;
	lpFindFileData->ftCreationTime.dwLowDateTime = ft & 0xFFFFFFFF;
	ft = STAT_TIME_TO_FILETIME(fileStat->st_mtime);
	lpFindFileData->ftLastWriteTime.dwHighDateTime = (ft) >> 32ULL;
	lpFindFileData->ftLastWriteTime.dwLowDateTime = ft & 0xFFFFFFFF;
	ft = STAT_TIME_TO_FILETIME(fileStat->st_atime);
	lpFindFileData->ftLastAccessTime.dwHighDateTime = (ft) >> 32ULL;
	lpFindFileData->ftLastAccessTime.dwLowDateTime = ft & 0xFFFFFFFF;
	lpFindFileData->nFileSizeHigh = ((UINT64)fileStat->st_size) >> 32ULL;
	lpFindFileData->nFileSizeLow = fileStat->st_size & 0xFFFFFFFF;
	return TRUE;
}

HANDLE FindFirstFileA(LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData)
{
	if (!lpFindFileData || !lpFileName)
	{
		SetLastError(ERROR_BAD_ARGUMENTS);
		return INVALID_HANDLE_VALUE;
	}

	const WIN32_FIND_DATAA empty = { 0 };
	*lpFindFileData = empty;

	WIN32_FILE_SEARCH* pFileSearch = NULL;
	size_t patternlen = 0;
	const size_t flen = strlen(lpFileName);
	const char sep = PathGetSeparatorA(PATH_STYLE_NATIVE);
	const char* ptr = strrchr(lpFileName, sep);
	if (!ptr)
		goto fail;
	patternlen = strlen(ptr + 1);
	if (patternlen == 0)
		goto fail;

	pFileSearch = file_search_new(lpFileName, flen - patternlen, ptr + 1, patternlen);

	if (!pFileSearch)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return INVALID_HANDLE_VALUE;
	}

	if (FindNextFileA((HANDLE)pFileSearch, lpFindFileData))
		return (HANDLE)pFileSearch;

fail:
	FindClose(pFileSearch);
	return INVALID_HANDLE_VALUE;
}

static BOOL ConvertFindDataAToW(LPWIN32_FIND_DATAA lpFindFileDataA,
                                LPWIN32_FIND_DATAW lpFindFileDataW)
{
	if (!lpFindFileDataA || !lpFindFileDataW)
		return FALSE;

	lpFindFileDataW->dwFileAttributes = lpFindFileDataA->dwFileAttributes;
	lpFindFileDataW->ftCreationTime = lpFindFileDataA->ftCreationTime;
	lpFindFileDataW->ftLastAccessTime = lpFindFileDataA->ftLastAccessTime;
	lpFindFileDataW->ftLastWriteTime = lpFindFileDataA->ftLastWriteTime;
	lpFindFileDataW->nFileSizeHigh = lpFindFileDataA->nFileSizeHigh;
	lpFindFileDataW->nFileSizeLow = lpFindFileDataA->nFileSizeLow;
	lpFindFileDataW->dwReserved0 = lpFindFileDataA->dwReserved0;
	lpFindFileDataW->dwReserved1 = lpFindFileDataA->dwReserved1;

	if (ConvertUtf8NToWChar(lpFindFileDataA->cFileName, ARRAYSIZE(lpFindFileDataA->cFileName),
	                        lpFindFileDataW->cFileName, ARRAYSIZE(lpFindFileDataW->cFileName)) < 0)
		return FALSE;

	return ConvertUtf8NToWChar(lpFindFileDataA->cAlternateFileName,
	                           ARRAYSIZE(lpFindFileDataA->cAlternateFileName),
	                           lpFindFileDataW->cAlternateFileName,
	                           ARRAYSIZE(lpFindFileDataW->cAlternateFileName)) >= 0;
}

HANDLE FindFirstFileW(LPCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData)
{
	LPSTR utfFileName = NULL;
	HANDLE h = NULL;
	if (!lpFileName)
		return INVALID_HANDLE_VALUE;

	LPWIN32_FIND_DATAA fd = (LPWIN32_FIND_DATAA)calloc(1, sizeof(WIN32_FIND_DATAA));

	if (!fd)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return INVALID_HANDLE_VALUE;
	}

	utfFileName = ConvertWCharToUtf8Alloc(lpFileName, NULL);
	if (!utfFileName)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		free(fd);
		return INVALID_HANDLE_VALUE;
	}

	h = FindFirstFileA(utfFileName, fd);
	free(utfFileName);

	if (h != INVALID_HANDLE_VALUE)
	{
		if (!ConvertFindDataAToW(fd, lpFindFileData))
		{
			SetLastError(ERROR_NOT_ENOUGH_MEMORY);
			FindClose(h);
			h = INVALID_HANDLE_VALUE;
			goto out;
		}
	}

out:
	free(fd);
	return h;
}

HANDLE FindFirstFileExA(WINPR_ATTR_UNUSED LPCSTR lpFileName,
                        WINPR_ATTR_UNUSED FINDEX_INFO_LEVELS fInfoLevelId,
                        WINPR_ATTR_UNUSED LPVOID lpFindFileData,
                        WINPR_ATTR_UNUSED FINDEX_SEARCH_OPS fSearchOp,
                        WINPR_ATTR_UNUSED LPVOID lpSearchFilter,
                        WINPR_ATTR_UNUSED DWORD dwAdditionalFlags)
{
	WLog_ERR("TODO", "TODO: Implement");
	return INVALID_HANDLE_VALUE;
}

HANDLE FindFirstFileExW(WINPR_ATTR_UNUSED LPCWSTR lpFileName,
                        WINPR_ATTR_UNUSED FINDEX_INFO_LEVELS fInfoLevelId,
                        WINPR_ATTR_UNUSED LPVOID lpFindFileData,
                        WINPR_ATTR_UNUSED FINDEX_SEARCH_OPS fSearchOp,
                        WINPR_ATTR_UNUSED LPVOID lpSearchFilter,
                        WINPR_ATTR_UNUSED DWORD dwAdditionalFlags)
{
	WLog_ERR("TODO", "TODO: Implement");
	return INVALID_HANDLE_VALUE;
}

BOOL FindNextFileA(HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData)
{
	if (!lpFindFileData)
		return FALSE;

	const WIN32_FIND_DATAA empty = { 0 };
	*lpFindFileData = empty;

	if (!is_valid_file_search_handle(hFindFile))
		return FALSE;

	WIN32_FILE_SEARCH* pFileSearch = (WIN32_FILE_SEARCH*)hFindFile;
	struct dirent* pDirent = NULL;
	// NOLINTNEXTLINE(concurrency-mt-unsafe)
	while ((pDirent = readdir(pFileSearch->pDir)) != NULL)
	{
		if (FilePatternMatchA(pDirent->d_name, pFileSearch->lpPattern))
		{
			BOOL success = FALSE;

			strncpy(lpFindFileData->cFileName, pDirent->d_name, MAX_PATH);
			const size_t namelen = strnlen(lpFindFileData->cFileName, MAX_PATH);
			size_t pathlen = strlen(pFileSearch->lpPath);
			char* fullpath = (char*)malloc(pathlen + namelen + 2);

			if (fullpath == NULL)
			{
				SetLastError(ERROR_NOT_ENOUGH_MEMORY);
				return FALSE;
			}

			memcpy(fullpath, pFileSearch->lpPath, pathlen);
			/* Ensure path is terminated with a separator, but prevent
			 * duplicate separators */
			if (fullpath[pathlen - 1] != '/')
				fullpath[pathlen++] = '/';
			memcpy(fullpath + pathlen, pDirent->d_name, namelen);
			fullpath[pathlen + namelen] = 0;

			struct stat fileStat = { 0 };
			if (stat(fullpath, &fileStat) != 0)
			{
				free(fullpath);
				SetLastError(map_posix_err(errno));
				errno = 0;
				continue;
			}

			/* Skip FIFO entries. */
			if (S_ISFIFO(fileStat.st_mode))
			{
				free(fullpath);
				continue;
			}

			success = FindDataFromStat(fullpath, &fileStat, lpFindFileData);
			free(fullpath);
			return success;
		}
	}

	SetLastError(ERROR_NO_MORE_FILES);
	return FALSE;
}

BOOL FindNextFileW(HANDLE hFindFile, LPWIN32_FIND_DATAW lpFindFileData)
{
	LPWIN32_FIND_DATAA fd = (LPWIN32_FIND_DATAA)calloc(1, sizeof(WIN32_FIND_DATAA));

	if (!fd)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}

	if (FindNextFileA(hFindFile, fd))
	{
		if (!ConvertFindDataAToW(fd, lpFindFileData))
		{
			SetLastError(ERROR_NOT_ENOUGH_MEMORY);
			free(fd);
			return FALSE;
		}

		free(fd);
		return TRUE;
	}

	free(fd);
	return FALSE;
}

BOOL FindClose(HANDLE hFindFile)
{
	WIN32_FILE_SEARCH* pFileSearch = (WIN32_FILE_SEARCH*)hFindFile;
	if (!pFileSearch)
		return FALSE;

		/* Since INVALID_HANDLE_VALUE != NULL the analyzer guesses that there
		 * is a initialized HANDLE that is not freed properly.
		 * Disable this return to stop confusing the analyzer. */
#ifndef __clang_analyzer__
	if (!is_valid_file_search_handle(hFindFile))
		return FALSE;
#endif

	free(pFileSearch->lpPath);
	free(pFileSearch->lpPattern);

	if (pFileSearch->pDir)
		closedir(pFileSearch->pDir);

	// NOLINTNEXTLINE(clang-analyzer-unix.Malloc)
	free(pFileSearch);
	return TRUE;
}

BOOL CreateDirectoryA(LPCSTR lpPathName,
                      WINPR_ATTR_UNUSED LPSECURITY_ATTRIBUTES lpSecurityAttributes)
{
	if (!mkdir(lpPathName, S_IRUSR | S_IWUSR | S_IXUSR))
		return TRUE;

	return FALSE;
}

BOOL CreateDirectoryW(LPCWSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes)
{
	if (!lpPathName)
		return FALSE;
	char* utfPathName = ConvertWCharToUtf8Alloc(lpPathName, NULL);
	BOOL ret = FALSE;

	if (!utfPathName)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		goto fail;
	}

	ret = CreateDirectoryA(utfPathName, lpSecurityAttributes);
fail:
	free(utfPathName);
	return ret;
}

BOOL RemoveDirectoryA(LPCSTR lpPathName)
{
	int ret = rmdir(lpPathName);

	if (ret != 0)
		SetLastError(map_posix_err(errno));
	else
		SetLastError(STATUS_SUCCESS);

	return ret == 0;
}

BOOL RemoveDirectoryW(LPCWSTR lpPathName)
{
	if (!lpPathName)
		return FALSE;
	char* utfPathName = ConvertWCharToUtf8Alloc(lpPathName, NULL);
	BOOL ret = FALSE;

	if (!utfPathName)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		goto fail;
	}

	ret = RemoveDirectoryA(utfPathName);
fail:
	free(utfPathName);
	return ret;
}

BOOL MoveFileExA(LPCSTR lpExistingFileName, LPCSTR lpNewFileName, DWORD dwFlags)
{
	struct stat st;
	int ret = 0;
	ret = stat(lpNewFileName, &st);

	if ((dwFlags & MOVEFILE_REPLACE_EXISTING) == 0)
	{
		if (ret == 0)
		{
			SetLastError(ERROR_ALREADY_EXISTS);
			return FALSE;
		}
	}
	else
	{
		if (ret == 0 && (st.st_mode & S_IWUSR) == 0)
		{
			SetLastError(ERROR_ACCESS_DENIED);
			return FALSE;
		}
	}

	ret = rename(lpExistingFileName, lpNewFileName);

	if (ret != 0)
		SetLastError(map_posix_err(errno));

	return ret == 0;
}

BOOL MoveFileExW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName, DWORD dwFlags)
{
	if (!lpExistingFileName || !lpNewFileName)
		return FALSE;

	LPSTR lpCExistingFileName = ConvertWCharToUtf8Alloc(lpExistingFileName, NULL);
	LPSTR lpCNewFileName = ConvertWCharToUtf8Alloc(lpNewFileName, NULL);
	BOOL ret = FALSE;

	if (!lpCExistingFileName || !lpCNewFileName)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		goto fail;
	}

	ret = MoveFileExA(lpCExistingFileName, lpCNewFileName, dwFlags);
fail:
	free(lpCNewFileName);
	free(lpCExistingFileName);
	return ret;
}

BOOL MoveFileA(LPCSTR lpExistingFileName, LPCSTR lpNewFileName)
{
	return MoveFileExA(lpExistingFileName, lpNewFileName, 0);
}

BOOL MoveFileW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName)
{
	return MoveFileExW(lpExistingFileName, lpNewFileName, 0);
}

#endif

/* Extended API */

int UnixChangeFileMode(const char* filename, int flags)
{
	if (!filename)
		return -1;
#ifndef _WIN32
	mode_t fl = 0;
	fl |= (flags & 0x4000) ? S_ISUID : 0;
	fl |= (flags & 0x2000) ? S_ISGID : 0;
	fl |= (flags & 0x1000) ? S_ISVTX : 0;
	fl |= (flags & 0x0400) ? S_IRUSR : 0;
	fl |= (flags & 0x0200) ? S_IWUSR : 0;
	fl |= (flags & 0x0100) ? S_IXUSR : 0;
	fl |= (flags & 0x0040) ? S_IRGRP : 0;
	fl |= (flags & 0x0020) ? S_IWGRP : 0;
	fl |= (flags & 0x0010) ? S_IXGRP : 0;
	fl |= (flags & 0x0004) ? S_IROTH : 0;
	fl |= (flags & 0x0002) ? S_IWOTH : 0;
	fl |= (flags & 0x0001) ? S_IXOTH : 0;
	return chmod(filename, fl);
#else
	int rc;
	WCHAR* wfl = ConvertUtf8ToWCharAlloc(filename, NULL);

	if (!wfl)
		return -1;

	/* Check for unsupported flags. */
	if (flags & ~(_S_IREAD | _S_IWRITE))
		WLog_WARN(TAG, "Unsupported file mode %d for _wchmod", flags);

	rc = _wchmod(wfl, flags);
	free(wfl);
	return rc;
#endif
}
