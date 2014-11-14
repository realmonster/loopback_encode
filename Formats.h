#ifndef FORMAT_DETECT_H
#define FORMAT_DETECT_H
#pragma once

#include <windows.h>
#include "Mmreg.h"

bool FormatDetect(const WAVEFORMATEX* format, WORD* id);
bool FormatAllBits(const WAVEFORMATEX* format);
bool FormatIsPCM(const WAVEFORMATEX *format);

bool IsPCM(const WAVEFORMATEX *format);

void InitFormatNames();
const wchar_t* GetFormatName(unsigned short id);

#endif
