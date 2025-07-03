// Full working MP3 player with fixed next and remove behavior
#include <stdio.h>
#include <windows.h>
#include <commdlg.h>
#include <mpg123.h>
#include <portaudio.h>
#include <string.h>
#include <stdlib.h>

#define ID_BUTTON_OPEN   1
#define ID_BUTTON_PAUSE  2
#define ID_BUTTON_NEXT   3
#define ID_BUTTON_REMOVE 4
#define ID_LISTBOX_QUEUE 5
#define FRAMES_PER_BUFFER 4096

HWND hwndMain, hwndPauseBtn, hwndNextBtn, hwndRemoveBtn, hwndListBox;
HANDLE playThread = NULL;
PaStream *stream = NULL;

volatile int isPlaying = 0;
volatile int isPaused = 0;
volatile int stopPlayback = 0;
volatile int skipToNext = 0;
volatile int nextPressed = 0; // Added: distinguish next button from remove

size_t currentTrackIndex = 0;

typedef struct {
    char **files;
    size_t count;
    size_t capacity;
} Playlist;

Playlist playlist = { NULL, 0, 0 };
CRITICAL_SECTION playlistLock;

void AddToPlaylist(const char *filename) {
    EnterCriticalSection(&playlistLock);
    if (playlist.count == playlist.capacity) {
        size_t newCap = playlist.capacity ? playlist.capacity * 2 : 4;
        char **newFiles = realloc(playlist.files, newCap * sizeof(char *));
        if (!newFiles) {
            LeaveCriticalSection(&playlistLock);
            MessageBox(hwndMain, "Failed to allocate playlist memory", "Error", MB_OK | MB_ICONERROR);
            return;
        }
        playlist.files = newFiles;
        playlist.capacity = newCap;
    }
    playlist.files[playlist.count] = _strdup(filename);
    SendMessage(hwndListBox, LB_ADDSTRING, 0, (LPARAM)filename);
    playlist.count++;
    LeaveCriticalSection(&playlistLock);
}

void RemoveFromPlaylist(size_t index) {
    EnterCriticalSection(&playlistLock);
    if (index >= playlist.count) {
        LeaveCriticalSection(&playlistLock);
        return;
    }
    free(playlist.files[index]);
    for (size_t i = index; i < playlist.count - 1; ++i) {
        playlist.files[i] = playlist.files[i + 1];
    }
    playlist.count--;
    SendMessage(hwndListBox, LB_DELETESTRING, index, 0);

    if (index == currentTrackIndex) {
        skipToNext = 1;
        nextPressed = 0;  // Remove: do NOT advance index here, playlist shifted left
    } else if (index < currentTrackIndex && currentTrackIndex > 0) {
        currentTrackIndex--;
    }
    LeaveCriticalSection(&playlistLock);
}

void FreePlaylist() {
    EnterCriticalSection(&playlistLock);
    for (size_t i = 0; i < playlist.count; i++) {
        free(playlist.files[i]);
    }
    free(playlist.files);
    playlist.files = NULL;
    playlist.count = 0;
    playlist.capacity = 0;
    LeaveCriticalSection(&playlistLock);
}

DWORD WINAPI PlayMP3Queue(LPVOID lpParam) {
    (void)lpParam;

    mpg123_init();
    mpg123_handle *mh = mpg123_new(NULL, NULL);
    if (!mh) return 0;
    mpg123_format_all(mh);

    PaError err = Pa_Initialize();
    if (err != paNoError) return 0;

    unsigned char *buffer = malloc(FRAMES_PER_BUFFER * 2 * sizeof(short) * 2);
    if (!buffer) return 0;

    isPlaying = 1;
    isPaused = 0;
    stopPlayback = 0;

    while (!stopPlayback) {
        EnterCriticalSection(&playlistLock);
        if (playlist.count == 0) {
            LeaveCriticalSection(&playlistLock);
            Sleep(100);
            continue;
        }
        if (currentTrackIndex >= playlist.count) currentTrackIndex = 0;
        char *file = _strdup(playlist.files[currentTrackIndex]);
        LeaveCriticalSection(&playlistLock);

        if (mpg123_open(mh, file) != MPG123_OK) {
            free(file);
            EnterCriticalSection(&playlistLock);
            currentTrackIndex = (currentTrackIndex + 1) % playlist.count;
            LeaveCriticalSection(&playlistLock);
            continue;
        }

        long rate;
        int channels, encoding;
        if (mpg123_getformat(mh, &rate, &channels, &encoding) != MPG123_OK || encoding != MPG123_ENC_SIGNED_16) {
            mpg123_close(mh);
            free(file);
            EnterCriticalSection(&playlistLock);
            currentTrackIndex = (currentTrackIndex + 1) % playlist.count;
            LeaveCriticalSection(&playlistLock);
            continue;
        }

        if (stream) {
            Pa_StopStream(stream);
            Pa_CloseStream(stream);
            stream = NULL;
        }
        err = Pa_OpenDefaultStream(&stream, 0, channels, paInt16, rate,
                                   FRAMES_PER_BUFFER, NULL, NULL);
        if (err != paNoError) {
            mpg123_close(mh);
            free(file);
            EnterCriticalSection(&playlistLock);
            currentTrackIndex = (currentTrackIndex + 1) % playlist.count;
            LeaveCriticalSection(&playlistLock);
            continue;
        }
        Pa_StartStream(stream);

        char nowPlaying[512];
        snprintf(nowPlaying, sizeof(nowPlaying), "Playing: %s", file);
        SetWindowText(hwndMain, nowPlaying);

        size_t done;
        int ret;
        skipToNext = 0;

        while (!skipToNext && !stopPlayback && (ret = mpg123_read(mh, buffer, FRAMES_PER_BUFFER * channels * sizeof(short), &done)) == MPG123_OK) {
            while (isPaused && !stopPlayback) Sleep(100);
            if (done > 0 && !stopPlayback) {
                size_t samples = done / (sizeof(short) * channels);
                err = Pa_WriteStream(stream, buffer, samples);
                if (err != paNoError) break;
            }
        }

        mpg123_close(mh);
        free(file);

        EnterCriticalSection(&playlistLock);
        if (playlist.count > 0) {
            if (nextPressed) {
                currentTrackIndex = (currentTrackIndex + 1) % playlist.count;
                nextPressed = 0;
            } else if (!skipToNext) {
                currentTrackIndex = (currentTrackIndex + 1) % playlist.count;
            }
            // if skipToNext and nextPressed == 0 (remove case), do NOT advance index here
        } else {
            currentTrackIndex = 0; // reset if playlist empty
        }
        LeaveCriticalSection(&playlistLock);
    }

    free(buffer);
    Pa_Terminate();
    mpg123_delete(mh);
    mpg123_exit();

    isPlaying = 0;
    isPaused = 0;
    stopPlayback = 0;

    SetWindowText(hwndMain, "MP3 Player");
    return 0;
}

void OpenFileDialogAndAddFiles(HWND hwnd) {
    static char filesBuffer[8192];
    OPENFILENAME ofn = { sizeof(ofn) };
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = "MP3 Files\0*.mp3\0All Files\0*.*\0";
    ofn.lpstrFile = filesBuffer;
    ofn.nMaxFile = sizeof(filesBuffer);
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST;

    if (GetOpenFileName(&ofn)) {
        char *ptr = filesBuffer;
        char directory[MAX_PATH];
        strcpy(directory, ptr);
        ptr += strlen(ptr) + 1;

        if (*ptr == 0) {
            AddToPlaylist(directory);
        } else {
            while (*ptr) {
                char fullpath[MAX_PATH];
                snprintf(fullpath, sizeof(fullpath), "%s\\%s", directory, ptr);
                AddToPlaylist(fullpath);
                ptr += strlen(ptr) + 1;
            }
        }

        if (!isPlaying) {
            stopPlayback = 0;
            currentTrackIndex = 0;
            playThread = CreateThread(NULL, 0, PlayMP3Queue, NULL, 0, NULL);
            EnableWindow(hwndPauseBtn, TRUE);
            SetWindowText(hwndPauseBtn, "Pause");
        }
    }
}

void TogglePause() {
    if (!isPlaying) return;
    isPaused = !isPaused;
    SetWindowText(hwndPauseBtn, isPaused ? "Resume" : "Pause");
}

void SkipToNext() {
    if (isPlaying) {
        skipToNext = 1;
        nextPressed = 1;
    }
}

void RemoveSelectedFromQueue() {
    int sel = SendMessage(hwndListBox, LB_GETCURSEL, 0, 0);
    if (sel != LB_ERR) RemoveFromPlaylist((size_t)sel);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_BUTTON_OPEN: OpenFileDialogAndAddFiles(hwnd); break;
                case ID_BUTTON_PAUSE: TogglePause(); break;
                case ID_BUTTON_NEXT: SkipToNext(); break;
                case ID_BUTTON_REMOVE: RemoveSelectedFromQueue(); break;
            }
            break;
        case WM_CLOSE:
            stopPlayback = 1;
            if (playThread) {
                WaitForSingleObject(playThread, 3000);
                CloseHandle(playThread);
                playThread = NULL;
            }
            DestroyWindow(hwnd);
            break;
        case WM_DESTROY:
            FreePlaylist();
            DeleteCriticalSection(&playlistLock);
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nCmdShow) {
    InitializeCriticalSection(&playlistLock);

    WNDCLASS wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "MP3Window";
    RegisterClass(&wc);

    hwndMain = CreateWindow(wc.lpszClassName, "MP3 Player",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            CW_USEDEFAULT, CW_USEDEFAULT, 400, 300,
                            NULL, NULL, hInst, NULL);

    CreateWindow("BUTTON", "Open", WS_VISIBLE | WS_CHILD,
                 10, 10, 80, 30, hwndMain, (HMENU)ID_BUTTON_OPEN, hInst, NULL);

    hwndPauseBtn = CreateWindow("BUTTON", "Pause", WS_VISIBLE | WS_CHILD | WS_DISABLED,
                                100, 10, 80, 30, hwndMain, (HMENU)ID_BUTTON_PAUSE, hInst, NULL);

    hwndNextBtn = CreateWindow("BUTTON", "Next", WS_VISIBLE | WS_CHILD,
                               190, 10, 80, 30, hwndMain, (HMENU)ID_BUTTON_NEXT, hInst, NULL);

    hwndRemoveBtn = CreateWindow("BUTTON", "Remove", WS_VISIBLE | WS_CHILD,
                                 280, 10, 80, 30, hwndMain, (HMENU)ID_BUTTON_REMOVE, hInst, NULL);

    hwndListBox = CreateWindow("LISTBOX", NULL, WS_VISIBLE | WS_CHILD | WS_BORDER | LBS_NOTIFY,
                               10, 50, 360, 200, hwndMain, (HMENU)ID_LISTBOX_QUEUE, hInst, NULL);

    ShowWindow(hwndMain, nCmdShow);
    UpdateWindow(hwndMain);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) DispatchMessage(&msg);
    return 0;
}
