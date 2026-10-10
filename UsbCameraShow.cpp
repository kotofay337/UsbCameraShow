#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <shlwapi.h>
#include <wrl/client.h> // Для ComPtr
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

using Microsoft::WRL::ComPtr;

// ---------- Глобальные переменные ----------
HWND g_hWnd = nullptr;
ComPtr<IMFSourceReader> g_pReader = nullptr;
UINT32 g_width = 0;
UINT32 g_height = 0;
std::vector<BYTE> g_frameBuffer;   // RGB32
BITMAPINFO g_bmi = {};

// Таблица для быстрой обрезки значений [0..255] (Убирает медленные if/else)
static BYTE g_ClampTable[1024];
static void InitClampTable() {
    for (int i = 0; i < 1024; ++i) {
        int val = i - 256;
        g_ClampTable[i] = (val < 0) ? 0 : (val > 255) ? 255 : (BYTE)val;
    }
}
inline BYTE Clamp(int val) {
    return g_ClampTable[val + 256];
}

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

// ---------- Оптимизированная конвертация I420 -> RGB32 (BT.601) ----------
static void I420_to_RGB32(const BYTE* yuv, BYTE* rgb, int width, int height)
{
    const BYTE* Y = yuv;
    const BYTE* U = yuv + width * height;
    const BYTE* V = U + ((width + 1) >> 1) * ((height + 1) >> 1);

    const int halfW = width >> 1;

    for (int y = 0; y < height; ++y) {
        const BYTE* yRow = Y + y * width;
        // Предвычисляем указатели на хрома-строки (избавляемся от деления y / 2)
        const BYTE* uRow = U + (y >> 1) * halfW;
        const BYTE* vRow = V + (y >> 1) * halfW;
        BYTE* rgbRow = rgb + y * width * 4;

        for (int x = 0; x < width; ++x) {
            int C = yRow[x] - 16;
            int x_half = x >> 1; // вместо x / 2
            int D = uRow[x_half] - 128;
            int E = vRow[x_half] - 128;

            int C298 = 298 * C + 128;

            // Быстрый маппинг по таблице без ветвлений if
            int R = (C298 + 409 * E) >> 8;
            int G = (C298 - 100 * D - 208 * E) >> 8;
            int B = (C298 + 516 * D) >> 8;

            int idx = x << 2; // x * 4
            rgbRow[idx + 0] = Clamp(B);
            rgbRow[idx + 1] = Clamp(G);
            rgbRow[idx + 2] = Clamp(R);
            rgbRow[idx + 3] = 255;
        }
    }
}

// ---------- Инициализация камеры ----------
HRESULT InitCamera()
{
    HRESULT hr = S_OK;
    const wchar_t* step = L"start";
    ComPtr<IMFAttributes> pAttributes;
    IMFActivate** ppDevices = nullptr;
    UINT32 count = 0;
    ComPtr<IMFMediaSource> pSource;
    ComPtr<IMFAttributes> pReaderAttributes;
    ComPtr<IMFMediaType> pChosenType;
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
    hr = MFEnumDeviceSources(pAttributes.Get(), &ppDevices, &count);
    if (FAILED(hr)) goto done;

    if (count == 0) { step = L"no devices"; hr = E_FAIL; goto done; }

    step = L"ActivateObject";
    hr = ppDevices[0]->ActivateObject(IID_PPV_ARGS(&pSource));
    if (FAILED(hr)) goto done;

    step = L"MFCreateAttributes (reader)";
    hr = MFCreateAttributes(&pReaderAttributes, 1);
    if (FAILED(hr)) goto done;

    step = L"SetUINT32 (VIDEO_PROCESSING)";
    hr = pReaderAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (FAILED(hr)) goto done;

    step = L"MFCreateSourceReaderFromMediaSource";
    hr = MFCreateSourceReaderFromMediaSource(pSource.Get(), pReaderAttributes.Get(), &g_pReader);
    if (FAILED(hr)) goto done;

    // Цикл 1: Ищем первый I420 640x480
    step = L"Find I420 (640x480)";
    for (DWORD i = 0; ; i++)
    {
        ComPtr<IMFMediaType> pType;
        hr = g_pReader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &pType);
        if (hr == MF_E_NO_MORE_TYPES) { hr = S_OK; break; }
        if (FAILED(hr)) { hr = S_OK; break; } // Сбрасываем ошибку, чтобы не упасть ниже

        GUID subtype = {};
        pType->GetGUID(MF_MT_SUBTYPE, &subtype);

        UINT32 w = 0, h = 0;
        MFGetAttributeSize(pType.Get(), MF_MT_FRAME_SIZE, &w, &h);

        if (subtype == MFVideoFormat_I420 && w == 640 && h == 480) {
            pChosenType = pType; // Умный указатель сам сделает AddRef / Release корректно
            DebugPrint(L"[InitCamera] Found I420 640x480 at index %lu\n", i);
            break;
        }
    }

    // Цикл 2: Если не нашли 640x480, ищем любое I420
    if (!pChosenType) {
        step = L"Find any I420";
        for (DWORD i = 0; ; i++)
        {
            ComPtr<IMFMediaType> pType;
            hr = g_pReader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &pType);
            if (hr == MF_E_NO_MORE_TYPES) { hr = S_OK; break; }
            if (FAILED(hr)) { hr = S_OK; break; }

            GUID subtype = {};
            pType->GetGUID(MF_MT_SUBTYPE, &subtype);

            if (subtype == MFVideoFormat_I420) {
                pChosenType = pType;
                DebugPrint(L"[InitCamera] Found any I420 at index %lu\n", i);
                break;
            }
        }
    }

    if (!pChosenType) {
        step = L"No I420 format";
        hr = MF_E_INVALIDMEDIATYPE;
        goto done;
    }

    step = L"SetCurrentMediaType (I420)";
    hr = g_pReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pChosenType.Get());
    if (FAILED(hr)) goto done;

    step = L"MFGetAttributeSize";
    hr = MFGetAttributeSize(pChosenType.Get(), MF_MT_FRAME_SIZE, &g_width, &g_height);
    if (FAILED(hr)) goto done;

    DebugPrint(L"[InitCamera] Selected %lux%lu I420\n", g_width, g_height);

    g_frameBuffer.resize(static_cast<size_t>(g_width) * g_height * 4);

    ZeroMemory(&g_bmi, sizeof(g_bmi));
    g_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    g_bmi.bmiHeader.biWidth = static_cast<LONG>(g_width);
    g_bmi.bmiHeader.biHeight = -static_cast<LONG>(g_height); // Сверху вниз
    g_bmi.bmiHeader.biPlanes = 1;
    g_bmi.bmiHeader.biBitCount = 32;
    g_bmi.bmiHeader.biCompression = BI_RGB;

    hr = S_OK;

done:
    if (ppDevices) {
        for (UINT32 i = 0; i < count; i++) {
            if (ppDevices[i]) ppDevices[i]->Release();
        }
        CoTaskMemFree(ppDevices);
    }

    if (FAILED(hr)) {
        DebugPrint(L"[InitCamera] FAILED at '%s', hr=0x%08X\n", step, hr);
        wchar_t msg[256];
        swprintf_s(msg, L"Шаг: %s\nHRESULT: 0x%08X", step, hr);
        MessageBoxW(nullptr, msg, L"Ошибка инициализации камеры", MB_OK);

        g_pReader.Reset();
        if (mfStarted) MFShutdown();
    }
    return hr;
}

// ---------- Захват одного кадра ----------
HRESULT GrabFrame()
{
    if (!g_pReader) return E_FAIL;

    DWORD streamIndex = 0, flags = 0;
    LONGLONG timestamp = 0;
    ComPtr<IMFSample> pSample;
    ComPtr<IMFMediaBuffer> pBuffer;

    HRESULT hr = g_pReader->ReadSample(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
        &streamIndex, &flags, &timestamp, &pSample);

    if (FAILED(hr)) return hr;
    if (flags & MF_SOURCE_READERF_STREAMTICK) return S_OK;
    if (!pSample) return S_OK;

    hr = pSample->GetBufferByIndex(0, &pBuffer);
    if (SUCCEEDED(hr)) {
        BYTE* pData = nullptr;
        DWORD maxLen = 0, curLen = 0;
        hr = pBuffer->Lock(&pData, &maxLen, &curLen);
        if (SUCCEEDED(hr)) {
            size_t need = static_cast<size_t>(g_width) * g_height * 3 / 2;
            if (curLen >= need) {
                I420_to_RGB32(pData, g_frameBuffer.data(), static_cast<int>(g_width), static_cast<int>(g_height));
                // Заставляем окно перерисоваться в штатном WM_PAINT (убирает мерцание)
                InvalidateRect(g_hWnd, nullptr, FALSE);
            }
            pBuffer->Unlock();
        }
    }
    return S_OK;
}

// ---------- Отрисовка кадра (Вызывается только в WM_PAINT) ----------
void DrawFrame(HWND hWnd, HDC hdc)
{
    if (g_frameBuffer.empty()) return;

    RECT rc;
    GetClientRect(hWnd, &rc);
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;

    if (cw <= 0 || ch <= 0) return;

    double imgAspect = static_cast<double>(g_width) / g_height;
    double winAspect = static_cast<double>(cw) / ch;

    int dstW, dstH, dstX, dstY;
    if (winAspect > imgAspect) {
        dstH = ch;
        dstW = static_cast<int>(ch * imgAspect + 0.5);
        dstX = (cw - dstW) / 2;
        dstY = 0;
    }
    else {
        dstW = cw;
        dstH = static_cast<int>(cw / imgAspect + 0.5);
        dstX = 0;
        dstY = (ch - dstH) / 2;
    }

    // Умная закраска черных полей (Исключаем область кадра из отрисовки фона)
    // Это полностью уничтожает мерцание экрана!
    HRGN hRgnTotal = CreateRectRgnIndirect(&rc);
    HRGN hRgnVideo = CreateRectRgn(dstX, dstY, dstX + dstW, dstY + dstH);
    HRGN hRgnBkgnd = CreateRectRgn(0, 0, 0, 0);

    CombineRgn(hRgnBkgnd, hRgnTotal, hRgnVideo, RGN_DIFF);
    HBRUSH hbr = (HBRUSH)GetStockObject(BLACK_BRUSH);
    FillRgn(hdc, hRgnBkgnd, hbr);

    DeleteObject(hRgnTotal);
    DeleteObject(hRgnVideo);
    DeleteObject(hRgnBkgnd);

    // Рисуем сам кадр
    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, nullptr);

    StretchDIBits(
        hdc,
        dstX, dstY, dstW, dstH,
        0, 0, static_cast<int>(g_width), static_cast<int>(g_height),
        g_frameBuffer.data(), &g_bmi,
        DIB_RGB_COLORS, SRCCOPY);
}
// ---------- Оконная процедура ----------
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        DrawFrame(hWnd, hdc);
        EndPaint(hWnd, &ps);
    }
    return 0;
    case WM_CLOSE:   DestroyWindow(hWnd); return 0;
    case WM_DESTROY: PostQuitMessage(0);  return 0;
    case WM_ERASEBKGND:
        return 1; // Запрещаем Windows стирать фон, мы всё делаем в DrawFrame
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}
// ---------- Точка входа ----------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    InitClampTable();
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return 1;
    const wchar_t CLASS_NAME[] = L"CameraWindowClass";
    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); // Чтобы курсор не песочил
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
        hr = GrabFrame();
        if (FAILED(hr)) break;
        Sleep(15); // Оптимальный интервал для ~60 FPS preview
    }
cleanup:
    g_pReader.Reset();
    MFShutdown();
    CoUninitialize();
    return 0;
}