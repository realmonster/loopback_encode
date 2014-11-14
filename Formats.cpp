#include "Formats.h"

#include "ks.h"
#include "Ksmedia.h"

// returns format id in style of WAVEFORMATEX wFormatTag, if it possible
bool FormatDetect(const WAVEFORMATEX* format, WORD* id)
{
	if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
	{
		const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE*)format;
		if (IS_VALID_WAVEFORMATEX_GUID(&ext->SubFormat))
		{
			*id = EXTRACT_WAVEFORMATEX_ID(&ext->SubFormat);
			return true;
		}
		return false;
	}
	*id = format->wFormatTag;
	return true;
}

// checks that all bits in sample is used
bool FormatAllBits(const WAVEFORMATEX* format)
{
	if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
	{
		const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE*)format;
		return (ext->Samples.wValidBitsPerSample == format->wBitsPerSample);
	}
	return true;
}

bool FormatIsPCM(const WAVEFORMATEX *format)
{
	if (format->wFormatTag == WAVE_FORMAT_PCM)
		return true;
	if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE)
		return false;
	return (((const WAVEFORMATEXTENSIBLE*)format)->SubFormat == KSDATAFORMAT_SUBTYPE_PCM);
}
