#include "framework.h"
#include <stdio.h>
#include <windows.h>
#include <ShObjIdl.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
using namespace std;

WNDCLASS main_wc, original_wc, schedule_wc, completed_wc, result_wc;
ATOM main_wClass, original_wClass, schedule_wClass, completed_wClass, result_wClass;
HWND main_hwnd, original_hwnd, schedule_hwnd, completed_hwnd, result_hwnd;
HWND open_button_hwnd, close_button_hwnd, save_button_hwnd, cancel_operation_button_hwnd, memory_monopoly_checkbox_hwnd,
start_grayscale_button_hwnd, start_negative_button_hwnd, start_box_blur_button_hwnd, start_gaussian_blur_button_hwnd,
remove_thread_button_hwnd, add_thread_button_hwnd, threads_count_hwnd, toggle_pause_button_hwnd, reset_windows_pos_button_hwnd;

int width, height, colors;
int pixels_count;
uint32_t* original_pixels, * schedule_pixels, * completed_pixels, * result_pixels;
BITMAPINFO bitmapinfo;
#define PIX(m,x,y) ((m)[(height - (y) - 1) * width + (x)])
#define PIXCOMP(p,i) (((unsigned char*)&(p))[(2-i)])
#define MAKEPIX(r,g,b) ((uint32_t(r)<<16)|(uint32_t(g)<<8)|uint32_t(b))

bool opened = false, working = false, pause = false, finished = false;

#define THREADS_MAX 8
uint32_t thread_colors[THREADS_MAX] = {
    0xFF0000,
    0x00FF00,
    0x0000FF,
    0x555555,
    0xED5314,
    0xFFB92A,
    0xFEEB51,
    0x9BCA3E,
};
struct LineInterval { int begin, end; };
vector<LineInterval> free_intervals_pool;

struct Thread {
    HANDLE handle;
    bool stop;
    bool pause;
    uint32_t color;
    LineInterval interval;
};
vector<Thread*> threads;
CRITICAL_SECTION GetNextLineSection;
CONDITION_VARIABLE PauseCond;
CRITICAL_SECTION MemoryMonopolySection;
CONDITION_VARIABLE MonopolyBusyCond;
bool memory_monopoly;
int memory_monopoly_check_stop;

void refresh_state() {
    EnableWindow(open_button_hwnd, !working);
    ShowWindow(close_button_hwnd, opened ? SW_SHOW : SW_HIDE);
    ShowWindow(save_button_hwnd, finished ? SW_SHOW : SW_HIDE);

    ShowWindow(original_hwnd, opened ? SW_SHOW : SW_HIDE);
    EnableWindow(start_grayscale_button_hwnd, opened);
    EnableWindow(start_negative_button_hwnd, opened);
    EnableWindow(start_box_blur_button_hwnd, opened);
    EnableWindow(start_gaussian_blur_button_hwnd, opened);

    ShowWindow(schedule_hwnd, working ? SW_SHOW : SW_HIDE);
    ShowWindow(completed_hwnd, working ? SW_SHOW : SW_HIDE);
    ShowWindow(result_hwnd, (working || finished) ? SW_SHOW : SW_HIDE);

    ShowWindow(cancel_operation_button_hwnd, (working || finished) ? SW_SHOW : SW_HIDE);
    SetWindowText(cancel_operation_button_hwnd, finished ? L"Видалити" : L"Стоп");

    ShowWindow(remove_thread_button_hwnd, working ? SW_SHOW : SW_HIDE);
    ShowWindow(add_thread_button_hwnd, working ? SW_SHOW : SW_HIDE);
    ShowWindow(threads_count_hwnd, working ? SW_SHOW : SW_HIDE);
    ShowWindow(toggle_pause_button_hwnd, working ? SW_SHOW : SW_HIDE);
    ShowWindow(memory_monopoly_checkbox_hwnd, working ? SW_SHOW : SW_HIDE);

    wstring text = L"Потоків: " + to_wstring((int)threads.size());
    if (pause) text += L"\nПАУЗА";
    SetWindowText(threads_count_hwnd, text.c_str());
}

void draw_schedule(LineInterval interval, uint32_t color) {
    for (int y = interval.begin; y < interval.end; ++y)
        for (int x = 0; x < width; ++x)
            PIX(schedule_pixels, x, y) = color;
}

struct ThreadControlButtons {
    int id;
    HWND Stop, Pause, PrDown, PrUp, PrVal;
    COLORREF Color;
    Thread* thread;
};
int ThreadControlButtons_last_id;
vector<ThreadControlButtons> thread_control_buttons;
CRITICAL_SECTION ThreadControlButtonsSection;

void refresh_thread_control_buttons() {
    int vpos = 150;
    for (auto iter : thread_control_buttons) {
        SetWindowPos(iter.Stop, NULL, 30, vpos, 30, 30, NULL);
        SetWindowPos(iter.Pause, NULL, 65, vpos, 30, 30, NULL);
        SetWindowPos(iter.PrDown, NULL, 100, vpos, 30, 30, NULL);
        SetWindowPos(iter.PrUp, NULL, 135, vpos, 30, 30, NULL);
        SetWindowPos(iter.PrVal, NULL, 175, vpos + 7, 100, 30, NULL);
        vpos += 40;
    }
}

#define THREAD_CONTROL_BUTTONS_BASE 100
HWND make_thread_control_button(LPCWSTR text, long long int callback_id) {
    return CreateWindow(L"BUTTON",
        text,
        WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
        0, 0, 30, 30,
        main_hwnd,
        (HMENU)(THREAD_CONTROL_BUTTONS_BASE + callback_id),
        (HINSTANCE)GetWindowLongPtr(main_hwnd, GWLP_HINSTANCE),
        NULL);
}

#define THREAD_CONTROL_STOP 0
#define THREAD_CONTROL_PAUSE 1
#define THREAD_CONTROL_PR_DOWN 2
#define THREAD_CONTROL_PR_UP 3
void add_thread_control_buttons(HANDLE thread) {
    EnterCriticalSection(&ThreadControlButtonsSection);
    for (auto iter = threads.begin(); iter != threads.end(); iter++)
        if ((*iter)->handle == thread) {
            ThreadControlButtons btn;
            btn.id = ThreadControlButtons_last_id++;
            btn.Stop = make_thread_control_button(L"X", btn.id * 4 + THREAD_CONTROL_STOP);
            btn.Pause = make_thread_control_button(L"=", btn.id * 4 + THREAD_CONTROL_PAUSE);
            btn.PrDown = make_thread_control_button(L"-", btn.id * 4 + THREAD_CONTROL_PR_DOWN);
            btn.PrUp = make_thread_control_button(L"+", btn.id * 4 + THREAD_CONTROL_PR_UP);
            btn.PrVal = CreateWindow(L"STATIC", L"0", WS_VISIBLE | WS_CHILD, 0, 0, 30, 30, main_hwnd,
                NULL, (HINSTANCE)GetWindowLongPtr(main_hwnd, GWLP_HINSTANCE), NULL);
            btn.Color = RGB(PIXCOMP((*iter)->color, 0), PIXCOMP((*iter)->color, 1), PIXCOMP((*iter)->color, 2));
            btn.thread = *iter;
            thread_control_buttons.insert(thread_control_buttons.begin(), btn);
        }
    LeaveCriticalSection(&ThreadControlButtonsSection);
    refresh_thread_control_buttons();
}

void remove_thread_control_buttons(HANDLE thread) {
    EnterCriticalSection(&ThreadControlButtonsSection);
    for (auto iter = thread_control_buttons.begin(); iter != thread_control_buttons.end(); iter++)
        if (iter->thread->handle == thread) {
            ThreadControlButtons btn = *iter;
            thread_control_buttons.erase(iter);
            LeaveCriticalSection(&ThreadControlButtonsSection);
            DestroyWindow(btn.Stop);
            DestroyWindow(btn.Pause);
            DestroyWindow(btn.PrDown);
            DestroyWindow(btn.PrUp);
            DestroyWindow(btn.PrVal);
            EnterCriticalSection(&ThreadControlButtonsSection);
            break;
        }
    LeaveCriticalSection(&ThreadControlButtonsSection);
    refresh_thread_control_buttons();
}

#define GRAYSCALE 0
#define NEGATIVE 1
#define BOX_BLUR 2
#define GAUSSIAN_BLUR 3
int operation;

uint32_t grayscale(int x, int y) {
    uint32_t p = PIX(original_pixels, x, y);
    double g = PIXCOMP(p, 0) * 0.3 + PIXCOMP(p, 1) * 0.59 + PIXCOMP(p, 2) * 0.11;
    return MAKEPIX(g, g, g);
}

uint32_t negative(int x, int y) {
    uint32_t p = PIX(original_pixels, x, y);
    return MAKEPIX(255 - PIXCOMP(p, 0), 255 - PIXCOMP(p, 1), 255 - PIXCOMP(p, 2));
}

uint32_t box_blur(int x, int y) {
    int ks = 3;
    double sR = 0.0, sG = 0.0, sB = 0.0;
    for (int yy = y - ks; yy <= y + ks; ++yy)
        for (int xx = x - ks; xx <= x + ks; ++xx) {
            int rx = min(max(0, xx), width - 1), ry = min(max(0, yy), height - 1);
            uint32_t p = PIX(original_pixels, rx, ry);
            int m = ks * 2 + 1; m *= m;
            sR += (double)PIXCOMP(p, 0) / m;
            sG += (double)PIXCOMP(p, 1) / m;
            sB += (double)PIXCOMP(p, 2) / m;
        }
    return MAKEPIX(sR, sG, sB);
}

uint32_t gaussian_blur(int x, int y) {
    int ks = 50;
    double sigma = 0.84089642 * 3.0;
    double sR = 0.0, sG = 0.0, sB = 0.0;
    for (int yy = y - ks; yy <= y + ks; ++yy)
        for (int xx = x - ks; xx <= x + ks; ++xx) {
            int rx = min(max(0, xx), width - 1), ry = min(max(0, yy), height - 1);
            uint32_t p = PIX(original_pixels, rx, ry);
            double m = exp(-((xx - x) * (xx - x) + (yy - y) * (yy - y)) / (2.0 * sigma * sigma)) / (2.0 * 3.1415926 * sigma * sigma);
            sR += PIXCOMP(p, 0) * m;
            sG += PIXCOMP(p, 1) * m;
            sB += PIXCOMP(p, 2) * m;
        }
    return MAKEPIX(int(sR), int(sG), int(sB));
}

#define NOTIFY_START 0
#define NOTIFY_STOP 1
#define NOTIFY_FINISHED 2
DWORD WINAPI thread_main(CONST LPVOID lpParam) {
    const int MIN_BLOCK_SIZE = 10;
    Thread* thread = (Thread*)lpParam;
    EnterCriticalSection(&GetNextLineSection);
    threads.push_back(thread);
    SendMessage(main_hwnd, WM_NOTIFY, (WPARAM)thread->handle, NOTIFY_START);
    refresh_state();
    while (true) {
        if (thread->stop) {
            free_intervals_pool.push_back(thread->interval);
            if (memory_monopoly_check_stop > 0) {
                --memory_monopoly_check_stop;
                WakeAllConditionVariable(&MonopolyBusyCond);
            }
            break;
        }

        if (memory_monopoly_check_stop > 0) {
            SleepConditionVariableCS(&MonopolyBusyCond, &GetNextLineSection, INFINITE);
            continue;
        }

        if (thread->interval.begin >= thread->interval.end) {
            if (free_intervals_pool.empty()) {
                int max_len = 0;
                auto max_iter = threads.begin();
                for (auto iter = threads.begin(); iter != threads.end(); ++iter) {
                    int cur_len = (*iter)->interval.end - (*iter)->interval.begin;
                    if (cur_len > max_len) {
                        max_len = cur_len;
                        max_iter = iter;
                    }
                }

                if (max_len <= MIN_BLOCK_SIZE) {
                    if (max_len == 0 && threads.size() == 1)
                        finished = true;
                    break;
                }
                thread->interval.end = (*max_iter)->interval.end;
                (*max_iter)->interval.end -= min(max(0, max_len - MIN_BLOCK_SIZE), max_len / 2);
                thread->interval.begin = (*max_iter)->interval.end;
            }
            else {
                thread->interval = free_intervals_pool.back();
                free_intervals_pool.pop_back();
            }
            draw_schedule(thread->interval, thread->color);
        }

        if (pause) {
            SleepConditionVariableCS(&PauseCond, &GetNextLineSection, INFINITE);
            continue;
        }

        if (thread->pause) {
            LeaveCriticalSection(&GetNextLineSection);
            SwitchToThread();
            EnterCriticalSection(&GetNextLineSection);
            continue;
        }

        int y = thread->interval.begin++;
        bool monopoly = memory_monopoly;
        LeaveCriticalSection(&GetNextLineSection);

        if (monopoly) EnterCriticalSection(&MemoryMonopolySection);
        if (memory_monopoly_check_stop > 0)
            --thread->interval.begin;
        else for (int x = 0; x < width; ++x)
        {
            PIX(completed_pixels, x, y) = thread->color;
            PIX(schedule_pixels, x, y) = 0;
            uint32_t pix = 0;
            switch (operation) {
            case GRAYSCALE: pix = grayscale(x, y); break;
            case NEGATIVE: pix = negative(x, y); break;
            case BOX_BLUR: pix = box_blur(x, y); break;
            case GAUSSIAN_BLUR: pix = gaussian_blur(x, y); break;
            }
            PIX(result_pixels, x, y) = pix;
        }

        if (monopoly) LeaveCriticalSection(&MemoryMonopolySection);
        EnterCriticalSection(&GetNextLineSection);
    }
    draw_schedule(thread->interval, 0xFFFFFF);
    thread->interval.end = thread->interval.begin;
    for (auto iter = threads.begin(); iter != threads.end(); ++iter)
        if ((*iter)->handle == thread->handle) {
            threads.erase(iter);
            break;
        }
    CloseHandle(thread->handle);
    bool done = finished && threads.empty();
    LeaveCriticalSection(&GetNextLineSection);
    refresh_state();
    SendMessage(main_hwnd, WM_NOTIFY, (WPARAM)thread->handle, NOTIFY_STOP);
    if (done)
        SendMessage(main_hwnd, WM_NOTIFY, (WPARAM)thread->handle, NOTIFY_FINISHED);
    delete thread;
    return 0;
}

void set_threads_count(int new_count) {
    EnterCriticalSection(&GetNextLineSection);
    new_count = min(new_count, THREADS_MAX);
    size_t threads_count = threads.size();
    for (size_t c = threads_count; c < new_count; ++c) {
        Thread* new_thread = new Thread;
        new_thread->interval.begin = new_thread->interval.end = 0;
        new_thread->stop = new_thread->pause = false;
        new_thread->color = thread_colors[c];
        if ((new_thread->handle = CreateThread(NULL, NULL, &thread_main, new_thread, NULL, NULL)) == NULL) {
            MessageBox(main_hwnd, L"Error creating thread!",
                L"Error", MB_OK | MB_ICONERROR);
            LeaveCriticalSection(&GetNextLineSection);
            return;
        }
    }

    memory_monopoly_check_stop = 0;
    new_count = max(new_count, 0);
    for (size_t c = threads_count; c > new_count; --c, ++memory_monopoly_check_stop)
        threads[c - 1]->stop = true;
    if (!memory_monopoly) memory_monopoly_check_stop = 0;
    LeaveCriticalSection(&GetNextLineSection);

    WakeAllConditionVariable(&PauseCond);

    while (threads_count > new_count) {
        EnterCriticalSection(&GetNextLineSection);
        threads_count = (int)threads.size();
        LeaveCriticalSection(&GetNextLineSection);
        Sleep(10);
    }
}

void resize_client_area(HWND hwnd, int x, int y, int height, int width) {
    RECT window_rect, client_rect;
    GetWindowRect(hwnd, &window_rect);
    GetClientRect(hwnd, &client_rect);
    SetWindowPos(hwnd, NULL, x, y,
        height + (window_rect.right - window_rect.left) - (client_rect.right - client_rect.left),
        width + (window_rect.bottom - window_rect.top) - (client_rect.bottom - client_rect.top),
        NULL);
}

void reset_windows_pos() {
    resize_client_area(original_hwnd, 5, 5, width, height);
    resize_client_area(schedule_hwnd, 5, 40 + height, width, height);
    resize_client_area(completed_hwnd, 10 + width, 40 + height, width, height);
    resize_client_area(result_hwnd, 10 + width, 5, width, height);
}

#define OPEN_BUTTON 1
#define CLOSE_BUTTON 2
#define SAVE_BUTTON 3
#define START_GRAYSCALE_BUTTON 4
#define START_NEGATIVE_BUTTON 5
#define START_BOX_BLUR_BUTTON 6
#define START_GAUSSIAN_BLUR_BUTTON 7
#define CANCEL_OPERATION_BUTTON 8
#define ADD_THREAD_BUTTON 9
#define REMOVE_THREAD_BUTTON 10
#define TOGGLE_PAUSE_BUTTON 11
#define MEMORY_MONOPOLY_CHECKBOX 12
#define RESET_WINDOWS_POS_BUTTON 13

void cancel_operation() {
    if (!opened) return;
    if (working) {
        LeaveCriticalSection(&GetNextLineSection);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                PIX(schedule_pixels, x, y) = PIX(completed_pixels, x, y) = 0;
        set_threads_count(0);
        free_intervals_pool.clear();
        working = false;
    }
    else finished = false;
    pause = false;
    refresh_state();
}

void start_operation(int op) {
    if (opened) {
        if (working)
            cancel_operation();
        operation = op;
        working = true;
        finished = false;
        pause = false;
        memory_monopoly = false;
        memory_monopoly_check_stop = 0;
        ThreadControlButtons_last_id = 0;
        SendMessage(memory_monopoly_checkbox_hwnd, BM_SETCHECK, BST_UNCHECKED, 0);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                PIX(schedule_pixels, x, y) = PIX(completed_pixels, x, y) = PIX(result_pixels, x, y) = 0;
        /*ShowWindow(schedule_hwnd, SW_SHOW);
        ShowWindow(completed_hwnd, SW_SHOW);
        ShowWindow(result_hwnd, SW_SHOW);
        ShowWindow(remove_thread_button_hwnd, SW_SHOW);
        ShowWindow(add_thread_button_hwnd, SW_SHOW);
        ShowWindow(threads_count_hwnd, SW_SHOW);
        ShowWindow(toggle_pause_button_hwnd, SW_SHOW);
        ShowWindow(cancel_operation_button_hwnd, SW_SHOW);*/
        refresh_state();
        free_intervals_pool.push_back(LineInterval{ 0, height });
        SetTimer(main_hwnd, 0, 17, NULL);
        set_threads_count(4);
    }
}

void close_ppm() {
    if (!opened) return;
    cancel_operation();
    finished = false;
    opened = false;
    refresh_state();

    VirtualFree(original_pixels, 0, MEM_RELEASE);
    VirtualFree(result_pixels, 0, MEM_RELEASE);
    VirtualFree(schedule_pixels, 0, MEM_RELEASE);
    VirtualFree(completed_pixels, 0, MEM_RELEASE);
}

PWSTR open_file_dialog(COMDLG_FILTERSPEC* file_types, int file_types_count) {
    PWSTR pszFilePath = NULL;
    IFileOpenDialog* pFileOpen;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL,
        IID_IFileOpenDialog, reinterpret_cast<void**>(&pFileOpen));
    if (!SUCCEEDED(hr)) return NULL;
    hr = pFileOpen->SetFileTypes(file_types_count, file_types);
    if (!SUCCEEDED(hr)) return NULL;
    hr = pFileOpen->Show(NULL);
    if (!SUCCEEDED(hr)) return NULL;
    IShellItem* pItem;
    hr = pFileOpen->GetResult(&pItem);
    if (!SUCCEEDED(hr)) return NULL;
    hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);
    pItem->Release();
    pFileOpen->Release();
    return pszFilePath;
}

PWSTR save_file_dialog(COMDLG_FILTERSPEC* file_types, int file_types_count) {
    PWSTR pszFilePath = NULL;
    IFileSaveDialog* pFileSave;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL,
        IID_IFileOpenDialog, reinterpret_cast<void**>(&pFileSave));
    if (!SUCCEEDED(hr)) return NULL;
    hr = pFileSave->SetFileTypes(file_types_count, file_types);
    if (!SUCCEEDED(hr)) return NULL;
    DWORD dwFlags;
    hr = pFileSave->GetOptions(&dwFlags);
    if (!SUCCEEDED(hr)) return NULL;
    hr = pFileSave->SetOptions(dwFlags & ~FOS_FILEMUSTEXIST);
    if (!SUCCEEDED(hr)) return NULL;
    hr = pFileSave->SetDefaultExtension(L"ppm");
    if (!SUCCEEDED(hr)) return NULL;
    hr = pFileSave->Show(NULL);
    if (!SUCCEEDED(hr)) return NULL;
    IShellItem* pItem;
    hr = pFileSave->GetResult(&pItem);
    if (!SUCCEEDED(hr)) return NULL;
    hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);
    if (!SUCCEEDED(hr)) return NULL;
    pItem->Release();
    pFileSave->Release();
    return pszFilePath;
}

void open_ppm() {
    if (working) return;
    close_ppm();

    COMDLG_FILTERSPEC file_types[] = { { L"PPM-зображення", L"*.ppm" } };
    LPWSTR file_path = open_file_dialog(file_types, _countof(file_types));
    if (file_path == NULL) return;

    ifstream file(file_path, ios_base::in | ios_base::binary);
    if (!file.is_open()) {
        MessageBox(main_hwnd, L"Error opening file!",
            L"Error", MB_OK | MB_ICONERROR);
        return;
    }

    if (opened) close_ppm();

    string _tmp;
    getline(file, _tmp);
    if (_tmp.compare("P6")) {
        MessageBox(main_hwnd, L"PPM format signature missing!",
            L"Error", MB_OK | MB_ICONERROR);
        return;
    }

    file >> width >> height >> colors;
    if (colors != 255) {
        MessageBox(main_hwnd, L"Only 255-colors PPM are supported!",
            L"Error", MB_OK | MB_ICONERROR);
        return;
    }

    long long int memory_size = width * height * 4;
    original_pixels = (uint32_t*)VirtualAlloc(NULL, memory_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    result_pixels = (uint32_t*)VirtualAlloc(NULL, memory_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    schedule_pixels = (uint32_t*)VirtualAlloc(NULL, memory_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    completed_pixels = (uint32_t*)VirtualAlloc(NULL, memory_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

    file.get();
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            unsigned char R = file.get();
            unsigned char G = file.get();
            unsigned char B = file.get();
            PIX(original_pixels, x, y) = (R << 16) | (G << 8) | B;
        }
    }
    file.close();

    bitmapinfo.bmiHeader.biSize = sizeof(bitmapinfo.bmiHeader);
    bitmapinfo.bmiHeader.biWidth = width;
    bitmapinfo.bmiHeader.biHeight = height;
    bitmapinfo.bmiHeader.biPlanes = 1;
    bitmapinfo.bmiHeader.biBitCount = 32;
    bitmapinfo.bmiHeader.biCompression = BI_RGB;

    opened = true;
    working = finished = false;
    reset_windows_pos();
    refresh_state();
}

void save_ppm() {
    if (!finished) return;

    COMDLG_FILTERSPEC file_types[] = { { L"PPM-зображення", L"*.ppm" } };
    LPWSTR file_path = save_file_dialog(file_types, _countof(file_types));
    if (file_path == NULL) return;

    ofstream file(file_path, ios_base::out | ios_base::binary);

    if (!file.is_open()) {
        MessageBox(main_hwnd, L"Error opening file!",
            L"Error", MB_OK | MB_ICONERROR);
        return;
    }

    file << "P6\n" << width << " " << height << "\n" << colors << "\n";

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            unsigned char R = PIXCOMP(PIX(result_pixels, x, y), 0);
            unsigned char G = PIXCOMP(PIX(result_pixels, x, y), 1);
            unsigned char B = PIXCOMP(PIX(result_pixels, x, y), 2);
            file.put(R); file.put(G); file.put(B);
        }
    }

    file.close();
}

void tweak_thread_priority(HANDLE thread, HWND PrVal_hwnd, int inc) {
    int pr_val = GetThreadPriority(thread) + inc;
    if (pr_val < -2) {
        if (pr_val == -14) pr_val = -2;
        else pr_val = -15;
    }
    else if (pr_val > 2) {
        if (pr_val == 14) pr_val = 2;
        else pr_val = 15;
    }
    SetThreadPriority(thread, pr_val);
}

void handle_thread_control_button(int callback_id) {
    int buttons_id = callback_id / 4, action_id = callback_id % 4;
    EnterCriticalSection(&GetNextLineSection);
    for (auto iter = thread_control_buttons.begin(); iter != thread_control_buttons.end(); iter++)
        if (iter->id == buttons_id) {
            switch (action_id) {
            case THREAD_CONTROL_STOP:
                iter->thread->stop = true;
                ++memory_monopoly_check_stop;
                break;
            case THREAD_CONTROL_PAUSE: iter->thread->pause = !iter->thread->pause; break;
            case THREAD_CONTROL_PR_DOWN: tweak_thread_priority(iter->thread->handle, iter->PrVal, -1); break;
            case THREAD_CONTROL_PR_UP: tweak_thread_priority(iter->thread->handle, iter->PrVal, 1); break;
            }
            wchar_t pr_val_str[10];
            wsprintf(pr_val_str, L"%d%s", GetThreadPriority(iter->thread->handle), iter->thread->pause ? L" ПАУЗА" : L"");
            SetWindowText(iter->PrVal, pr_val_str);
            if (action_id == THREAD_CONTROL_STOP)
                WakeAllConditionVariable(&PauseCond);
            break;
        }
    LeaveCriticalSection(&GetNextLineSection);
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        original_hwnd = CreateWindowW(
            MAKEINTATOM(original_wClass),
            L"Вхідне зображення",
            NULL,
            CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
            main_hwnd,
            NULL,
            GetModuleHandle(NULL),
            NULL
        );
        if (!original_hwnd)
            return 1;

        schedule_hwnd = CreateWindowW(
            MAKEINTATOM(schedule_wClass),
            L"План обчислення",
            NULL,
            CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
            main_hwnd,
            NULL,
            GetModuleHandle(NULL),
            NULL
        );
        if (!schedule_hwnd)
            return 1;

        completed_hwnd = CreateWindowW(
            MAKEINTATOM(completed_wClass),
            L"Обчислено",
            NULL,
            CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
            main_hwnd,
            NULL,
            GetModuleHandle(NULL),
            NULL
        );
        if (!completed_hwnd)
            return 1;

        result_hwnd = CreateWindowW(
            MAKEINTATOM(result_wClass),
            L"Результат",
            NULL,
            CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
            main_hwnd,
            NULL,
            GetModuleHandle(NULL),
            NULL
        );
        if (!result_hwnd)
            return 1;

        refresh_state();
        return 0;
    }
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: {
        DestroyWindow(original_hwnd);
        DestroyWindow(schedule_hwnd);
        DestroyWindow(completed_hwnd);
        DestroyWindow(result_hwnd);
        PostQuitMessage(0);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        FillRect(hdc, &ps.rcPaint, (HBRUSH)(COLOR_WINDOW));
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_COMMAND: {
        switch (wParam)
        {
        case OPEN_BUTTON: open_ppm(); break;
        case CLOSE_BUTTON: close_ppm(); break;
        case SAVE_BUTTON: save_ppm(); break;
        case START_GRAYSCALE_BUTTON: start_operation(GRAYSCALE); break;
        case START_NEGATIVE_BUTTON: start_operation(NEGATIVE); break;
        case START_BOX_BLUR_BUTTON: start_operation(BOX_BLUR); break;
        case START_GAUSSIAN_BLUR_BUTTON: start_operation(GAUSSIAN_BLUR); break;
        case CANCEL_OPERATION_BUTTON: cancel_operation(); break;
        case ADD_THREAD_BUTTON: {
            if (opened && working && !finished) {
                set_threads_count((int)threads.size() + 1);
            }
            break;
        }
        case REMOVE_THREAD_BUTTON: {
            if (opened && working && !finished) {
                set_threads_count((int)threads.size() - 1);
            }
            break;
        }
        case TOGGLE_PAUSE_BUTTON: {
            pause = !pause;
            if (!pause)
                WakeAllConditionVariable(&PauseCond);
            refresh_state();
            break;
        }
        case MEMORY_MONOPOLY_CHECKBOX: {
            EnterCriticalSection(&GetNextLineSection);
            memory_monopoly = SendMessage(memory_monopoly_checkbox_hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
            LeaveCriticalSection(&GetNextLineSection);
        }
        case RESET_WINDOWS_POS_BUTTON: reset_windows_pos(); break;
        default:
            if (wParam >= THREAD_CONTROL_BUTTONS_BASE)
                handle_thread_control_button((int)wParam - THREAD_CONTROL_BUTTONS_BASE);
            break;
        }
    }
    case WM_TIMER: {
        InvalidateRect(completed_hwnd, NULL, false);
        InvalidateRect(schedule_hwnd, NULL, false);
        InvalidateRect(result_hwnd, NULL, false);
        break;
    }
    case WM_NOTIFY:
        switch (lParam) {
        case NOTIFY_START: add_thread_control_buttons((HANDLE)wParam); break;
        case NOTIFY_STOP: remove_thread_control_buttons((HANDLE)wParam); break;
        case NOTIFY_FINISHED: cancel_operation(); break;
        };
        break;
    case WM_CTLCOLORSTATIC:
        for (auto iter = thread_control_buttons.begin(); iter != thread_control_buttons.end(); iter++)
            if ((HWND)lParam == (*iter).PrVal) {
                SetBkMode((HDC)wParam, TRANSPARENT);
                SetTextColor((HDC)wParam, (*iter).Color);
                return (LRESULT)GetSysColorBrush(COLOR_MENU);
            }
        break;
    };
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void PaintMemory(HWND hwnd, uint32_t* memory) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    StretchDIBits(hdc, 0, 0, width, height, 0, 0, width, height, memory, &bitmapinfo, DIB_RGB_COLORS, SRCCOPY);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK OriginalWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg)
    {
    case WM_KEYDOWN: break;
    case WM_CLOSE:   DestroyWindow(main_hwnd); return 0;
    case WM_DESTROY: PostQuitMessage(0);  return 0;
    case WM_PAINT:   PaintMemory(hwnd, original_pixels);   return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}
LRESULT CALLBACK ScheduleWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg)
    {
    case WM_KEYDOWN: break;
    case WM_CLOSE:   DestroyWindow(main_hwnd); return 0;
    case WM_DESTROY: PostQuitMessage(0);  return 0;
    case WM_PAINT:   PaintMemory(hwnd, schedule_pixels);   return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}
LRESULT CALLBACK CompletedWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg)
    {
    case WM_KEYDOWN: break;
    case WM_CLOSE:   DestroyWindow(main_hwnd); return 0;
    case WM_DESTROY: PostQuitMessage(0);  return 0;
    case WM_PAINT:   PaintMemory(hwnd, completed_pixels);   return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}
LRESULT CALLBACK ResultWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg)
    {
    case WM_KEYDOWN: break;
    case WM_CLOSE:   DestroyWindow(main_hwnd); return 0;
    case WM_DESTROY: PostQuitMessage(0);  return 0;
    case WM_PAINT:   PaintMemory(hwnd, result_pixels);   return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR    lpCmdLine,
    _In_ int       nCmdShow)
{
    main_wc = {
      0, MainWndProc, 0, 0, 0,
      LoadIcon(NULL, IDI_APPLICATION),
      LoadCursor(NULL, IDC_ARROW),
      NULL,
      NULL,
      L"MainWindowClass"
    };
    main_wClass = RegisterClass(&main_wc);
    if (!main_wClass)
        return 1;

    original_wc = {
      0, OriginalWndProc, 0, 0, 0,
      LoadIcon(NULL, IDI_APPLICATION),
      LoadCursor(NULL, IDC_ARROW),
      NULL,
      NULL,
      L"OriginalWindowClass"
    };
    original_wClass = RegisterClass(&original_wc);
    if (!original_wClass)
        return 1;

    schedule_wc = {
      0, ScheduleWndProc, 0, 0, 0,
      LoadIcon(NULL, IDI_APPLICATION),
      LoadCursor(NULL, IDC_ARROW),
      NULL,
      NULL,
      L"SchedulelWindowClass"
    };
    schedule_wClass = RegisterClass(&schedule_wc);
    if (!schedule_wClass)
        return 1;

    completed_wc = {
      0, CompletedWndProc, 0, 0, 0,
      LoadIcon(NULL, IDI_APPLICATION),
      LoadCursor(NULL, IDC_ARROW),
      NULL,
      NULL,
      L"CompletedWindowClass"
    };
    completed_wClass = RegisterClass(&completed_wc);
    if (!completed_wClass)
        return 1;

    result_wc = {
      0, ResultWndProc, 0, 0, 0,
      LoadIcon(NULL, IDI_APPLICATION),
      LoadCursor(NULL, IDC_ARROW),
      NULL,
      NULL,
      L"ResultWindowClass"
    };
    result_wClass = RegisterClass(&result_wc);
    if (!result_wClass)
        return 1;

    main_hwnd = CreateWindow(
        MAKEINTATOM(main_wClass),
        L"Лабораторна робота 1",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 625, 550,
        NULL,
        NULL,
        GetModuleHandle(NULL),
        NULL
    );
    if (!main_hwnd)
        return 1;

    open_button_hwnd = CreateWindow(
        L"BUTTON",
        L"Відкрити",
        WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
        10,
        10,
        90,
        30,
        main_hwnd,
        (HMENU)OPEN_BUTTON,
        (HINSTANCE)GetWindowLongPtr(main_hwnd, GWLP_HINSTANCE),
        NULL);

    close_button_hwnd = CreateWindow(
        L"BUTTON",
        L"Закрити",
        WS_TABSTOP | WS_CHILD | BS_DEFPUSHBUTTON,
        110,
        10,
        90,
        30,
        main_hwnd,
        (HMENU)CLOSE_BUTTON,
        (HINSTANCE)GetWindowLongPtr(main_hwnd, GWLP_HINSTANCE),
        NULL);

    save_button_hwnd = CreateWindow(
        L"BUTTON",
        L"Зберегти",
        WS_TABSTOP | WS_CHILD | BS_DEFPUSHBUTTON,
        210,
        10,
        90,
        30,
        main_hwnd,
        (HMENU)SAVE_BUTTON,
        (HINSTANCE)GetWindowLongPtr(main_hwnd, GWLP_HINSTANCE),
        NULL);

    start_grayscale_button_hwnd = CreateWindow(
        L"BUTTON",
        L"Чорно-біле",
        WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON | WS_DISABLED,
        10,
        50,
        90,
        30,
        main_hwnd,
        (HMENU)START_GRAYSCALE_BUTTON,
        (HINSTANCE)GetWindowLongPtr(main_hwnd, GWLP_HINSTANCE),
        NULL);

    start_negative_button_hwnd = CreateWindow(
        L"BUTTON",
        L"Негатив",
        WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON | WS_DISABLED,
        110,
        50,
        70,
        30,
        main_hwnd,
        (HMENU)START_NEGATIVE_BUTTON,
        (HINSTANCE)GetWindowLongPtr(main_hwnd, GWLP_HINSTANCE),
        NULL);

    start_box_blur_button_hwnd = CreateWindow(
        L"BUTTON",
        L"Квадратне розмиття",
        WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON | WS_DISABLED,
        190,
        50,
        150,
        30,
        main_hwnd,
        (HMENU)START_BOX_BLUR_BUTTON,
        (HINSTANCE)GetWindowLongPtr(main_hwnd, GWLP_HINSTANCE),
        NULL);

    start_gaussian_blur_button_hwnd = CreateWindow(
        L"BUTTON",
        L"Гауссове розмиття",
        WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON | WS_DISABLED,
        350,
        50,
        150,
        30,
        main_hwnd,
        (HMENU)START_GAUSSIAN_BLUR_BUTTON,
        (HINSTANCE)GetWindowLongPtr(main_hwnd, GWLP_HINSTANCE),
        NULL);

    cancel_operation_button_hwnd = CreateWindow(
        L"BUTTON",
        L"Стоп",
        WS_TABSTOP | WS_CHILD | BS_DEFPUSHBUTTON,
        510,
        50,
        90,
        30,
        main_hwnd,
        (HMENU)CANCEL_OPERATION_BUTTON,
        (HINSTANCE)GetWindowLongPtr(main_hwnd, GWLP_HINSTANCE),
        NULL);

    remove_thread_button_hwnd = CreateWindow(
        L"BUTTON",
        L"-",
        WS_TABSTOP | WS_CHILD | BS_DEFPUSHBUTTON,
        10,
        100,
        30,
        30,
        main_hwnd,
        (HMENU)REMOVE_THREAD_BUTTON,
        (HINSTANCE)GetWindowLongPtr(main_hwnd, GWLP_HINSTANCE),
        NULL);

    threads_count_hwnd = CreateWindow(
        L"STATIC",
        L"",
        WS_VISIBLE | WS_CHILD,
        50,
        107,
        150,
        30,
        main_hwnd,
        NULL,
        (HINSTANCE)GetWindowLongPtr(main_hwnd, GWLP_HINSTANCE),
        NULL
    );

    add_thread_button_hwnd = CreateWindow(
        L"BUTTON",
        L"+",
        WS_TABSTOP | WS_CHILD | BS_DEFPUSHBUTTON,
        200,
        100,
        30,
        30,
        main_hwnd,
        (HMENU)ADD_THREAD_BUTTON,
        (HINSTANCE)GetWindowLongPtr(main_hwnd, GWLP_HINSTANCE),
        NULL);

    toggle_pause_button_hwnd = CreateWindow(
        L"BUTTON",
        L"Пауза",
        WS_TABSTOP | WS_CHILD | BS_DEFPUSHBUTTON,
        250,
        100,
        70,
        30,
        main_hwnd,
        (HMENU)TOGGLE_PAUSE_BUTTON,
        (HINSTANCE)GetWindowLongPtr(main_hwnd, GWLP_HINSTANCE),
        NULL);

    memory_monopoly_checkbox_hwnd = CreateWindow(
        L"BUTTON",
        L"Монопольний доступ",
        WS_TABSTOP | WS_CHILD | BS_AUTOCHECKBOX,
        330,
        100,
        200,
        30,
        main_hwnd,
        (HMENU)MEMORY_MONOPOLY_CHECKBOX,
        (HINSTANCE)GetWindowLongPtr(main_hwnd, GWLP_HINSTANCE),
        NULL);

    reset_windows_pos_button_hwnd = CreateWindow(
        L"BUTTON",
        L"Відновити положення вікон",
        WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
        400,
        10,
        200,
        30,
        main_hwnd,
        (HMENU)RESET_WINDOWS_POS_BUTTON,
        (HINSTANCE)GetWindowLongPtr(main_hwnd, GWLP_HINSTANCE),
        NULL);

    InitializeCriticalSection(&GetNextLineSection);
    InitializeConditionVariable(&PauseCond);
    InitializeCriticalSection(&MemoryMonopolySection);
    InitializeConditionVariable(&MonopolyBusyCond);
    InitializeCriticalSection(&ThreadControlButtonsSection);

    ShowWindow(main_hwnd, SW_SHOWNORMAL);

    MSG msg;
    BOOL bRet;
    while ((bRet = GetMessage(&msg, NULL, 0, 0) > 0) != 0)
    {
        if (bRet == -1) break;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}