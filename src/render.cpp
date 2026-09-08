#include "render.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "config.h"
#include "log.h"

namespace k2se {
namespace render {
namespace {

// --- GL constants, spelled out so no GL headers are needed --------------------
constexpr uint32_t GL_TEXTURE_2D = 0x0DE1;
constexpr uint32_t GL_RGB = 0x1907;
constexpr uint32_t GL_TEXTURE_MIN_FILTER = 0x2801;
constexpr uint32_t GL_TEXTURE_MAG_FILTER = 0x2800;
constexpr uint32_t GL_TEXTURE_WRAP_S = 0x2802;
constexpr uint32_t GL_TEXTURE_WRAP_T = 0x2803;
constexpr uint32_t GL_LINEAR = 0x2601;
constexpr uint32_t GL_CLAMP_TO_EDGE = 0x812F;
constexpr uint32_t GL_MODELVIEW = 0x1700;
constexpr uint32_t GL_PROJECTION = 0x1701;
constexpr uint32_t GL_QUADS = 0x0007;
constexpr uint32_t GL_DEPTH_TEST = 0x0B71;
constexpr uint32_t GL_BLEND = 0x0BE2;
constexpr uint32_t GL_LIGHTING = 0x0B50;
constexpr uint32_t GL_CULL_FACE = 0x0B44;
constexpr uint32_t GL_FOG = 0x0B60;
constexpr uint32_t GL_ALPHA_TEST = 0x0BC0;
constexpr uint32_t GL_VIEWPORT = 0x0BA2;
constexpr uint32_t GL_ALL_ATTRIB_BITS = 0x000FFFFF;
constexpr uint32_t GL_FRAGMENT_PROGRAM_ARB = 0x8804;
constexpr uint32_t GL_PROGRAM_FORMAT_ASCII_ARB = 0x8875;
constexpr uint32_t GL_PROGRAM_ERROR_POSITION_ARB = 0x864B;
constexpr uint32_t GL_PROGRAM_ERROR_STRING_ARB = 0x8874;

// The engine's own GDI32 import slot for SwapBuffers -- the frame boundary.
// Verified by name against the import directory, as glhook.cpp does.
constexpr uint32_t kIatSwapBuffers = 0x0098601C;

// --- entry points -------------------------------------------------------------
using SwapBuffersFn = int(__stdcall*)(void* hdc);
using VoidFn = void(__stdcall*)();
using GenTexturesFn = void(__stdcall*)(int n, uint32_t* out);
using BindTextureFn = void(__stdcall*)(uint32_t target, uint32_t id);
using TexParameteriFn = void(__stdcall*)(uint32_t target, uint32_t pname, int param);
using CopyTexImage2DFn = void(__stdcall*)(uint32_t target, int level, uint32_t fmt, int x,
                                          int y, int w, int h, int border);
using CopyTexSubImage2DFn = void(__stdcall*)(uint32_t target, int level, int xoff, int yoff,
                                             int x, int y, int w, int h);
using EnableFn = void(__stdcall*)(uint32_t cap);
using GetIntegervFn = void(__stdcall*)(uint32_t pname, int* out);
using MatrixModeFn = void(__stdcall*)(uint32_t mode);
using OrthoFn = void(__stdcall*)(double l, double r, double b, double t, double n, double f);
using BeginFn = void(__stdcall*)(uint32_t mode);
using TexCoord2fFn = void(__stdcall*)(float s, float t);
using Vertex2fFn = void(__stdcall*)(float x, float y);
using Color4fFn = void(__stdcall*)(float r, float g, float b, float a);
using PushAttribFn = void(__stdcall*)(uint32_t mask);
using ViewportFn = void(__stdcall*)(int x, int y, int w, int h);
using GenProgramsFn = void(__stdcall*)(int n, uint32_t* out);
using BindProgramFn = void(__stdcall*)(uint32_t target, uint32_t id);
using ProgramStringFn = void(__stdcall*)(uint32_t target, uint32_t fmt, int len, const void* s);
using ProgramEnvFn = void(__stdcall*)(uint32_t target, uint32_t index, float a, float b,
                                      float c, float d);
using WglGetProcAddressFn = void*(__stdcall*)(const char* name);
using GetStringFn = const char*(__stdcall*)(uint32_t name);

struct Gl {
    SwapBuffersFn swapBuffers = nullptr;
    GenTexturesFn genTextures = nullptr;
    BindTextureFn bindTexture = nullptr;
    TexParameteriFn texParameteri = nullptr;
    CopyTexImage2DFn copyTexImage2D = nullptr;
    CopyTexSubImage2DFn copyTexSubImage2D = nullptr;
    EnableFn enable = nullptr;
    EnableFn disable = nullptr;
    GetIntegervFn getIntegerv = nullptr;
    MatrixModeFn matrixMode = nullptr;
    VoidFn loadIdentity = nullptr;
    VoidFn pushMatrix = nullptr;
    VoidFn popMatrix = nullptr;
    OrthoFn ortho = nullptr;
    BeginFn begin = nullptr;
    VoidFn end = nullptr;
    TexCoord2fFn texCoord2f = nullptr;
    Vertex2fFn vertex2f = nullptr;
    Color4fFn color4f = nullptr;
    PushAttribFn pushAttrib = nullptr;
    VoidFn popAttrib = nullptr;
    ViewportFn viewport = nullptr;
    WglGetProcAddressFn wglGetProcAddress = nullptr;
    GetStringFn getString = nullptr;
    // ARB_fragment_program, through wglGetProcAddress.
    GenProgramsFn genPrograms = nullptr;
    BindProgramFn bindProgram = nullptr;
    ProgramStringFn programString = nullptr;
    ProgramEnvFn programEnv = nullptr;
};
Gl g_gl;

struct Cfg {
    bool enabled = false;
    bool post = false;
    bool dumpShaders = false;
    bool replaceShaders = false;
    float brightness = 1.0f;
    float contrast = 1.0f;
    float saturation = 1.0f;
    float tint[3] = {1.0f, 1.0f, 1.0f};
    float sharpen = 0.0f;
};
Cfg g_cfg;

struct State {
    int status = kOff;
    uint32_t texture = 0;
    uint32_t program = 0;
    int texSize = 0;          // power-of-two square that holds the frame
    int lastW = 0, lastH = 0;
    uint32_t frames = 0;
    uint32_t dumped = 0;
    uint32_t replaced = 0;
    bool postFailed = false;
    char gameDir[MAX_PATH] = "";
    char shaderDir[MAX_PATH] = "";
};
State g_st;

SwapBuffersFn g_origSwapBuffers = nullptr;
void* g_swapSlotOriginal = nullptr;

// --- the post-process fragment program ---------------------------------------
// One pass: a five-tap sharpen, then saturation, contrast, brightness, tint.
//
//   program.env[0] = (brightness, contrast, saturation, unused)
//   program.env[1] = (tintR, tintG, tintB, sharpen amount)
//   program.env[2] = (texel width, texel height, unused, unused)
//
// Written against ARB_fragment_program because that is what this engine has.
// The luma weights are Rec.709: desaturating with a flat 1/3 average turns
// KOTOR's warm interiors muddy.
const char kPostProgram[] =
    "!!ARBfp1.0\n"
    "PARAM grade = program.env[0];\n"
    "PARAM tint  = program.env[1];\n"
    "PARAM texel = program.env[2];\n"
    "PARAM luma  = {0.2126, 0.7152, 0.0722, 0.0};\n"
    "PARAM half  = {0.5, 0.5, 0.5, 1.0};\n"
    "PARAM five  = {5.0, 5.0, 5.0, 1.0};\n"
    "PARAM axisx = {1.0, 0.0, 0.0, 0.0};\n"
    "PARAM axisy = {0.0, 1.0, 0.0, 0.0};\n"
    "TEMP c, n, acc, l, tc, stepx, stepy, sharp;\n"
    "TEX c, fragment.texcoord[0], texture[0], 2D;\n"
    // ARB assembly has no expressions: an operand is a register, a constant or
    // a swizzle of one, and nothing else. Writing `{1,0,0,0} * texel.xxxx`
    // inline is GLSL, and the driver rejected the whole program for it. The
    // step vectors are therefore built with real MUL instructions first.
    "MUL stepx, texel, axisx;\n"
    "MUL stepy, texel, axisy;\n"
    // Sharpen: centre*5 - the four neighbours, blended back by the amount.
    "ADD tc, fragment.texcoord[0], stepx;\n"
    "TEX n, tc, texture[0], 2D;\n"
    "MOV acc, n;\n"
    "SUB tc, fragment.texcoord[0], stepx;\n"
    "TEX n, tc, texture[0], 2D;\n"
    "ADD acc, acc, n;\n"
    "ADD tc, fragment.texcoord[0], stepy;\n"
    "TEX n, tc, texture[0], 2D;\n"
    "ADD acc, acc, n;\n"
    "SUB tc, fragment.texcoord[0], stepy;\n"
    "TEX n, tc, texture[0], 2D;\n"
    "ADD acc, acc, n;\n"
    "MAD sharp, c, five, -acc;\n"
    "LRP c, tint.wwww, sharp, c;\n"   // amount 0 leaves the centre sample intact
    // Grade.
    "DP3 l, c, luma;\n"
    "LRP c.rgb, grade.zzzz, c, l;\n"            // saturation
    "SUB c.rgb, c, half;\n"
    "MUL c.rgb, c, grade.yyyy;\n"               // contrast
    "ADD c.rgb, c, half;\n"
    "MUL c.rgb, c, grade.xxxx;\n"               // brightness
    "MUL c.rgb, c, tint;\n"
    "MOV c.a, 1.0;\n"
    "MOV result.color, c;\n"
    "END\n";

// --- resolving GL ------------------------------------------------------------
template <typename Fn>
bool Resolve(HMODULE gl, const char* name, Fn* out) {
    *out = reinterpret_cast<Fn>(GetProcAddress(gl, name));
    if (!*out) log::Writef("render: opengl32!%s not found", name);
    return *out != nullptr;
}

bool ResolveCore() {
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    HMODULE gdi = GetModuleHandleA("gdi32.dll");
    if (!gl || !gdi) {
        log::Write("render: opengl32/gdi32 not loaded yet");
        return false;
    }
    bool ok = true;
    ok &= Resolve(gdi, "SwapBuffers", &g_gl.swapBuffers);
    ok &= Resolve(gl, "glGenTextures", &g_gl.genTextures);
    ok &= Resolve(gl, "glBindTexture", &g_gl.bindTexture);
    ok &= Resolve(gl, "glTexParameteri", &g_gl.texParameteri);
    ok &= Resolve(gl, "glCopyTexImage2D", &g_gl.copyTexImage2D);
    ok &= Resolve(gl, "glCopyTexSubImage2D", &g_gl.copyTexSubImage2D);
    ok &= Resolve(gl, "glEnable", &g_gl.enable);
    ok &= Resolve(gl, "glDisable", &g_gl.disable);
    ok &= Resolve(gl, "glGetIntegerv", &g_gl.getIntegerv);
    ok &= Resolve(gl, "glMatrixMode", &g_gl.matrixMode);
    ok &= Resolve(gl, "glLoadIdentity", &g_gl.loadIdentity);
    ok &= Resolve(gl, "glPushMatrix", &g_gl.pushMatrix);
    ok &= Resolve(gl, "glPopMatrix", &g_gl.popMatrix);
    ok &= Resolve(gl, "glOrtho", &g_gl.ortho);
    ok &= Resolve(gl, "glBegin", &g_gl.begin);
    ok &= Resolve(gl, "glEnd", &g_gl.end);
    ok &= Resolve(gl, "glTexCoord2f", &g_gl.texCoord2f);
    ok &= Resolve(gl, "glVertex2f", &g_gl.vertex2f);
    ok &= Resolve(gl, "glColor4f", &g_gl.color4f);
    ok &= Resolve(gl, "glPushAttrib", &g_gl.pushAttrib);
    ok &= Resolve(gl, "glPopAttrib", &g_gl.popAttrib);
    ok &= Resolve(gl, "glViewport", &g_gl.viewport);
    ok &= Resolve(gl, "wglGetProcAddress", &g_gl.wglGetProcAddress);
    ok &= Resolve(gl, "glGetString", &g_gl.getString);
    return ok;
}

// ARB entry points exist only once a context is current, so this runs from the
// first SwapBuffers rather than from Install.
bool ResolveArb() {
    if (g_gl.programString) return true;
    if (!g_gl.wglGetProcAddress) return false;
    g_gl.genPrograms = reinterpret_cast<GenProgramsFn>(g_gl.wglGetProcAddress("glGenProgramsARB"));
    g_gl.bindProgram = reinterpret_cast<BindProgramFn>(g_gl.wglGetProcAddress("glBindProgramARB"));
    g_gl.programString =
        reinterpret_cast<ProgramStringFn>(g_gl.wglGetProcAddress("glProgramStringARB"));
    g_gl.programEnv =
        reinterpret_cast<ProgramEnvFn>(g_gl.wglGetProcAddress("glProgramEnvParameter4fARB"));
    const bool ok = g_gl.genPrograms && g_gl.bindProgram && g_gl.programString && g_gl.programEnv;
    if (!ok) log::Write("render: ARB_fragment_program entry points unavailable -- "
                        "post-processing off");
    return ok;
}

int NextPowerOfTwo(int v) {
    int p = 1;
    while (p < v && p < 4096) p <<= 1;
    return p;
}

bool EnsureResources(int w, int h) {
    if (!ResolveArb()) return false;

    if (!g_st.program) {
        uint32_t id = 0;
        g_gl.genPrograms(1, &id);
        if (!id) return false;
        g_gl.bindProgram(GL_FRAGMENT_PROGRAM_ARB, id);
        g_gl.programString(GL_FRAGMENT_PROGRAM_ARB, GL_PROGRAM_FORMAT_ASCII_ARB,
                           static_cast<int>(sizeof(kPostProgram) - 1), kPostProgram);
        int errorPos = -1;
        g_gl.getIntegerv(GL_PROGRAM_ERROR_POSITION_ARB, &errorPos);
        if (errorPos != -1) {
            // A character offset alone cost a whole play session: it said the
            // program was wrong but not why. The driver spells it out in
            // GL_PROGRAM_ERROR_STRING_ARB, and the offending line is worth
            // printing next to it.
            const char* why = g_gl.getString ? g_gl.getString(GL_PROGRAM_ERROR_STRING_ARB)
                                             : nullptr;
            int lineStart = errorPos;
            while (lineStart > 0 && kPostProgram[lineStart - 1] != '\n') --lineStart;
            int lineEnd = errorPos;
            const int total = static_cast<int>(sizeof(kPostProgram) - 1);
            while (lineEnd < total && kPostProgram[lineEnd] != '\n') ++lineEnd;
            char snippet[160];
            int length = lineEnd - lineStart;
            if (length > static_cast<int>(sizeof(snippet)) - 1)
                length = static_cast<int>(sizeof(snippet)) - 1;
            if (length < 0) length = 0;
            memcpy(snippet, kPostProgram + lineStart, static_cast<size_t>(length));
            snippet[length] = '\0';
            log::Writef("render: the post-process program was REJECTED at character %d: %s",
                        errorPos, why ? why : "(driver gave no message)");
            log::Writef("render:   offending line: %s", snippet);
            g_st.postFailed = true;
            return false;
        }
        g_st.program = id;
        g_st.status |= kProgramReady;
        log::Writef("render: post-process program compiled (id %u)", id);
    }

    const int size = NextPowerOfTwo(w > h ? w : h);
    if (!g_st.texture || size != g_st.texSize) {
        if (!g_st.texture) {
            g_gl.genTextures(1, &g_st.texture);
            if (!g_st.texture) return false;
        }
        g_gl.bindTexture(GL_TEXTURE_2D, g_st.texture);
        g_gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        g_gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        g_gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        g_gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // A power-of-two square: GL 1.x cannot be relied on for NPOT textures,
        // and the frame occupies the bottom-left corner of it.
        g_gl.copyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 0, 0, size, size, 0);
        g_st.texSize = size;
        log::Writef("render: capture texture %ux%u for a %dx%d frame", size, size, w, h);
    }
    return true;
}

bool GradeIsIdentity() {
    return g_cfg.brightness == 1.0f && g_cfg.contrast == 1.0f && g_cfg.saturation == 1.0f &&
           g_cfg.sharpen == 0.0f && g_cfg.tint[0] == 1.0f && g_cfg.tint[1] == 1.0f &&
           g_cfg.tint[2] == 1.0f;
}

bool g_coreResolved = false;

void PostProcess() {
    if (!g_cfg.post || g_st.postFailed || GradeIsIdentity()) return;
    // Resolved here, not in Install: at DllMain time opengl32 may not be mapped
    // yet (this DLL is loaded as a dependency, and the loader order is not ours
    // to choose), and the ARB entry points need a current context in any case.
    if (!g_coreResolved) {
        g_coreResolved = true;
        if (!ResolveCore()) {
            g_st.postFailed = true;
            log::Write("render: GL entry points did not resolve -- post-processing off");
            return;
        }
    }
    if (!g_gl.getIntegerv) return;

    int viewport[4] = {0, 0, 0, 0};
    g_gl.getIntegerv(GL_VIEWPORT, viewport);
    const int w = viewport[2];
    const int h = viewport[3];
    if (w <= 0 || h <= 0) return;
    if (w != g_st.lastW || h != g_st.lastH) {
        g_st.lastW = w;
        g_st.lastH = h;
    }
    if (!EnsureResources(w, h)) return;

    // Grab the finished frame. glPushAttrib/glPopAttrib is what keeps this from
    // leaking state into the game's next frame -- without it the first visible
    // symptom is the GUI losing its blending.
    g_gl.pushAttrib(GL_ALL_ATTRIB_BITS);

    g_gl.disable(GL_DEPTH_TEST);
    g_gl.disable(GL_BLEND);
    g_gl.disable(GL_LIGHTING);
    g_gl.disable(GL_CULL_FACE);
    g_gl.disable(GL_FOG);
    g_gl.disable(GL_ALPHA_TEST);
    g_gl.enable(GL_TEXTURE_2D);
    g_gl.bindTexture(GL_TEXTURE_2D, g_st.texture);
    g_gl.copyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);

    g_gl.matrixMode(GL_PROJECTION);
    g_gl.pushMatrix();
    g_gl.loadIdentity();
    g_gl.ortho(0.0, 1.0, 0.0, 1.0, -1.0, 1.0);
    g_gl.matrixMode(GL_MODELVIEW);
    g_gl.pushMatrix();
    g_gl.loadIdentity();

    g_gl.enable(GL_FRAGMENT_PROGRAM_ARB);
    g_gl.bindProgram(GL_FRAGMENT_PROGRAM_ARB, g_st.program);
    g_gl.programEnv(GL_FRAGMENT_PROGRAM_ARB, 0, g_cfg.brightness, g_cfg.contrast,
                    g_cfg.saturation, 0.0f);
    g_gl.programEnv(GL_FRAGMENT_PROGRAM_ARB, 1, g_cfg.tint[0], g_cfg.tint[1], g_cfg.tint[2],
                    g_cfg.sharpen);
    g_gl.programEnv(GL_FRAGMENT_PROGRAM_ARB, 2, 1.0f / static_cast<float>(g_st.texSize),
                    1.0f / static_cast<float>(g_st.texSize), 0.0f, 0.0f);

    // The frame sits in the bottom-left of the power-of-two texture, so the
    // quad's texture coordinates stop at the frame's fraction of it.
    const float u = static_cast<float>(w) / static_cast<float>(g_st.texSize);
    const float v = static_cast<float>(h) / static_cast<float>(g_st.texSize);
    g_gl.color4f(1.0f, 1.0f, 1.0f, 1.0f);
    g_gl.begin(GL_QUADS);
    g_gl.texCoord2f(0.0f, 0.0f);
    g_gl.vertex2f(0.0f, 0.0f);
    g_gl.texCoord2f(u, 0.0f);
    g_gl.vertex2f(1.0f, 0.0f);
    g_gl.texCoord2f(u, v);
    g_gl.vertex2f(1.0f, 1.0f);
    g_gl.texCoord2f(0.0f, v);
    g_gl.vertex2f(0.0f, 1.0f);
    g_gl.end();

    g_gl.disable(GL_FRAGMENT_PROGRAM_ARB);
    g_gl.matrixMode(GL_MODELVIEW);
    g_gl.popMatrix();
    g_gl.matrixMode(GL_PROJECTION);
    g_gl.popMatrix();
    g_gl.popAttrib();

    g_st.status |= kPostActive;
}

int __stdcall HookSwapBuffers(void* hdc) {
    ++g_st.frames;
    __try {
        PostProcess();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (!g_st.postFailed) {
            g_st.postFailed = true;
            log::Write("render: the post-process pass faulted -- disabled for this session");
        }
    }
    return g_origSwapBuffers ? g_origSwapBuffers(hdc) : 1;
}

// --- import patching ----------------------------------------------------------
bool WriteSlot(uint32_t va, void* value, void** previous) {
    auto* slot = reinterpret_cast<void**>(va);
    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        log::Writef("render: REFUSED TO WRITE 0x%08X, VirtualProtect failed (%lu)", va,
                    GetLastError());
        return false;
    }
    if (previous) *previous = *slot;
    *slot = value;
    DWORD restored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &restored);
    return true;
}

// --- shader dump and replace --------------------------------------------------
// A stable name for a program, so a dump and its replacement line up. FNV-1a
// over the source: short, deterministic, and independent of load order, which
// a sequence number would not be.
uint32_t HashSource(const char* s, int len) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; ++i) {
        h ^= static_cast<unsigned char>(s[i]);
        h *= 16777619u;
    }
    return h;
}

char g_replacement[64 * 1024];

void EnsureShaderDir() {
    if (g_st.shaderDir[0]) return;
    _snprintf(g_st.shaderDir, sizeof(g_st.shaderDir), "%sk2se_shaders", g_st.gameDir);
    g_st.shaderDir[sizeof(g_st.shaderDir) - 1] = '\0';
    CreateDirectoryA(g_st.shaderDir, nullptr);
}

void ReadConfig() {
    g_cfg.enabled = config::GetBool("Render", "Enabled", false);
    g_cfg.post = config::GetBool("Render", "PostProcess", false);
    g_cfg.dumpShaders = config::GetBool("Render", "DumpShaders", false);
    g_cfg.replaceShaders = config::GetBool("Render", "ReplaceShaders", false);
    g_cfg.brightness = config::GetFloat("Render", "Brightness", 1.0f);
    g_cfg.contrast = config::GetFloat("Render", "Contrast", 1.0f);
    g_cfg.saturation = config::GetFloat("Render", "Saturation", 1.0f);
    g_cfg.tint[0] = config::GetFloat("Render", "TintR", 1.0f);
    g_cfg.tint[1] = config::GetFloat("Render", "TintG", 1.0f);
    g_cfg.tint[2] = config::GetFloat("Render", "TintB", 1.0f);
    g_cfg.sharpen = config::GetFloat("Render", "Sharpen", 0.0f);

    // Clamps that keep a typo from producing a black or blinding screen.
    if (g_cfg.brightness < 0.1f) g_cfg.brightness = 0.1f;
    if (g_cfg.brightness > 3.0f) g_cfg.brightness = 3.0f;
    if (g_cfg.contrast < 0.1f) g_cfg.contrast = 0.1f;
    if (g_cfg.contrast > 3.0f) g_cfg.contrast = 3.0f;
    if (g_cfg.saturation < 0.0f) g_cfg.saturation = 0.0f;
    if (g_cfg.saturation > 3.0f) g_cfg.saturation = 3.0f;
    if (g_cfg.sharpen < 0.0f) g_cfg.sharpen = 0.0f;
    if (g_cfg.sharpen > 1.0f) g_cfg.sharpen = 1.0f;
    for (int i = 0; i < 3; ++i) {
        if (g_cfg.tint[i] < 0.0f) g_cfg.tint[i] = 0.0f;
        if (g_cfg.tint[i] > 2.0f) g_cfg.tint[i] = 2.0f;
    }

    log::Writef("render: %s post %d dump %d replace %d | bright %d contrast %d sat %d "
                "tint %d,%d,%d sharpen %d (x100)",
                g_cfg.enabled ? "ON" : "off", g_cfg.post ? 1 : 0, g_cfg.dumpShaders ? 1 : 0,
                g_cfg.replaceShaders ? 1 : 0, static_cast<int>(g_cfg.brightness * 100.0f),
                static_cast<int>(g_cfg.contrast * 100.0f),
                static_cast<int>(g_cfg.saturation * 100.0f),
                static_cast<int>(g_cfg.tint[0] * 100.0f),
                static_cast<int>(g_cfg.tint[1] * 100.0f),
                static_cast<int>(g_cfg.tint[2] * 100.0f),
                static_cast<int>(g_cfg.sharpen * 100.0f));
}

}  // namespace

bool Install() {
    if (g_st.status & kInstalled) return true;
    if (!config::Present()) return false;
    ReadConfig();
    if (!g_cfg.enabled) return false;
    if (!GetModuleFileNameA(nullptr, g_st.gameDir, MAX_PATH)) return false;
    char* slash = strrchr(g_st.gameDir, '\\');
    if (!slash) return false;
    slash[1] = '\0';

    if (g_cfg.dumpShaders || g_cfg.replaceShaders) EnsureShaderDir();

    if (g_cfg.post) {
        void* previous = nullptr;
        if (WriteSlot(kIatSwapBuffers, reinterpret_cast<void*>(&HookSwapBuffers), &previous) &&
            previous) {
            g_swapSlotOriginal = previous;
            g_origSwapBuffers = reinterpret_cast<SwapBuffersFn>(previous);
            g_st.status |= kSwapHooked;
            log::Writef("render: SwapBuffers [0x%08X] 0x%08X -> hook", kIatSwapBuffers,
                        reinterpret_cast<uint32_t>(previous));
        } else {
            log::Write("render: SwapBuffers could not be hooked -- post-processing off");
            g_cfg.post = false;
        }
    }

    g_st.status |= kInstalled;
    log::Writef("render: installed%s", g_cfg.post ? " with the post-process pass" : "");
    return true;
}

void Remove() {
    if (!(g_st.status & kInstalled)) return;
    if (g_st.status & kSwapHooked) WriteSlot(kIatSwapBuffers, g_swapSlotOriginal, nullptr);
    log::Writef("render: removed after %u frames, %u shader(s) dumped, %u replaced",
                g_st.frames, g_st.dumped, g_st.replaced);
    g_st.status = kOff;
}

int Status() { return g_st.status; }

void SetGrade(float brightness, float contrast, float saturation) {
    g_cfg.brightness = brightness;
    g_cfg.contrast = contrast;
    g_cfg.saturation = saturation;
    log::Writef("render: grade set to bright %d contrast %d sat %d (x100)",
                static_cast<int>(brightness * 100.0f), static_cast<int>(contrast * 100.0f),
                static_cast<int>(saturation * 100.0f));
}

void SetTint(float r, float g, float b) {
    g_cfg.tint[0] = r;
    g_cfg.tint[1] = g;
    g_cfg.tint[2] = b;
}

void SetSharpen(float amount) {
    if (amount < 0.0f) amount = 0.0f;
    if (amount > 1.0f) amount = 1.0f;
    g_cfg.sharpen = amount;
}

bool SetEnabled(bool on) {
    if (!(g_st.status & kSwapHooked)) return false;
    g_cfg.post = on;
    log::Writef("render: post-process %s", on ? "enabled" : "disabled");
    return true;
}

bool WantShaderDump() { return g_cfg.enabled && g_cfg.dumpShaders; }
bool WantShaderReplace() { return g_cfg.enabled && g_cfg.replaceShaders; }

void NoteShaderSeen(const char* source, int len) {
    if (!WantShaderDump() || !source || len <= 0) return;
    EnsureShaderDir();
    char path[MAX_PATH];
    _snprintf(path, sizeof(path), "%s\\%08X.fp", g_st.shaderDir, HashSource(source, len));
    path[sizeof(path) - 1] = '\0';
    // CREATE_NEW: the first dump of a program wins, so a session does not
    // rewrite files the user may have started editing.
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, source, static_cast<DWORD>(len), &written, nullptr);
    CloseHandle(h);
    ++g_st.dumped;
    if (g_st.dumped <= 3)
        log::Writef("render: fragment program dumped to %s (%d bytes)", path, len);
}

const char* ReplacementFor(const char* source, int len, int* outLen) {
    if (!WantShaderReplace() || !source || len <= 0 || !outLen) return nullptr;
    EnsureShaderDir();
    char path[MAX_PATH];
    _snprintf(path, sizeof(path), "%s\\%08X.fp", g_st.shaderDir, HashSource(source, len));
    path[sizeof(path) - 1] = '\0';
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    DWORD size = GetFileSize(h, nullptr);
    if (size == INVALID_FILE_SIZE || size == 0 ||
        size > static_cast<DWORD>(sizeof(g_replacement))) {
        CloseHandle(h);
        return nullptr;
    }
    DWORD read = 0;
    const BOOL ok = ReadFile(h, g_replacement, size, &read, nullptr);
    CloseHandle(h);
    if (!ok || read == 0) return nullptr;
    // A replacement identical to the original is not a replacement; skipping it
    // keeps the counter honest about how much is actually being overridden.
    if (static_cast<int>(read) == len && memcmp(g_replacement, source, read) == 0) return nullptr;
    *outLen = static_cast<int>(read);
    ++g_st.replaced;
    if (g_st.replaced <= 3)
        log::Writef("render: fragment program replaced from %s (%d -> %d bytes)", path, len,
                    *outLen);
    return g_replacement;
}

}  // namespace render
}  // namespace k2se
