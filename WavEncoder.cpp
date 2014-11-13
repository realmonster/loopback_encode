#include "WavEncoder.h"

#include <cstring>
#include <cstdint>

WavEncoder::WavEncoder(const wchar_t *filename) : f(NULL), format_size(0)
{
	name=(wchar_t*)malloc((wcslen(filename)+1)*sizeof(wchar_t));
	wcscpy(name, filename);
}

enum
{
	ERROR_OPEN_FILE = ENCODER_ERROR_MASK,
	ERROR_WRITE_FILE,
	ERROR_MAXIMUM_SIZE_REACHED,
};

wchar_t* WavEncoder::GetErrorString(DWORD error)
{
	switch(error)
	{
	case ERROR_OPEN_FILE:
		return L"Can't open file";
	case ERROR_WRITE_FILE:
		return L"Can't write data in file (out of space?)";
	case ERROR_MAXIMUM_SIZE_REACHED:
		return L"Maximum wav size reached";
	default:
		return NULL;
	}
}

DWORD WavEncoder::Init(const WAVEFORMATEX *format)
{
	f = _wfopen(name, L"wb");
	if (!f)
		return ERROR_OPEN_FILE;

	if (fwrite("RIFF    WAVEfmt ", 1, 16, f) != 16)
		return ERROR_WRITE_FILE;

	format_size = sizeof(WAVEFORMATEX) + format->cbSize;
	int32_t tmp = format_size;
	if (fwrite(&tmp, 1, 4, f) != 4)
		return ERROR_WRITE_FILE;

	if (fwrite(format, 1, tmp, f) != tmp)
		return ERROR_WRITE_FILE;

	if (fwrite("data    ", 1, 8, f) != 8)
		return ERROR_WRITE_FILE;

	return 0;
}

DWORD WavEncoder::Encode(BYTE *data, int size)
{
	int err = 0;
	if (ftell(f) + (long long)size >= 0x10000000LL)
	{
		err = ERROR_MAXIMUM_SIZE_REACHED;
		size = 0xFFFFFFFF-ftell(f);
	}
	if (fwrite(data, 1, size, f) != size)
		return ERROR_WRITE_FILE;

	return err;
}

DWORD WavEncoder::Finalize()
{
	if (f)
	{
		if (format_size)
		{
			int size = ftell(f);
			fseek(f, 4, SEEK_SET);
			int32_t tmp = size - 8;
			fwrite(&tmp, 1, 4, f);

			int data_start = 20+format_size+8;
			tmp = size - data_start;
			fseek(f, data_start-4, SEEK_SET);
			fwrite(&tmp, 1, 4, f);
		}
		fclose(f);
	}
	free(name);
	return 0;
}
