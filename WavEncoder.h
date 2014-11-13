#ifndef WAV_ENCODER_H
#define WAV_ENCODER_H
#pragma once

#include "Encoder.h"
#include <cstdio>

struct WavEncoder : IEncoder
{
	FILE *f;
	wchar_t *name;
	int format_size;

	WavEncoder(const wchar_t *filename);
	DWORD Init(const WAVEFORMATEX *format);
	DWORD Finalize();
	DWORD Encode(BYTE *data, int size);
	wchar_t *GetErrorString(DWORD error);
};

#endif
