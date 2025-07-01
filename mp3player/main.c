#include <stdio.h>
#include <stdlib.h>
#include <mpg123.h>
#include <portaudio.h>

#define FRAMES_PER_BUFFER 1024
#define MP3_BUFFER_SIZE (FRAMES_PER_BUFFER * 4) // Stereo 16-bit

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <filename.mp3>\n", argv[0]);
        return 1;
    }

    mpg123_init();
    mpg123_handle *mh = mpg123_new(NULL, NULL);
    if (!mh) {
        fprintf(stderr, "MPG123 init failed\n");
        mpg123_exit();
        return 1;
    }

    // Clear all formats, then allow only these:
    mpg123_format_none(mh);
    mpg123_format(mh, 44100, MPG123_STEREO, MPG123_ENC_SIGNED_16);
    mpg123_format(mh, 22050, MPG123_STEREO, MPG123_ENC_SIGNED_16);
    mpg123_format(mh, 11025, MPG123_STEREO, MPG123_ENC_SIGNED_16);

    if (mpg123_open(mh, argv[1]) != MPG123_OK) {
        fprintf(stderr, "Failed to open: %s\n", argv[1]);
        mpg123_delete(mh);
        mpg123_exit();
        return 1;
    }

    long rate;
    int channels, encoding;
    if (mpg123_getformat(mh, &rate, &channels, &encoding) != MPG123_OK) {
        fprintf(stderr, "Failed to get format\n");
        mpg123_close(mh);
        mpg123_delete(mh);
        mpg123_exit();
        return 1;
    }

    // Initialize PortAudio
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        fprintf(stderr, "PortAudio init error: %s\n", Pa_GetErrorText(err));
        goto cleanup;
    }

    PaStream *stream;
    err = Pa_OpenDefaultStream(&stream, 0, channels, paInt16, rate,
                               FRAMES_PER_BUFFER, NULL, NULL);
    if (err != paNoError) {
        fprintf(stderr, "PortAudio open stream error: %s\n", Pa_GetErrorText(err));
        goto cleanup;
    }

    err = Pa_StartStream(stream);
    if (err != paNoError) {
        fprintf(stderr, "PortAudio start stream error: %s\n", Pa_GetErrorText(err));
        goto cleanup;
    }

    printf("Playing: %s (%.1f kHz, %d channels)\n", argv[1], rate / 1000.0, channels);
    printf("Press Ctrl+C to stop...\n");

    unsigned char buffer[MP3_BUFFER_SIZE];
    size_t bytes_read;

    while (mpg123_read(mh, buffer, sizeof(buffer), &bytes_read) == MPG123_OK) {
        // bytes_read is in bytes, convert to samples per channel for PortAudio
        size_t samples = bytes_read / (sizeof(short) * channels);
        err = Pa_WriteStream(stream, buffer, samples);
        if (err != paNoError) {
            fprintf(stderr, "PortAudio write error: %s\n", Pa_GetErrorText(err));
            break;
        }
    }

    // Cleanup
cleanup:
    if (stream) {
        Pa_StopStream(stream);
        Pa_CloseStream(stream);
    }
    Pa_Terminate();
    mpg123_close(mh);
    mpg123_delete(mh);
    mpg123_exit();

    return 0;
}
