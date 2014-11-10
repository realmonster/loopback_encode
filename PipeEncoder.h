#ifndef PIPE_ENCODER_H
#define PIPE_ENCODER_H
#pragma once

#include "Encoder.h"

struct PipeEncoder : IEncoder
{
	wchar_t *command;
	HANDLE child_stdin_rd;
	HANDLE child_stdin_wr;
	HANDLE child_stdout_rd;
	HANDLE child_stdout_wr;
	PROCESS_INFORMATION proc_info;

	PipeEncoder(const wchar_t *command_line);
	DWORD Init(const WAVEFORMATEX *format);
	DWORD Finalize();
	DWORD Encode(BYTE *data, int size);
	wchar_t *GetErrorString(DWORD error);
};

#endif
