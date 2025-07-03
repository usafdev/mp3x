// Full working MP3 player with fixed next and remove behavior

#include <stdio.h>
#include <windows.h>
#include <commdlg.h>
#include <mpg123.h>
#include <portaudio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>  // for srand(time(NULL)) and rand()

// Control IDs for buttons and listbox
#define ID_BUTTON_OPEN   1
#define ID_BUTTON_PAUSE  2
#define ID_BUTTON_NEXT   3
#define ID_BUTTON_REMOVE 4
#define ID_LISTBOX_QUEUE 5
#define ID_BUTTON_SHUFFLE 6

#define FRAMES_PER_BUFFER 4096  // Buffer size for audio playback

// Window handles for controls
HWND hwndMain, hwndPauseBtn, hwndNextBtn, hwndRemoveBtn, hwndListBox, hwndShuffleBtn;

// Thread handle for playback
HANDLE playThread = NULL;
// PortAudio stream handle
PaStream *stream = NULL;

// Playback state flags
volatile int isPlaying = 0;      // Whether playback is active
volatile int isPaused = 0;       // Whether playback is paused
volatile int stopPlayback = 0;   // Signal to stop playback thread
volatile int skipToNext = 0;     // Flag to skip current track
volatile int nextPressed = 0;    // Distinguishes skip caused by Next button vs Remove

size_t currentTrackIndex = 0;    // Index of current playing track

// Playlist structure holds dynamically growing list of file paths
typedef struct {
    char **files;
    size_t count;
    size_t capacity;
} Playlist;

Playlist playlist = { NULL, 0, 0 };

// Critical section for thread-safe playlist access
CRITICAL_SECTION playlistLock;

// Adds a file path to the playlist and updates the listbox UI
void AddToPlaylist(const char *filepath) {
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
    // Store full path for playback
    playlist.files[playlist.count] = _strdup(filepath);

    // Extract filename from path to show in listbox
    const char *filename = filepath;
    const char *lastSlash = strrchr(filepath, '\\'); // find last backslash
    if (lastSlash) filename = lastSlash + 1;  // move past last slash

    SendMessage(hwndListBox, LB_ADDSTRING, 0, (LPARAM)filename);
    playlist.count++;
    LeaveCriticalSection(&playlistLock);
}


// Removes the file at index from playlist and updates listbox UI
void RemoveFromPlaylist(size_t index) {
    EnterCriticalSection(&playlistLock);
    if (index >= playlist.count) {
        LeaveCriticalSection(&playlistLock);
        return;
    }
    free(playlist.files[index]);  // Free filename string
    // Shift remaining entries left to fill gap
    for (size_t i = index; i < playlist.count - 1; ++i) {
        playlist.files[i] = playlist.files[i + 1];
    }
    playlist.count--;
    // Remove from listbox UI
    SendMessage(hwndListBox, LB_DELETESTRING, index, 0);

    // If removing currently playing track, signal skip but do not advance index
    if (index == currentTrackIndex) {
        skipToNext = 1;
        nextPressed = 0;  // Removing track, so do not increment currentTrackIndex
    } else if (index < currentTrackIndex && currentTrackIndex > 0) {
        // Adjust currentTrackIndex if removal before current track
        currentTrackIndex--;
    }
    LeaveCriticalSection(&playlistLock);
}

// Frees all playlist memory on shutdown
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

// Playback thread function: continuously plays tracks from playlist queue
DWORD WINAPI PlayMP3Queue(LPVOID lpParam) {
    (void)lpParam;

    mpg123_init();  // Initialize mpg123 decoder
    mpg123_handle *mh = mpg123_new(NULL, NULL);
    if (!mh) return 0;
    mpg123_format_all(mh);  // Enable all formats

    PaError err = Pa_Initialize();  // Initialize PortAudio
    if (err != paNoError) return 0;

    // Buffer for decoded audio data
    unsigned char *buffer = malloc(FRAMES_PER_BUFFER * 2 * sizeof(short) * 2);
    if (!buffer) return 0;

    isPlaying = 1;
    isPaused = 0;
    stopPlayback = 0;

    while (!stopPlayback) {
        EnterCriticalSection(&playlistLock);
        if (playlist.count == 0) {
            LeaveCriticalSection(&playlistLock);
            Sleep(100);  // Wait if no tracks to play
            continue;
        }
        if (currentTrackIndex >= playlist.count) currentTrackIndex = 0;
        char *file = _strdup(playlist.files[currentTrackIndex]);  // Copy current track filename
        LeaveCriticalSection(&playlistLock);

        // Open and prepare MP3 file for decoding
        if (mpg123_open(mh, file) != MPG123_OK) {
            free(file);
            EnterCriticalSection(&playlistLock);
            currentTrackIndex = (currentTrackIndex + 1) % playlist.count;
            LeaveCriticalSection(&playlistLock);
            continue;
        }

        // Get audio format info (sample rate, channels, encoding)
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

        // If audio stream open, stop and close before opening new stream
        if (stream) {
            Pa_StopStream(stream);
            Pa_CloseStream(stream);
            stream = NULL;
        }
        // Open PortAudio stream with audio format parameters
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
        Pa_StartStream(stream);  // Start audio playback stream

        // Update window title to show now playing track
        char nowPlaying[512];
        snprintf(nowPlaying, sizeof(nowPlaying), "Playing: %s", file);
        SetWindowText(hwndMain, nowPlaying);

        size_t done;
        int ret;
        skipToNext = 0;

        // Read decoded audio data and play until track ends or skip/stop signaled
        while (!skipToNext && !stopPlayback && (ret = mpg123_read(mh, buffer, FRAMES_PER_BUFFER * channels * sizeof(short), &done)) == MPG123_OK) {
            while (isPaused && !stopPlayback) Sleep(100);  // Pause playback if requested
            if (done > 0 && !stopPlayback) {
                size_t samples = done / (sizeof(short) * channels);
                err = Pa_WriteStream(stream, buffer, samples);  // Write audio data to output
                if (err != paNoError) break;
            }
        }

        mpg123_close(mh);
        free(file);

        EnterCriticalSection(&playlistLock);
        if (playlist.count > 0) {
            // Advance track index if Next button pressed or normal track end
            if (nextPressed) {
                currentTrackIndex = (currentTrackIndex + 1) % playlist.count;
                nextPressed = 0;
            } else if (!skipToNext) {
                currentTrackIndex = (currentTrackIndex + 1) % playlist.count;
            }
            // If skipToNext and nextPressed==0 (Remove button), do NOT advance index here
        } else {
            currentTrackIndex = 0;  // Reset if playlist empty
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

    SetWindowText(hwndMain, "MP3 Player");  // Reset window title when stopped
    return 0;
}

// Opens a file dialog to select MP3 files and adds them to the playlist
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

        // Single file selected
        if (*ptr == 0) {
            AddToPlaylist(directory);
        } else {
            // Multiple files selected, combine directory with each filename
            while (*ptr) {
                char fullpath[MAX_PATH];
                snprintf(fullpath, sizeof(fullpath), "%s\\%s", directory, ptr);
                AddToPlaylist(fullpath);
                ptr += strlen(ptr) + 1;
            }
        }

        // Start playback thread if not already playing
        if (!isPlaying) {
            stopPlayback = 0;
            currentTrackIndex = 0;
            playThread = CreateThread(NULL, 0, PlayMP3Queue, NULL, 0, NULL);
            EnableWindow(hwndPauseBtn, TRUE);
            SetWindowText(hwndPauseBtn, "Pause");
        }
    }
}

// Toggles pause/resume playback state
void TogglePause() {
    if (!isPlaying) return;
    isPaused = !isPaused;
    SetWindowText(hwndPauseBtn, isPaused ? "Resume" : "Pause");
}

// Signals playback thread to skip to next track
void SkipToNext() {
    if (isPlaying) {
        skipToNext = 1;
        nextPressed = 1;
    }
}

// Removes the selected track from the playlist
void RemoveSelectedFromQueue() {
    int sel = SendMessage(hwndListBox, LB_GETCURSEL, 0, 0);
    if (sel != LB_ERR) RemoveFromPlaylist((size_t)sel);
}

// Shuffles the playlist
void ShufflePlaylist() {
    EnterCriticalSection(&playlistLock);
    if (playlist.count <= 1) {
        LeaveCriticalSection(&playlistLock);
        return; // nothing to shuffle
    }

    // Fisher-Yates shuffle
    srand((unsigned int)time(NULL));
    for (size_t i = playlist.count - 1; i > 0; i--) {
        size_t j = rand() % (i + 1);
        char *temp = playlist.files[i];
        playlist.files[i] = playlist.files[j];
        playlist.files[j] = temp;
    }

    SendMessage(hwndListBox, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < playlist.count; i++) {
    // Show filename only
    const char *filename = playlist.files[i];
    const char *lastSlash = strrchr(filename, '\\');
    if (lastSlash) filename = lastSlash + 1;
    SendMessage(hwndListBox, LB_ADDSTRING, 0, (LPARAM)filename);
}


    currentTrackIndex = 0;
    nextPressed = 1;    // Tell playback thread to advance and update playing track immediately
    skipToNext = 1;     // Trigger playback to skip current track and reload new track

    LeaveCriticalSection(&playlistLock);
}


// Main window message handler
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_BUTTON_OPEN: OpenFileDialogAndAddFiles(hwnd); break;
                case ID_BUTTON_PAUSE: TogglePause(); break;
                case ID_BUTTON_NEXT: SkipToNext(); break;
                case ID_BUTTON_REMOVE: RemoveSelectedFromQueue(); break;
                case ID_BUTTON_SHUFFLE: ShufflePlaylist(); break;

            }
            break;
        case WM_CLOSE:
            stopPlayback = 1;  // Signal playback thread to stop
            if (playThread) {
                WaitForSingleObject(playThread, 3000);  // Wait for thread exit
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

// Program entry point: creates window, controls, and enters message loop
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

    // Create control buttons and listbox
    CreateWindow("BUTTON", "Open", WS_VISIBLE | WS_CHILD,
                 10, 10, 80, 30, hwndMain, (HMENU)ID_BUTTON_OPEN, hInst, NULL);


    hwndPauseBtn = CreateWindow("BUTTON", "Pause", WS_VISIBLE | WS_CHILD | WS_DISABLED,
                                100, 10, 80, 30, hwndMain, (HMENU)ID_BUTTON_PAUSE, hInst, NULL);

    hwndNextBtn = CreateWindow("BUTTON", "Next", WS_VISIBLE | WS_CHILD,
                               190, 10, 80, 30, hwndMain, (HMENU)ID_BUTTON_NEXT, hInst, NULL);

    hwndRemoveBtn = CreateWindow("BUTTON", "Remove", WS_VISIBLE | WS_CHILD,
                                 280, 10, 80, 30, hwndMain, (HMENU)ID_BUTTON_REMOVE, hInst, NULL);

    hwndListBox = CreateWindow("LISTBOX", NULL, WS_VISIBLE | WS_CHILD | WS_BORDER | LBS_NOTIFY,
                               10, 90, 360, 200, hwndMain, (HMENU)ID_LISTBOX_QUEUE, hInst, NULL);

    hwndShuffleBtn = CreateWindow("BUTTON", "Shuffle", WS_VISIBLE | WS_CHILD,
                 280, 50, 80, 30, hwndMain, (HMENU)ID_BUTTON_SHUFFLE, hInst, NULL);

    ShowWindow(hwndMain, nCmdShow);
    UpdateWindow(hwndMain);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) DispatchMessage(&msg);
    return 0;
}
