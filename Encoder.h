#ifndef ENCODER_H
#define ENCODER_H
#pragma once

#include <windows.h>

#define ENCODER_ERROR_MASK (1<<15)

struct IEncoder
{
	virtual DWORD Init(const WAVEFORMATEX *format) = 0;
	virtual DWORD Finalize() = 0;
	virtual DWORD Encode(BYTE *data, int size) = 0;
	virtual wchar_t *GetErrorString(DWORD error) = 0;
};

#endif
