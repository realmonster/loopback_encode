#ifndef RAW_ENCODER_H
#define RAW_ENCODER_H
#pragma once

#include "Encoder.h"

#include <cstdio>

struct RawEncoder : IEncoder
{
	FILE *f;
	char *name;

	RawEncoder(const char *filename);
	DWORD Init(const WAVEFORMATEX *format);
	DWORD Finalize();
	DWORD Encode(BYTE *data, int size);
	wchar_t *GetErrorString(DWORD error);
};

#endif
