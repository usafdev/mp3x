#include <stdio.h>
#include <windows.h>
#include <commdlg.h>
#include <mpg123.h>
#include <portaudio.h>
#include <string.h>
#include <stdlib.h>

#define ID_BUTTON_OPEN 1
#define ID_BUTTON_PAUSE 2
#define FRAMES_PER_BUFFER 4096

HWND hwndMain, hwndPauseBtn;
HANDLE playThread = NULL;
PaStream *stream = NULL;

volatile int isPlaying = 0;
volatile int isPaused = 0;
volatile int stopPlayback = 0;

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
    playlist.files[playlist.count++] = _strdup(filename);
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
    if (!mh) {
        MessageBox(hwndMain, "Failed to init mpg123", "Error", MB_OK | MB_ICONERROR);
        return 0;
    }
    mpg123_format_all(mh);

    PaError err = Pa_Initialize();
    if (err != paNoError) {
        MessageBox(hwndMain, "Failed to initialize PortAudio", "Error", MB_OK | MB_ICONERROR);
        mpg123_delete(mh);
        mpg123_exit();
        return 0;
    }

    unsigned char *buffer = malloc(FRAMES_PER_BUFFER * 2 * sizeof(short) * 2); // max buffer, stereo 16bit
    if (!buffer) {
        MessageBox(hwndMain, "Failed to allocate buffer", "Error", MB_OK | MB_ICONERROR);
        Pa_Terminate();
        mpg123_delete(mh);
        mpg123_exit();
        return 0;
    }

    isPlaying = 1;
    isPaused = 0;
    stopPlayback = 0;

    while (!stopPlayback) {
        EnterCriticalSection(&playlistLock);
        if (playlist.count == 0) {
            LeaveCriticalSection(&playlistLock);
            Sleep(100); // no files, wait
            continue;
        }
        // Make a copy of current playlist count and files pointer to avoid holding lock during play
        size_t count = playlist.count;
        char **files = playlist.files;
        LeaveCriticalSection(&playlistLock);

        for (size_t track = 0; track < count; track++) {
            if (stopPlayback) break;

            // Open current track
            if (mpg123_open(mh, files[track]) != MPG123_OK) {
                char errMsg[512];
                snprintf(errMsg, sizeof(errMsg), "Failed to open MP3:\n%s", files[track]);
                MessageBox(hwndMain, errMsg, "Error", MB_OK | MB_ICONERROR);
                continue; // skip to next
            }

            long rate;
            int channels, encoding;
            if (mpg123_getformat(mh, &rate, &channels, &encoding) != MPG123_OK || encoding != MPG123_ENC_SIGNED_16) {
                MessageBox(hwndMain, "Unsupported MP3 format", "Error", MB_OK | MB_ICONERROR);
                mpg123_close(mh);
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
                MessageBox(hwndMain, "Failed to open audio stream", "Error", MB_OK | MB_ICONERROR);
                mpg123_close(mh);
                continue;
            }
            err = Pa_StartStream(stream);
            if (err != paNoError) {
                MessageBox(hwndMain, "Failed to start audio stream", "Error", MB_OK | MB_ICONERROR);
                Pa_CloseStream(stream);
                stream = NULL;
                mpg123_close(mh);
                continue;
            }

            char nowPlaying[512];
            snprintf(nowPlaying, sizeof(nowPlaying), "Playing: %s", files[track]);
            SetWindowText(hwndMain, nowPlaying);

            size_t done = 0;
            int ret;

            while ((ret = mpg123_read(mh, buffer, FRAMES_PER_BUFFER * channels * sizeof(short), &done)) == MPG123_OK) {
                if (stopPlayback) break;

                while (isPaused) {
                    Sleep(100);
                    if (stopPlayback) break;
                }
                if (stopPlayback) break;

                if (done > 0) {
                    size_t samples = done / (sizeof(short) * channels);
                    err = Pa_WriteStream(stream, buffer, samples);
                    if (err != paNoError) {
                        MessageBox(hwndMain, "PortAudio write error", "Error", MB_OK | MB_ICONERROR);
                        break;
                    }
                }
            }

            Pa_StopStream(stream);
            Pa_CloseStream(stream);
            stream = NULL;
            mpg123_close(mh);
            if (stopPlayback) break;
        }
        // Loop again forever
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
        // Multiple files may be selected, parse them
        char *ptr = filesBuffer;
        char directory[MAX_PATH];
        strcpy(directory, ptr);
        ptr += strlen(ptr) + 1;

        if (*ptr == 0) {
            // Only one file selected, add it
            AddToPlaylist(directory);
        } else {
            // Multiple files selected
            while (*ptr) {
                char fullpath[MAX_PATH];
                snprintf(fullpath, sizeof(fullpath), "%s\\%s", directory, ptr);
                AddToPlaylist(fullpath);
                ptr += strlen(ptr) + 1;
            }
        }

        if (!isPlaying) {
            stopPlayback = 0;
            playThread = CreateThread(NULL, 0, PlayMP3Queue, NULL, 0, NULL);
            EnableWindow(hwndPauseBtn, TRUE);
            SetWindowText(hwndPauseBtn, "Pause");
        }
    }
}

void TogglePause() {
    if (!isPlaying) return;

    isPaused = !isPaused;
    if (isPaused) {
        SetWindowText(hwndPauseBtn, "Resume");
    } else {
        SetWindowText(hwndPauseBtn, "Pause");
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch(msg) {
    case WM_COMMAND:
        if (LOWORD(wParam) == ID_BUTTON_OPEN) {
            OpenFileDialogAndAddFiles(hwnd);
        } else if (LOWORD(wParam) == ID_BUTTON_PAUSE) {
            TogglePause();
        }
        break;
    case WM_CLOSE:
        if (isPlaying) {
            stopPlayback = 1;
            if (playThread) {
                WaitForSingleObject(playThread, 3000);
                CloseHandle(playThread);
                playThread = NULL;
            }
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
    wc.lpszClassName = "MinimalMP3Window";
    RegisterClass(&wc);

    hwndMain = CreateWindow(wc.lpszClassName, "MP3 Player",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            CW_USEDEFAULT, CW_USEDEFAULT, 360, 120,
                            NULL, NULL, hInst, NULL);

    CreateWindow("BUTTON", "Open MP3(s)", WS_VISIBLE | WS_CHILD,
                 30, 30, 130, 40, hwndMain, (HMENU)ID_BUTTON_OPEN, hInst, NULL);

    hwndPauseBtn = CreateWindow("BUTTON", "Pause", WS_VISIBLE | WS_CHILD | WS_DISABLED,
                               200, 30, 100, 40, hwndMain, (HMENU)ID_BUTTON_PAUSE, hInst, NULL);

    ShowWindow(hwndMain, nCmdShow);
    UpdateWindow(hwndMain);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) DispatchMessage(&msg);

    return 0;
}
