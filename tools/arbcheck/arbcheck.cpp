// Compile K2SE's post-process fragment program against the real GL driver.
//
// This exists because a shader that fails to compile costs a whole play
// session: the game logged "REJECTED at character 301" and the post-process
// pass silently did nothing, and nobody knew until the log was read afterwards.
// A character offset is not a diagnosis. This builds a hidden window, makes a
// GL context current, submits the program, and prints the driver's own error
// string with the offending line -- in under a second, before anything ships.
//
// Build and run:
//     powershell -File tools/arbcheck/build_arbcheck.ps1
//     out/arbcheck.exe                  # the program compiled into K2SE
//     out/arbcheck.exe some_shader.fp   # or any file
//
// This is a test harness, not part of the DLL, so linking opengl32 here does
// not violate the KERNEL32-only rule that tools/check_dll.py enforces.

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <GL/gl.h>

#ifndef GL_FRAGMENT_PROGRAM_ARB
#define GL_FRAGMENT_PROGRAM_ARB 0x8804
#endif
#define GL_PROGRAM_FORMAT_ASCII_ARB 0x8875
#define GL_PROGRAM_ERROR_POSITION_ARB 0x864B
#define GL_PROGRAM_ERROR_STRING_ARB 0x8874

typedef void(__stdcall* GenProgramsFn)(int, unsigned int*);
typedef void(__stdcall* BindProgramFn)(unsigned int, unsigned int);
typedef void(__stdcall* ProgramStringFn)(unsigned int, unsigned int, int, const void*);

// Kept in step with src/render.cpp by build_arbcheck.ps1, which extracts the
// literal from that file rather than letting a copy drift out of date.
#include "program.inc"

static void ReportLine(const char* source, int length, int errorPos) {
    if (errorPos < 0 || errorPos > length) return;
    int start = errorPos;
    while (start > 0 && source[start - 1] != '\n') --start;
    int end = errorPos;
    while (end < length && source[end] != '\n') ++end;
    printf("  offending line: %.*s\n", end - start, source + start);
    int lineNumber = 1;
    for (int i = 0; i < start; ++i)
        if (source[i] == '\n') ++lineNumber;
    printf("  line %d, character %d\n", lineNumber, errorPos);
}

int main(int argc, char** argv) {
    const char* source = kPostProgram;
    int length = (int)strlen(kPostProgram);
    char* owned = NULL;

    if (argc > 1) {
        FILE* fh = fopen(argv[1], "rb");
        if (!fh) {
            printf("FAIL cannot open %s\n", argv[1]);
            return 2;
        }
        fseek(fh, 0, SEEK_END);
        long size = ftell(fh);
        fseek(fh, 0, SEEK_SET);
        owned = (char*)malloc((size_t)size + 1);
        length = (int)fread(owned, 1, (size_t)size, fh);
        owned[length] = 0;
        fclose(fh);
        source = owned;
        printf("checking %s (%d bytes)\n", argv[1], length);
    } else {
        printf("checking the program compiled into src/render.cpp (%d bytes)\n", length);
    }

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "k2se_arbcheck";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("k2se_arbcheck", "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL,
                              NULL, wc.hInstance, NULL);
    HDC hdc = GetDC(hwnd);

    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    int format = ChoosePixelFormat(hdc, &pfd);
    SetPixelFormat(hdc, format, &pfd);
    HGLRC ctx = wglCreateContext(hdc);
    if (!ctx || !wglMakeCurrent(hdc, ctx)) {
        printf("FAIL could not create a GL context\n");
        return 2;
    }

    printf("GL_RENDERER: %s\n", (const char*)glGetString(GL_RENDERER));

    GenProgramsFn genPrograms = (GenProgramsFn)wglGetProcAddress("glGenProgramsARB");
    BindProgramFn bindProgram = (BindProgramFn)wglGetProcAddress("glBindProgramARB");
    ProgramStringFn programString = (ProgramStringFn)wglGetProcAddress("glProgramStringARB");
    if (!genPrograms || !bindProgram || !programString) {
        printf("FAIL ARB_fragment_program is not available on this driver\n");
        return 2;
    }

    unsigned int id = 0;
    genPrograms(1, &id);
    bindProgram(GL_FRAGMENT_PROGRAM_ARB, id);
    programString(GL_FRAGMENT_PROGRAM_ARB, GL_PROGRAM_FORMAT_ASCII_ARB, length, source);

    GLint errorPos = -1;
    glGetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB, &errorPos);
    if (errorPos != -1) {
        const char* why = (const char*)glGetString(GL_PROGRAM_ERROR_STRING_ARB);
        printf("FAIL %s\n", why ? why : "(driver gave no message)");
        ReportLine(source, length, (int)errorPos);
        free(owned);
        return 1;
    }

    printf("OK the fragment program compiles\n");
    free(owned);
    return 0;
}
