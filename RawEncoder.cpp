#include "RawEncoder.h"

#include <cstring>

RawEncoder::RawEncoder(const char *filename) : f(NULL)
{
	name=(char*)malloc(strlen(filename)+1);
	strcpy(name,filename);
}

DWORD RawEncoder::Init(const WAVEFORMATEX *format)
{
	f = fopen("recording.raw","wb");
	if (!f)
		return ENCODER_ERROR_MASK;
	return 0;
}

DWORD RawEncoder::Encode(BYTE *data, int size)
{
	if (fwrite(data,1,size,f) != size)
		return ENCODER_ERROR_MASK|1;
	return 0;
}

wchar_t* RawEncoder::GetErrorString(DWORD error)
{
	switch(error)
	{
	case ENCODER_ERROR_MASK:
		return L"Can't open file";
	case (ENCODER_ERROR_MASK|1):
		return L"Can't write data in file (out of space?)";
	default:
		return NULL;
	}
}

DWORD RawEncoder::Finalize()
{
	if (f)
		fclose(f);
	free(name);
	return 0;
}
