#include "PipeEncoder.h"
#include "Formats.h"

#include <cstring>
#include <string>

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

static std::wstring AutoReplace(const wchar_t *command, const WAVEFORMATEX *format)
{
	std::wstring res;
	wchar_t buff[50];
	for (int i=0; command[i];)
	{
		if (command[i] == '%')
		{
			if (!wcsncmp(L"%b%", command+i, 3))
			{
				swprintf(buff, L"%d", format->wBitsPerSample);
				res += buff;
				i+=3;
				continue;
			}
			else if (!wcsncmp(L"%c%", command+i, 3))
			{
				swprintf(buff, L"%d", format->nChannels);
				res += buff;
				i += 3;
				continue;
			}
			else if (!wcsncmp(L"%r%", command+i, 3))
			{
				swprintf(buff, L"%d", format->nSamplesPerSec);
				res += buff;
				i += 3;
				continue;
			}
			else if (!wcsncmp(L"%f%", command+i, 3))
			{
				WORD id;
				if (!FormatDetect(format, &id))
					id = -1;

				switch(id)
				{
				case WAVE_FORMAT_PCM:
					swprintf(buff, L"%c%d%s",
						format->wBitsPerSample==8?L'u':L's',
						format->wBitsPerSample,
						format->wBitsPerSample<=8?L"":L"le");
					res += buff;
					break;

				case WAVE_FORMAT_IEEE_FLOAT:
					if (format->wBitsPerSample == 32)
						res += L"f32le";
					else if (format->wBitsPerSample == 64)
						res += L"f64le";
					else goto unk;
					break;

				default:
				unk:
					res += L"unknown";
				}
				i += 3;
				continue;
			}
		}
		res += command[i];
		++i;
	}
	return res;
}

DWORD PipeEncoder::Init(const WAVEFORMATEX *format)
{
	SECURITY_ATTRIBUTES attr;
	HANDLE tmp = NULL;
	DWORD res = 0;
	STARTUPINFO start_info;
	WCHAR* tmp_command = NULL;
	std::wstring new_command;

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

	new_command = AutoReplace(command, format);

	tmp_command = (WCHAR*)malloc((new_command.length()+1)*sizeof(WCHAR));
	if (!tmp_command)
		return ERROR_OUT_OF_MEMORY;

	wcscpy(tmp_command, new_command.c_str());
	if (!CreateProcess(NULL, tmp_command, NULL, NULL, TRUE, 0, NULL, NULL, &start_info, &proc_info))
		ERROR_EXIT(ERROR_CREATE_PROCESS);

	free(tmp_command);
	return 0;

#undef ERROR_EXIT

Exit:
	if (tmp_command)
		free(tmp_command);
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
