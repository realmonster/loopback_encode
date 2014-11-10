#ifndef RAW_ENCODER_H
#define RAW_ENCODER_H
#pragma once

#include "Encoder.h"

#include <cstdio>

struct RawEncoder : IEncoder
{
	FILE *f;
	wchar_t *name;

	RawEncoder(const wchar_t *filename);
	DWORD Init(const WAVEFORMATEX *format);
	DWORD Finalize();
	DWORD Encode(BYTE *data, int size);
	wchar_t *GetErrorString(DWORD error);
};

#endif
