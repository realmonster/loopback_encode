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

#include <string>
#include <vector>

#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace std;

#define ENCODE_BUFFER_SIZE (1<<16)

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

HANDLE CaptureThreadHandle;
HANDLE EncodeThreadHandle;

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

BYTE EncodeBuffer[ENCODE_BUFFER_SIZE];
int WriteIndex;
int ReadIndex;
bool bDone;

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

	DWORD streamflags = 0;
	if (dataflow == eRender)
		streamflags = AUDCLNT_STREAMFLAGS_LOOPBACK;

	hr = pAudioClient->Initialize(
		AUDCLNT_SHAREMODE_SHARED, streamflags, 0, 0, pwfx, NULL);

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
	Encoder = new RawEncoder("recording");
	WriteIndex = 0;
	ReadIndex = 0;
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
		break;
	}
	return FALSE;
}

int CALLBACK WinMain( HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	InitCommonControls();
	DialogBox(hInstance, MAKEINTRESOURCE(IDD_ENCODE), NULL, EncodeProc);
	return 0;
}
