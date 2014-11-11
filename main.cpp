#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include <avrt.h>
#include <FunctionDiscoveryKeys_devpkey.h>
#include <windowsx.h>
#include <commctrl.h>

#include "resource.h"
#include "RawEncoder.h"
#include "PipeEncoder.h"

#include "Formats.h"

#include <string>
#include <vector>

#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace std;

#define ENCODE_BUFFER_SIZE (1<<20)

const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
const IID IID_IAudioClient = __uuidof(IAudioClient);
const IID IID_IAudioCaptureClient = __uuidof(IAudioCaptureClient);

struct DeviceInfo
{
	wstring name;
	wstring id;
};

vector<DeviceInfo> DevicesList;
size_t default_device;

IEncoder *Encoder;

int BufferRequest;

HANDLE CaptureThreadHandle;
HANDLE EncodeThreadHandle;

BYTE EncodeBuffer[ENCODE_BUFFER_SIZE];
int WriteIndex;
int ReadIndex;
int Lags;
bool bDone;
WAVEFORMATEXTENSIBLE format;

HRESULT DeviceGetInfo(IMMDevice *pDevice, DeviceInfo* di)
{
	LPWSTR id;
	di->id = L"";

	HRESULT hr = pDevice->GetId(&id);
	if (FAILED(hr))
		return hr;

	di->id = id;
	di->name = id;

	CoTaskMemFree(id);

	IPropertyStore *pProps;
	hr = pDevice->OpenPropertyStore(STGM_READ, &pProps);
	if (FAILED(hr))
		return hr;

	PROPVARIANT varName;

	PropVariantInit(&varName);

	hr = pProps->GetValue(PKEY_Device_FriendlyName, &varName);
	if (FAILED(hr))
	{
		pProps->Release();
		return hr;
	}

	if (varName.vt == VT_LPWSTR)
		di->name = varName.pwszVal;

	PropVariantClear(&varName);
	return S_OK;
}

void DevicesListGet()
{
	default_device = (size_t)-1;
	DevicesList.clear();

	HRESULT hr = CoInitialize(NULL);
	if (FAILED(hr))
	{
		CoUninitialize();
		return;
	}

	IMMDeviceEnumerator* pEnumerator;
	IMMDeviceCollection* pCollection;
	IMMDevice *pDevice;
	DeviceInfo di;

	hr = CoCreateInstance(
		CLSID_MMDeviceEnumerator, NULL,
		CLSCTX_ALL, IID_IMMDeviceEnumerator,
		(void**)&pEnumerator);
	if (FAILED(hr))
	{
		CoUninitialize();
		return;
	}
	
	hr = pEnumerator->EnumAudioEndpoints(eAll,
		DEVICE_STATE_ACTIVE,//|DEVICE_STATE_DISABLED|DEVICE_STATE_NOTPRESENT|DEVICE_STATE_UNPLUGGED,
		&pCollection);
	if (SUCCEEDED(hr))
	{
		UINT count;
		hr = pCollection->GetCount(&count);
		if (SUCCEEDED(hr))
		{
			for (UINT i = 0; i < count; ++i)
			{
				hr = pCollection->Item(i, &pDevice);
				if (FAILED(hr))
					continue;

				hr = DeviceGetInfo(pDevice, &di);
				pDevice->Release();

				if (FAILED(hr) && di.id.length() == 0)
					continue;

				DevicesList.push_back(di);
			}
		}
		pCollection->Release();
	}

	pEnumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &pDevice);

	hr = DeviceGetInfo(pDevice, &di);
	pDevice->Release();

	for (size_t i = 0; i < DevicesList.size(); ++i)
		if (DevicesList[i].id == di.id)
			default_device = i;

	pEnumerator->Release();
	CoUninitialize();
}

enum
{
	ERROR_COM_FAILED = 1,
	ERROR_ENUMERATOR,
	ERROR_DEVICE_NOT_FOUND,
	ERROR_DEVICE_ENDPOINT,
	ERROR_ENDPOINT_FLOW,
	ERROR_AUDIO_CLIENT,
	ERROR_AUDIO_FORMAT,
	ERROR_AUDIO_INIT,
	ERROR_AUDIO_CAPTURE,
	ERROR_CAPTURE_START,
	ERROR_PACKET_SIZE,
	ERROR_GET_BUFFER,
	ERROR_RELEASE_BUFFER,
	ERROR_FILE_OPEN,
	ERROR_FILE_WRITE,
} ERROR_CAPTURE;

const wchar_t* GetErrorString(int error)
{
	switch (error)
	{
	case ERROR_COM_FAILED: return L"CoUninitialize failed";
	case ERROR_ENUMERATOR: return L"CoCreateInstance(IMMDeviceEnumerator) failed";
	case ERROR_DEVICE_NOT_FOUND: return L"Audio Device not found";
	case ERROR_DEVICE_ENDPOINT: return L"IMMDevice->QueryInterface(IMMEndpoint) failed";
	case ERROR_ENDPOINT_FLOW: return L"IMMEndpoint->GetDataFlow() failed";
	case ERROR_AUDIO_CLIENT: return L"IMMDevice->Activate(IAudioClient) failed";
	case ERROR_AUDIO_FORMAT: return L"IAudioClient->GetMixFormat() failed";
	case ERROR_AUDIO_INIT: return L"IAudioClient->Initialize() failed";
	case ERROR_AUDIO_CAPTURE: return L"IAudioClient->GetService(IAudioCaptureClient) failed";
	case ERROR_CAPTURE_START: return L"IAudioClient->Start() failed";
	case ERROR_PACKET_SIZE: return L"IAudioCaptureClient->GetNextPacketSize() failed";
	case ERROR_GET_BUFFER: return L"IAudioCaptureClient->GetBuffer() failed";
	case ERROR_RELEASE_BUFFER: return L"IAudioCaptureClient->ReleaseBuffer() failed";
	case ERROR_FILE_OPEN: return L"Can't create file";
	case ERROR_FILE_WRITE: return L"Can't write in file (out of space?)";
	default: return NULL;
	}
}

wstring GetThreadErrorString(HANDLE thread, const wchar_t* thread_name)
{
	wstring error_message;
	DWORD exit;
	if (!GetExitCodeThread(thread, &exit))
		return wstring(L"Can't get ")+thread_name+L" thread exit code";

	const wchar_t *msg;
	wchar_t buff[15];
	if (CaptureThreadHandle == 0)
		return wstring(L"Can't create ")+thread_name+L" thread";
	else if (exit != 0)
	{
		error_message = thread_name;
		error_message += L" thread error: ";
		if (exit & ENCODER_ERROR_MASK)
			msg = Encoder->GetErrorString(exit);
		else
			msg = GetErrorString(exit);
		if (msg)
			error_message += msg;
		else
		{
			wsprintf(buff,L"0x%X",exit);
			error_message += L"Unknown error: ";
			error_message += buff;
		}
	}
	return error_message;
}

DWORD WINAPI CaptureThread(LPVOID lpParameter)
{
	LPCWSTR id = (LPCWSTR)lpParameter;
	int res = 0;

	HRESULT hr = CoInitialize(NULL);
	if (FAILED(hr))
	{
		CoUninitialize();
		return ERROR_COM_FAILED;
	}

	IMMDeviceEnumerator* pEnumerator = NULL;
	IMMDevice *pDevice = NULL;
	IMMEndpoint *pEndpoint = NULL;
	IAudioClient *pAudioClient = NULL;
	IAudioCaptureClient *pAudioCaptureClient = NULL;
	WAVEFORMATEX *pwfx = NULL;

#define ERROR_EXIT(result) if (FAILED(hr)) { res = result; goto Exit;} else (void*)0

	hr = CoCreateInstance(
		CLSID_MMDeviceEnumerator, NULL,
		CLSCTX_ALL, IID_IMMDeviceEnumerator,
		(void**)&pEnumerator);

	ERROR_EXIT(ERROR_ENUMERATOR);

	hr = pEnumerator->GetDevice(id, &pDevice);

	ERROR_EXIT(ERROR_DEVICE_NOT_FOUND);

	hr = pDevice->QueryInterface<IMMEndpoint>(&pEndpoint);

	ERROR_EXIT(ERROR_DEVICE_ENDPOINT);

	EDataFlow dataflow;
	
	hr = pEndpoint->GetDataFlow(&dataflow);

	ERROR_EXIT(ERROR_ENDPOINT_FLOW);

	hr = pDevice->Activate(
		IID_IAudioClient, CLSCTX_ALL,
		NULL, (void**)&pAudioClient);

	ERROR_EXIT(ERROR_AUDIO_CLIENT);

	hr = pAudioClient->GetMixFormat(&pwfx);
   
	ERROR_EXIT(ERROR_AUDIO_FORMAT);

	hr = Encoder->Init(pwfx);
	if (hr != 0)
	{
		res = hr;
		goto Exit;
	}

	if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
		memcpy(&format, pwfx, sizeof(format));
	else
		memcpy(&format, pwfx, sizeof(format) < pwfx->cbSize ? sizeof(format) : pwfx->cbSize);

	DWORD streamflags = 0;
	if (dataflow == eRender)
		streamflags = AUDCLNT_STREAMFLAGS_LOOPBACK;

	hr = pAudioClient->Initialize(
		AUDCLNT_SHAREMODE_SHARED, streamflags, BufferRequest*10000LL, 0, pwfx, NULL);

	ERROR_EXIT(ERROR_AUDIO_INIT);

	hr = pAudioClient->GetService(IID_IAudioCaptureClient, (void**)&pAudioCaptureClient);

	ERROR_EXIT(ERROR_AUDIO_CAPTURE);

	hr = pAudioClient->Start();

    ERROR_EXIT(ERROR_CAPTURE_START);

	while (!bDone)
	{
		Sleep(1);

		UINT32 packetLength;
		UINT32 samples;
		DWORD flags;
		BYTE *pData;

		hr = pAudioCaptureClient->GetNextPacketSize(&packetLength);

        ERROR_EXIT(ERROR_PACKET_SIZE);

        while (packetLength != 0)
        {
            // Get the available data in the shared buffer.
            hr = pAudioCaptureClient->GetBuffer(
					&pData, &samples, &flags, NULL, NULL);

            ERROR_EXIT(ERROR_GET_BUFFER);

			if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
				++Lags;

			int size = samples*pwfx->nChannels*pwfx->wBitsPerSample/8;
			for (int i=0; i<size; ++i)
			{
				EncodeBuffer[WriteIndex] = pData[i];
				WriteIndex = (WriteIndex+1)&(ENCODE_BUFFER_SIZE-1);
			}

			hr = pAudioCaptureClient->ReleaseBuffer(samples);
			ERROR_EXIT(ERROR_RELEASE_BUFFER);

            hr = pAudioCaptureClient->GetNextPacketSize(&packetLength);
            ERROR_EXIT(ERROR_PACKET_SIZE);
        }
	}

Exit:
	if (pwfx)
		CoTaskMemFree(pwfx);
	if (pAudioCaptureClient)
		pAudioCaptureClient->Release();
	if (pAudioClient)
		pAudioClient->Release();
	if (pEndpoint)
		pEndpoint->Release();
	if (pDevice)
		pDevice->Release();
	if (pEnumerator)
		pEnumerator->Release();

	CoUninitialize();
	return res;
}

DWORD WINAPI EncodeThread(LPVOID lpParameter)
{
	int res;
	while (true)
	{
		Sleep(1);
		volatile int wi = WriteIndex;
		if (bDone && ReadIndex == wi)
			break;

		while (ReadIndex != wi)
		{
			if (ReadIndex < wi)
			{
				res = Encoder->Encode(EncodeBuffer + ReadIndex, wi - ReadIndex);
				if (res != 0)
					return res;
				ReadIndex = wi;
			}
			else
			{
				res = Encoder->Encode(EncodeBuffer + ReadIndex, ENCODE_BUFFER_SIZE - ReadIndex);
				if (res != 0)
					return res;
				ReadIndex = 0;
			}
		}
	}
	return 0;
}

void TurnButtons(HWND hDlg, bool recording)
{
	BOOL rec = recording ? TRUE : FALSE;
	BOOL nrec = recording ? FALSE: TRUE;
	EnableWindow(GetDlgItem(hDlg, IDOK), nrec);
	EnableWindow(GetDlgItem(hDlg, IDCANCEL), rec);
	EnableWindow(GetDlgItem(hDlg, IDC_DEVICES), nrec);
	EnableWindow(GetDlgItem(hDlg, IDC_REFRESH), nrec);
	EnableWindow(GetDlgItem(hDlg, IDC_ENCODER), nrec);
	EnableWindow(GetDlgItem(hDlg, IDC_BUFFERREQUEST), nrec);
}

void RefreshDevices(HWND hDlg)
{
	DevicesListGet();
	HWND devices = GetDlgItem(hDlg, IDC_DEVICES);
	ComboBox_ResetContent(devices);
	for (size_t i=0; i<DevicesList.size(); ++i)
		ComboBox_AddString(devices, DevicesList[i].name.c_str());

	if (default_device != (size_t)-1)
		ComboBox_SetCurSel(devices, default_device);
}

void InitDlg(HWND hDlg)
{
	CaptureThreadHandle = NULL;
	EncodeThreadHandle = NULL;
	Encoder = NULL;

	RefreshDevices(hDlg);

	SetDlgItemText(hDlg, IDC_BUFFERREQUEST, L"0");

	HWND encoders = GetDlgItem(hDlg ,IDC_ENCODER);
	ComboBox_AddString(encoders, L"RAW");
	ComboBox_AddString(encoders, L"Pipe");
	ComboBox_SetCurSel(encoders, 0);

	SendDlgItemMessage(hDlg, IDC_PROGRESS, PBM_SETRANGE, 0, (1<<15)<<16);
	SendDlgItemMessage(hDlg, IDC_LOAD, PBM_SETRANGE, 0, (1<<15)<<16);
	TurnButtons(hDlg, false);
	SetTimer(hDlg, 1, 500, NULL);
}

void Start(HWND hDlg)
{
	size_t n = ComboBox_GetCurSel(GetDlgItem(hDlg, IDC_DEVICES));
	if (n >= DevicesList.size())
	{
		MessageBox(hDlg, L"Invalid Device Selection", L"Error", MB_OK | MB_ICONERROR);
		return;
	}

	WCHAR text[512];

	int len = GetDlgItemText(hDlg, IDC_BUFFERREQUEST, text, 512);
	if (len)
	{
		int len2;
		int count = swscanf(text, L"%d%n", &BufferRequest, &len2);
		if (count != 1 || len != len2)
		{
			MessageBox(hDlg, L"Invalid Buffer Request Time", L"Error", MB_OK | MB_ICONERROR);
			return;
		}
	}
	else
		BufferRequest = 0;

	GetDlgItemText(hDlg, IDC_EDIT1, text, 512);
	if (ComboBox_GetCurSel(GetDlgItem(hDlg, IDC_ENCODER)) == 0)
		Encoder = new RawEncoder(text);
	else
		Encoder = new PipeEncoder(text);

	WriteIndex = 0;
	ReadIndex = 0;
	Lags = 0;
	bDone = false;
	TurnButtons(hDlg, true);
	EncodeThreadHandle = CreateThread(NULL, NULL, EncodeThread, NULL, 0, NULL);
	if (!EncodeThreadHandle)
		return;
	CaptureThreadHandle = CreateThread(NULL, NULL, CaptureThread, (LPVOID)DevicesList[n].id.c_str(), 0, NULL);
	if (!CaptureThreadHandle)
		bDone = true;
}

void Stop(HWND hDlg)
{
	bDone = true;
}

void CheckThreads(HWND hDlg)
{
	if (EncodeThreadHandle || CaptureThreadHandle)
	{
		DWORD captureExit = 0;
		DWORD encodeExit = 0;

		if (CaptureThreadHandle)
			GetExitCodeThread(CaptureThreadHandle, &captureExit);
		if (EncodeThreadHandle)
			GetExitCodeThread(EncodeThreadHandle, &encodeExit);

		if (captureExit != STILL_ACTIVE
		 || encodeExit != STILL_ACTIVE)
			bDone = true;

		if (captureExit != STILL_ACTIVE
		 && encodeExit != STILL_ACTIVE)
		{
			wstring error;
			if (captureExit != 0
			 || encodeExit != 0
			 || CaptureThreadHandle == 0
			 || EncodeThreadHandle == 0)
			{
				error = GetThreadErrorString(CaptureThreadHandle,L"Capture");
				if (error.length())
					error += L"\n";
				error += GetThreadErrorString(EncodeThreadHandle,L"Encode");
			}
			TurnButtons(hDlg, false);
			if (CaptureThreadHandle)
				CloseHandle(CaptureThreadHandle);
			CaptureThreadHandle = NULL;
			if (EncodeThreadHandle)
				CloseHandle(EncodeThreadHandle);
			EncodeThreadHandle = NULL;
			if (Encoder)
			{
				Encoder->Finalize();
				delete(Encoder);
				Encoder = NULL;
			}
			if (error.length())
				MessageBox(hDlg,error.c_str(),L"Error",MB_ICONERROR|MB_OK);
		}
	}
}

void UpdateFormat(HWND hDlg)
{
	const WAVEFORMATEX &ex = format.Format;
	WORD id;
	LPCWSTR format_name;

	if (!ex.nChannels)
	{
		SetDlgItemText(hDlg, IDC_FORMAT_INFO, L"");
		return;
	}

	WCHAR tmp[10];
	WCHAR info[200];

	if (FormatDetect(&ex, &id))
	{
		format_name = GetFormatName(id);
		if (!format_name)
		{
			swprintf(tmp, L"0x%X", id);
			format_name = tmp;
		}
	}
	else
		format_name = L"Unknown";

	swprintf(info, L"Format: %s Bits: %d Channels: %d Rate: %dHz",
		format_name, ex.wBitsPerSample, ex.nChannels, ex.nSamplesPerSec);
	if (ex.wFormatTag == WAVE_FORMAT_EXTENSIBLE)
	{
#define GUID_FORMAT "%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX"
#define GUID_ARG(guid) guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]
		swprintf(info+wcslen(info), L"\nSubFormat: (" TEXT(GUID_FORMAT) L") Valid bits: %d",
			GUID_ARG(format.SubFormat), format.Samples.wValidBitsPerSample);
	}
	SetDlgItemText(hDlg, IDC_FORMAT_INFO, info);
}

void UpdateStatus(HWND hDlg)
{
	WCHAR buff[50];
	_swprintf(buff, L"Lags: %d", Lags);
	SetDlgItemText(hDlg, IDC_STATUS, buff);
}

void UpdateProgress(HWND hDlg)
{
	volatile int ri = ReadIndex;
	volatile int wi = WriteIndex;
	if (wi < ri)
		wi += ENCODE_BUFFER_SIZE;

	int rp = MulDiv(ri, (1<<15), ENCODE_BUFFER_SIZE);
	int wp = MulDiv(wi-ri, (1<<15), ENCODE_BUFFER_SIZE);
	SendDlgItemMessage(hDlg, IDC_PROGRESS, PBM_SETPOS, rp, 0);
	if (rp) // trick to avoid smooth step-increasing animation
		SendDlgItemMessage(hDlg, IDC_PROGRESS, PBM_SETPOS, rp-1, 0);
	SendDlgItemMessage(hDlg, IDC_LOAD, PBM_SETPOS, wp, 0);
	if (wp) // trick to avoid smooth step-increasing animation
		SendDlgItemMessage(hDlg, IDC_LOAD, PBM_SETPOS, wp-1, 0);
}

INT_PTR CALLBACK EncodeProc(HWND hDlg, UINT uCmd, WPARAM wParam, LPARAM lParam)
{
	switch (uCmd)
	{
	case WM_INITDIALOG:
		InitDlg(hDlg);
		break;

	case WM_CLOSE:
		EndDialog(hDlg, NULL);
		break;

	case WM_COMMAND:
		switch (wParam)
		{
		case IDOK:
			Start(hDlg);
			break;
		case IDCANCEL:
			Stop(hDlg);
			break;
		case IDC_REFRESH:
			RefreshDevices(hDlg);
			break;
		}
		break;
	case WM_TIMER:
		CheckThreads(hDlg);
		UpdateFormat(hDlg);
		UpdateProgress(hDlg);
		UpdateStatus(hDlg);
		break;
	}
	return FALSE;
}

int CALLBACK WinMain( HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	InitCommonControls();
	InitFormatNames();
	DialogBox(hInstance, MAKEINTRESOURCE(IDD_ENCODE), NULL, EncodeProc);
	return 0;
}
