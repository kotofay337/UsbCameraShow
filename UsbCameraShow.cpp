#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <shlwapi.h>
#include <vector>
#include <cstdio>
#include <cstdarg>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")

// ---------- Глобальные переменные ----------
HWND g_hWnd = nullptr;
IMFSourceReader* g_pReader = nullptr;
UINT32 g_width = 0;
UINT32 g_height = 0;
std::vector<BYTE> g_frameBuffer;   // RGB32
BITMAPINFO g_bmi = {};
static int g_drawX = 0, g_drawY = 0, g_drawW = 0, g_drawH = 0;

// ---------- Отладочный вывод ----------
static void DebugPrint(const wchar_t* fmt, ...)
{
    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
    va_end(args);
    OutputDebugStringW(buf);
}

// ---------- Конвертация I420 -> RGB32 (BT.601) ----------
static void I420_to_RGB32(const BYTE* yuv, BYTE* rgb, int width, int height)
{
    const BYTE* Y = yuv;
    const BYTE* U = yuv + width * height;
    const BYTE* V = U + (width / 2) * (height / 2);

    const int halfW = width / 2;

    for (int y = 0; y < height; ++y) {
        const BYTE* yRow = Y + y * width;
        const BYTE* uRow = U + (y / 2) * halfW;
        const BYTE* vRow = V + (y / 2) * halfW;
        BYTE* rgbRow = rgb + y * width * 4;

        for (int x = 0; x < width; ++x) {
            int Yv = yRow[x];
            int Uv = uRow[x / 2];
            int Vv = vRow[x / 2];

            int C = Yv - 16;
            int D = Uv - 128;
            int E = Vv - 128;

            int R = (298 * C + 409 * E + 128) >> 8;
            int G = (298 * C - 100 * D - 208 * E + 128) >> 8;
            int B = (298 * C + 516 * D + 128) >> 8;

            if (R < 0) R = 0; else if (R > 255) R = 255;
            if (G < 0) G = 0; else if (G > 255) G = 255;
            if (B < 0) B = 0; else if (B > 255) B = 255;

            int idx = x * 4;
            rgbRow[idx + 0] = (BYTE)B;
            rgbRow[idx + 1] = (BYTE)G;
            rgbRow[idx + 2] = (BYTE)R;
            rgbRow[idx + 3] = 255;
        }
    }
}

// ---------- Инициализация камеры ----------
HRESULT InitCamera()
{
    HRESULT hr = S_OK;
    const wchar_t* step = L"start";
    IMFAttributes* pAttributes = nullptr;
    IMFActivate** ppDevices = nullptr;
    UINT32 count = 0;
    IMFMediaSource* pSource = nullptr;
    IMFAttributes* pReaderAttributes = nullptr;
    IMFMediaType* pChosenType = nullptr;
    bool mfStarted = false;

    step = L"MFStartup";
    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) goto done;
    mfStarted = true;

    step = L"MFCreateAttributes (devices)";
    hr = MFCreateAttributes(&pAttributes, 1);
    if (FAILED(hr)) goto done;

    step = L"SetGUID (SOURCE_TYPE)";
    hr = pAttributes->SetGUID(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) goto done;

    step = L"MFEnumDeviceSources";
    hr = MFEnumDeviceSources(pAttributes, &ppDevices, &count);
    if (FAILED(hr)) goto done;

    if (count == 0) { step = L"no devices"; hr = E_FAIL; goto done; }

    step = L"ActivateObject";
    hr = ppDevices[0]->ActivateObject(IID_PPV_ARGS(&pSource));
    if (FAILED(hr)) goto done;

    step = L"MFCreateAttributes (reader)";
    hr = MFCreateAttributes(&pReaderAttributes, 1);
    if (FAILED(hr)) goto done;

    step = L"SetUINT32 (VIDEO_PROCESSING)";
    hr = pReaderAttributes->SetUINT32(
        MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (FAILED(hr)) goto done;

    step = L"MFCreateSourceReaderFromMediaSource";
    hr = MFCreateSourceReaderFromMediaSource(
        pSource, pReaderAttributes, &g_pReader);
    if (FAILED(hr)) goto done;

    // --- Ищем первый I420 с разрешением 640x480, иначе просто первый I420 ---
    step = L"Find I420 (640x480)";
    for (DWORD i = 0; ; i++)
    {
        IMFMediaType* pType = nullptr;
        hr = g_pReader->GetNativeMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &pType);
        if (hr == MF_E_NO_MORE_TYPES) { hr = S_OK; break; }
        if (FAILED(hr)) break;

        GUID subtype = {};
        pType->GetGUID(MF_MT_SUBTYPE, &subtype);

        UINT32 w = 0, h = 0;
        MFGetAttributeSize(pType, MF_MT_FRAME_SIZE, &w, &h);

        if (subtype == MFVideoFormat_I420 && w == 640 && h == 480) {
            pChosenType = pType;
            pChosenType->AddRef();
            pType->Release();
            DebugPrint(L"[InitCamera] Found I420 640x480 at index %lu\n", i);
            break;
        }
        pType->Release();
    }

    if (!pChosenType) {
        step = L"Find any I420";
        for (DWORD i = 0; ; i++)
        {
            IMFMediaType* pType = nullptr;
            hr = g_pReader->GetNativeMediaType(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &pType);
            if (hr == MF_E_NO_MORE_TYPES) { hr = S_OK; break; }
            if (FAILED(hr)) break;

            GUID subtype = {};
            pType->GetGUID(MF_MT_SUBTYPE, &subtype);

            if (subtype == MFVideoFormat_I420) {
                pChosenType = pType;
                pChosenType->AddRef();
                pType->Release();
                DebugPrint(L"[InitCamera] Found I420 at index %lu\n", i);
                break;
            }
            pType->Release();
        }
    }

    if (!pChosenType) {
        step = L"No I420 format";
        hr = MF_E_INVALIDMEDIATYPE;
        goto done;
    }

    step = L"SetCurrentMediaType (I420)";
    hr = g_pReader->SetCurrentMediaType(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pChosenType);
    if (FAILED(hr)) goto done;

    step = L"MFGetAttributeSize";
    hr = MFGetAttributeSize(pChosenType, MF_MT_FRAME_SIZE, &g_width, &g_height);
    if (FAILED(hr)) goto done;

    DebugPrint(L"[InitCamera] Selected %lux%lu I420\n", g_width, g_height);

    // RGB32 буфер для GDI
    g_frameBuffer.resize(static_cast<size_t>(g_width) * g_height * 4);

    ZeroMemory(&g_bmi, sizeof(g_bmi));
    g_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    g_bmi.bmiHeader.biWidth = static_cast<LONG>(g_width);
    g_bmi.bmiHeader.biHeight = -static_cast<LONG>(g_height);
    g_bmi.bmiHeader.biPlanes = 1;
    g_bmi.bmiHeader.biBitCount = 32;
    g_bmi.bmiHeader.biCompression = BI_RGB;

    hr = S_OK;

done:
    if (pChosenType)        pChosenType->Release();
    if (pReaderAttributes)  pReaderAttributes->Release();
    if (pSource)            pSource->Release();

    if (ppDevices) {
        for (UINT32 i = 0; i < count; i++) ppDevices[i]->Release();
        CoTaskMemFree(ppDevices);
    }
    if (pAttributes) pAttributes->Release();

    if (FAILED(hr)) {
        DebugPrint(L"[InitCamera] FAILED at '%s', hr=0x%08X\n", step, hr);
        wchar_t msg[256];
        swprintf_s(msg, L"Шаг: %s\nHRESULT: 0x%08X", step, hr);
        MessageBoxW(nullptr, msg, L"Ошибка инициализации камеры", MB_OK);

        if (g_pReader) { g_pReader->Release(); g_pReader = nullptr; }
        if (mfStarted) MFShutdown();
    }
    return hr;
}

// ---------- Захват и отрисовка одного кадра ----------
HRESULT GrabAndDrawFrame()
{
    if (!g_pReader || !g_hWnd) return E_FAIL;

    DWORD streamIndex = 0, flags = 0;
    LONGLONG timestamp = 0;
    IMFSample* pSample = nullptr;
    IMFMediaBuffer* pBuffer = nullptr;

    HRESULT hr = g_pReader->ReadSample(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
        &streamIndex, &flags, &timestamp, &pSample);

    if (FAILED(hr)) return hr;
    if (flags & MF_SOURCE_READERF_STREAMTICK) {
        if (pSample) pSample->Release();
        return S_OK;
    }
    if (!pSample) return S_OK;

    hr = pSample->GetBufferByIndex(0, &pBuffer);
    if (SUCCEEDED(hr)) {
        BYTE* pData = nullptr;
        DWORD maxLen = 0, curLen = 0;
        hr = pBuffer->Lock(&pData, &maxLen, &curLen);
        if (SUCCEEDED(hr)) {
            size_t need = static_cast<size_t>(g_width) * g_height * 3 / 2;
            if (curLen >= need) {
                I420_to_RGB32(pData, g_frameBuffer.data(),
                    static_cast<int>(g_width),
                    static_cast<int>(g_height));
            }
            else {
                DebugPrint(L"[GrabFrame] buffer too small: %lu < %zu\n",
                    curLen, need);
            }
            pBuffer->Unlock();

            HDC hdc = GetDC(g_hWnd);
            if (hdc) {
                RECT rc;
                GetClientRect(g_hWnd, &rc);
                int cw = rc.right - rc.left;
                int ch = rc.bottom - rc.top;

                if (cw > 0 && ch > 0) {
                    // Соотношение сторон кадра и окна
                    double imgAspect = static_cast<double>(g_width) / g_height;
                    double winAspect = static_cast<double>(cw) / ch;

                    int dstW, dstH, dstX, dstY;
                    if (winAspect > imgAspect) {
                        // Окно шире кадра — поля слева/справа
                        dstH = ch;
                        dstW = static_cast<int>(ch * imgAspect + 0.5);
                        dstX = (cw - dstW) / 2;
                        dstY = 0;
                    }
                    else {
                        // Окно выше кадра — поля сверху/снизу
                        dstW = cw;
                        dstH = static_cast<int>(cw / imgAspect + 0.5);
                        dstX = 0;
                        dstY = (ch - dstH) / 2;
                    }

                    // Запоминаем — пригодится, если понадобится hit-test / overlay
                    g_drawX = dstX; g_drawY = dstY;
                    g_drawW = dstW; g_drawH = dstH;

                    // Заливаем фон чёрным, чтобы поля не мерцали остатками кадра
                    HBRUSH hbr = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
                    FillRect(hdc, &rc, hbr);

                    // Рисуем кадр, растянутый в dstW×dstH
                    SetStretchBltMode(hdc, HALFTONE);
                    SetBrushOrgEx(hdc, 0, 0, nullptr);

                    StretchDIBits(
                        hdc,
                        dstX, dstY, dstW, dstH,
                        0, 0, static_cast<int>(g_width), static_cast<int>(g_height),
                        g_frameBuffer.data(), &g_bmi,
                        DIB_RGB_COLORS, SRCCOPY);
                }
                ReleaseDC(g_hWnd, hdc);
            }
        }
        pBuffer->Release();
    }
    pSample->Release();
    return S_OK;
}

// ---------- Оконная процедура ----------
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CLOSE:   DestroyWindow(hWnd); return 0;
    case WM_DESTROY: PostQuitMessage(0);  return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// ---------- Точка входа ----------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return 1;

    const wchar_t CLASS_NAME[] = L"CameraWindowClass";
    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;

    if (!RegisterClass(&wc)) { CoUninitialize(); return 1; }

    g_hWnd = CreateWindowExW(
        0, CLASS_NAME,
        L"USB Camera Preview (Media Foundation)",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
        nullptr, nullptr, hInstance, nullptr);

    if (!g_hWnd) { CoUninitialize(); return 1; }

    ShowWindow(g_hWnd, nCmdShow);

    hr = InitCamera();
    if (FAILED(hr)) {
        DestroyWindow(g_hWnd);
        CoUninitialize();
        return 1;
    }

    MSG msg = {};
    for (;;) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto cleanup;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        hr = GrabAndDrawFrame();
        if (FAILED(hr)) break;
        Sleep(10);
    }

cleanup:
    if (g_pReader) { g_pReader->Release(); g_pReader = nullptr; }
    MFShutdown();
    CoUninitialize();
    return 0;
}