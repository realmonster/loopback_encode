#include "PipeEncoder.h"

#include <cstring>

PipeEncoder::PipeEncoder(const wchar_t *command_line)
	: child_stdin_rd(NULL), child_stdin_wr(NULL),
	child_stdout_rd(NULL), child_stdout_wr(NULL)
{
	memset(&proc_info, 0, sizeof(proc_info));

	command = (wchar_t*)malloc((wcslen(command_line) + 1)*sizeof(wchar_t));
	if (command)
		wcscpy(command, command_line);
}

enum
{
	ERROR_OUT_OF_MEMORY = ENCODER_ERROR_MASK,
	ERROR_CREATE_STDIN_PIPE,
	ERROR_CREATE_STDOUT_PIPE,
	ERROR_SET_INFORMATION_STDIN,
	ERROR_SET_INFORMATION_STDOUT,
	ERROR_CREATE_PROCESS,
	ERROR_WRITE_INTO_PIPE,
	ERROR_WRITE_NOT_FULL,
	ERROR_EXIT_PROCESS,
};

wchar_t* PipeEncoder::GetErrorString(DWORD error)
{
	switch(error)
	{
	case ERROR_OUT_OF_MEMORY:
		return L"Out of memory";
	case ERROR_CREATE_STDIN_PIPE:
		return L"Can't create stdin pipe for child process";
	case ERROR_CREATE_STDOUT_PIPE:
		return L"Can't create stdout pipe for child process";
	case ERROR_SET_INFORMATION_STDIN:
		return L"Can't set information for stdin pipe";
	case ERROR_SET_INFORMATION_STDOUT:
		return L"Can't set information for stdout pipe";
	case ERROR_CREATE_PROCESS:
		return L"Can't start child process";
	case ERROR_WRITE_INTO_PIPE:
		return L"Write attempt into pipe was failed";
	case ERROR_WRITE_NOT_FULL:
		return L"Write into pipe was not complete (partially)";
	case ERROR_EXIT_PROCESS:
		return L"Process exited before end of recording";
	default:
		return NULL;
	}
}

DWORD PipeEncoder::Init(const WAVEFORMATEX *format)
{
	SECURITY_ATTRIBUTES attr;
	HANDLE tmp = NULL;
	DWORD res = 0;
	STARTUPINFO start_info;

	if (!command)
		return ERROR_OUT_OF_MEMORY;

	attr.nLength = sizeof(SECURITY_ATTRIBUTES);
	attr.bInheritHandle = TRUE;
	attr.lpSecurityDescriptor = NULL;

#define ERROR_EXIT(error) if (1) {res = error; goto Exit;} else (void*)0

	if (!CreatePipe(&child_stdin_rd, &child_stdin_wr, &attr, 1<<20))
		ERROR_EXIT(ERROR_CREATE_STDIN_PIPE);

	if (!SetHandleInformation(child_stdin_wr, HANDLE_FLAG_INHERIT, 0))
		ERROR_EXIT(ERROR_SET_INFORMATION_STDIN); 

	if (!CreatePipe(&child_stdout_rd, &child_stdout_wr, &attr, 1<<20))
		ERROR_EXIT(ERROR_CREATE_STDOUT_PIPE);

	if (!SetHandleInformation(child_stdout_rd, HANDLE_FLAG_INHERIT, 0))
		ERROR_EXIT(ERROR_SET_INFORMATION_STDOUT);

	memset(&start_info, 0, sizeof(STARTUPINFO));
	start_info.cb = sizeof(STARTUPINFO);
	start_info.hStdError = child_stdout_wr;
	start_info.hStdOutput = child_stdout_wr;
	start_info.hStdInput = child_stdin_rd;
	start_info.dwFlags |= STARTF_USESTDHANDLES;

	if (!CreateProcess(NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &start_info, &proc_info))
		ERROR_EXIT(ERROR_CREATE_PROCESS);

	return 0;

#undef ERROR_EXIT

Exit:
	if (child_stdin_rd)
		CloseHandle(child_stdin_rd);
	if (child_stdin_wr)
		CloseHandle(child_stdin_wr);
	if (child_stdout_rd)
		CloseHandle(child_stdout_rd);
	if (child_stdout_wr)
		CloseHandle(child_stdout_wr);
	if (proc_info.hProcess)
		CloseHandle(proc_info.hProcess);
	if (proc_info.hThread)
		CloseHandle(proc_info.hThread);

	child_stdin_rd = NULL;
	child_stdin_wr = NULL;
	child_stdout_rd = NULL;
	child_stdout_wr = NULL;
	proc_info.hProcess = NULL;
	proc_info.hThread = NULL;
	return res;
}

DWORD PipeEncoder::Encode(BYTE *data, int size)
{
	DWORD dwWritten;
	DWORD exit;
	GetExitCodeProcess(proc_info.hProcess,&exit);
	if (exit != STILL_ACTIVE)
		return ERROR_EXIT_PROCESS;
	if (!WriteFile(child_stdin_wr, data, size, &dwWritten, NULL))
		return ERROR_WRITE_INTO_PIPE;
	if (dwWritten != size)
		return ERROR_WRITE_NOT_FULL;
	return 0;
}

DWORD PipeEncoder::Finalize()
{
	if (child_stdin_rd)
		CloseHandle(child_stdin_rd);
	if (child_stdin_wr)
		CloseHandle(child_stdin_wr);
	if (child_stdout_rd)
		CloseHandle(child_stdout_rd);
	if (child_stdout_wr)
		CloseHandle(child_stdout_wr);
	if (proc_info.hProcess)
		CloseHandle(proc_info.hProcess);
	if (proc_info.hThread)
		CloseHandle(proc_info.hThread);
	if (command)
		free(command);
	return 0;
}
