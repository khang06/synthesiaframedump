#include <Windows.h>
#include "Hooks.h"

extern "C" __declspec(dllexport) void dummyexport() {}

// why do i even have to make 3 macros for 1???
#define MAKE_HOOK(x) MAKE_HOOK_HIDDEN1(x).setSubs((void*)x, (void*)MAKE_HOOK_HIDDEN2(x)); \
                     MAKE_HOOK_HIDDEN1(x).installHook();
#define MAKE_HOOK_HIDDEN1(x) hook_##x
#define MAKE_HOOK_HIDDEN2(x) custom_##x

#define SYNTHESIA_VER_MAJ 9
#define SYNTHESIA_VER_MIN 0

// gl constants defined here so i don't have to require glfw
#define GL_VIEWPORT 0x0BA2
#define GL_BGRA 0x80E1
#define GL_UNSIGNED_BYTE 0x1401

// offsets for synthesia's executable, can easily be adjusted
// relative to base address
#if SYNTHESIA_VER_MAJ == 10 && SYNTHESIA_VER_MIN == 1
// 10.1.3320
#define USE_QPC
#define GLSWAP_PTR 0x539938
#define HDC_OFFSET 0x2C
#define GAMESTATE_CTOR_PTR 0x18A2B0
#define PLAYINGSTATE_CTOR_PTR 0x18B110
#define QPC_CALL 0x1FBAB8
#define QPF_CALL 0x1FBAAD
#elif SYNTHESIA_VER_MAJ == 9 && SYNTHESIA_VER_MIN == 0
// 9.0.2495
#define STDCALL_GAMESTATE_CTOR
#define GLSWAP_PTR 0x4E83E0
#define HDC_OFFSET 0x34
#define GAMESTATE_CTOR_PTR 0x1053A0
#define PLAYINGSTATE_CTOR_PTR 0x105BA0
#define TGT_CALL 0x1B2334 // older synthesia uses timeGetTime instead of QueryPerformanceCounter
#elif
#error invalid version!
#endif

HANDLE video_pipe = NULL;
int gl_viewport[4];
int vp_width;
int vp_height;

bool resolution_shown = false;
bool ingame = false;

char* base_addr = nullptr;

void* fb_data = nullptr;

#ifdef USE_QPC
LARGE_INTEGER fake_qpc;
__int64 fake_qpc_interval;
#else
DWORD fake_tgt;
#endif

C_Hook hook_gamestate_ctor;
#ifdef STDCALL_GAMESTATE_CTOR
auto gamestate_ctor = (void(__stdcall*)(void*, void*))nullptr;
#else
auto gamestate_ctor = (void(__thiscall*)(void*, DWORD*))nullptr;
#endif

C_Hook hook_playingstate_ctor;
auto playingstate_ctor = (void(__thiscall*)(void*))nullptr;

auto glFlush = (void(__stdcall*)())nullptr;
auto glGetIntegerv = (void(__stdcall*)(DWORD, int*))nullptr;
auto glReadPixels = (void(__stdcall*)(int, int, DWORD, DWORD, DWORD, DWORD, void*))nullptr;
auto _wglGetProcAddress = (PROC(WINAPI*)(LPCSTR))nullptr;

void fail() {
    printf("exiting in 5 seconds...\n");
    Sleep(5000);
    exit(1);
}

#ifdef USE_QPC
BOOL __stdcall custom_qpc(LARGE_INTEGER* out) {
    /*
    LARGE_INTEGER qpc_ret;
    bool ret = rtl_qpc(&qpc_ret);
    if (!ret)
        return false;
    out->QuadPart = qpc_ret.QuadPart / 2;
    */
    *out = fake_qpc;
    return true;
}

static auto custom_qpc_ptr = &custom_qpc;

BOOL __stdcall custom_qpf(LARGE_INTEGER* out) {
    LARGE_INTEGER ret;
    ret.QuadPart = 60;
    *out = ret;
    return true;
}

static auto custom_qpf_ptr = &custom_qpf;
#else
DWORD __stdcall custom_tgt() {
    return fake_tgt;
}

static auto custom_tgt_ptr = &custom_tgt;
#endif

// supposed to be __thiscall, but i have to use this hacky workaround
BOOL __fastcall custom_glswap(char* thisptr, void*) {
#ifdef USE_QPC
    fake_qpc.QuadPart += 1;
#else
    fake_tgt += 16; // 62.5 fps
#endif

    if (!resolution_shown) {
        // all of this has to be in this function because getting the viewport during normal init won't work
        // something to do with being in the middle of glBegin
        /*
        printf("disabling vsync\n");
        auto wglSwapIntervalEXT = (BOOL(WINAPI*)(int))_wglGetProcAddress("wglSwapIntervalEXT");
        wglSwapIntervalEXT(0);
        */

        glGetIntegerv(GL_VIEWPORT, gl_viewport);
        vp_width = gl_viewport[2];
        vp_height = gl_viewport[3];
        printf("creating the pipe\n");
        video_pipe = CreateNamedPipe(TEXT("\\\\.\\pipe\\synthesiaframedump"),
            PIPE_ACCESS_OUTBOUND,
            PIPE_TYPE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            static_cast<DWORD>(vp_width * vp_height * 4 * 120),
            0,
            0,
            nullptr);
        if (!video_pipe) {
            printf("failed to create the pipe! getlasterror: 0x%x\n", GetLastError());
            fail();
        }
        fb_data = malloc(vp_width * vp_height * 4);
        if (!fb_data) {
            printf("failed to allocate the framebuffer copy!\n");
            fail();
        }
        printf("detected resolution as %dx%d. if you want to change resolutions, close synthesia and edit your config.xml manually\n", vp_width, vp_height);
        printf("***do not resize the window after injecting the dll!!!***\n");
        printf("start ffmpeg and play a song when it's ready\n");
        resolution_shown = true;
    }

    glFlush();

    if (ingame) {
        glReadPixels(0, 0, vp_width, vp_height, GL_BGRA, GL_UNSIGNED_BYTE, fb_data);
        WriteFile(video_pipe, fb_data, static_cast<DWORD>(vp_width * vp_height * 4), nullptr, nullptr);
    }

    SwapBuffers(*(HDC*)(thisptr + HDC_OFFSET));
}

// same applies here
#ifdef STDCALL_GAMESTATE_CTOR
void __stdcall custom_gamestate_ctor(void* thisptr, void* unk) {
#else
void __fastcall custom_gamestate_ctor(void* thisptr, void*, DWORD* unk) {
#endif
    hook_gamestate_ctor.removeHook();
    
    printf("going ingame\n");
    ingame = true;
    gamestate_ctor(thisptr, unk);
}

void __fastcall custom_playingstate_ctor(void* thisptr, void*) {
    hook_playingstate_ctor.removeHook();

    printf("stopping\n");
    ingame = false;

    playingstate_ctor(thisptr);
}

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH: {
        AllocConsole();
        freopen("CONIN$", "r", stdin);
        freopen("CONOUT$", "w", stdout);
        SetConsoleTitle(L"synthesiaframedump");

        printf("synthesiaframedump %s\n", __DATE__);
        printf("compiled for synthesia %d.%d\n", SYNTHESIA_VER_MAJ, SYNTHESIA_VER_MIN);

        printf("***make sure that you're running synthesia in opengl and NOT directx9, or else this won't work!!!***\n");

#ifdef USE_QPC
        printf("getting initial performance counter\n");
        bool qpc_ret = QueryPerformanceCounter(&fake_qpc);
        if (!qpc_ret) {
            printf("failed to get the initial performance counter! getlasterror: 0x%x\n", GetLastError());
            fail();
        }
#else
        printf("getting initial system time\n");
        fake_tgt = timeGetTime();
#endif

        // synthesia has aslr on
        printf("getting synthesia's base address\n");
        base_addr = (char*)GetModuleHandle(NULL);
        if (!base_addr) {
            printf("failed to get the base address! getlasterror: 0x%x\n", GetLastError());
            fail();
        }
        printf("base_addr: 0x%p\n", base_addr);
#ifdef USE_QPC
        printf("replacing QueryPerformanceCounter call\n");
        BYTE qpc_patch_bytes[] = { 0xFF, 0x15, 0x00, 0x00, 0x00, 0x00 };
        auto custom_qpc_ptr_ptr = &custom_qpc_ptr;
        memcpy(&qpc_patch_bytes[2], &custom_qpc_ptr_ptr, 4);
        auto qpc_patch = QPatch((void*)(base_addr + QPC_CALL), (BYTE*)&qpc_patch_bytes, sizeof(qpc_patch_bytes));
        qpc_patch.patch();

        // could reuse qpc_patch_bytes but that's less readable
        BYTE qpf_patch_bytes[] = { 0xFF, 0x15, 0x00, 0x00, 0x00, 0x00 };
        printf("replacing QueryPerformanceFrequency call\n");
        auto custom_qpf_ptr_ptr = &custom_qpf_ptr;
        memcpy(&qpf_patch_bytes[2], &custom_qpf_ptr_ptr, 4);
        auto qpf_patch = QPatch((void*)(base_addr + QPF_CALL), (BYTE*)&qpf_patch_bytes, sizeof(qpf_patch_bytes));
        qpf_patch.patch();
#else
        printf("replacing timeGetTime call\n");
        BYTE tgt_patch_bytes[] = { 0xFF, 0x15, 0x00, 0x00, 0x00, 0x00 };
        auto custom_tgt_ptr_ptr = &custom_tgt_ptr;
        memcpy(&tgt_patch_bytes[2], &custom_tgt_ptr_ptr, 4);
        auto tgt_patch = QPatch((void*)(base_addr + TGT_CALL), (BYTE*)&tgt_patch_bytes, sizeof(tgt_patch_bytes));
        tgt_patch.patch();
#endif

        printf("getting opengl functions\n");
        auto ogl32_handle = GetModuleHandle(L"opengl32.dll");
        if (!ogl32_handle) {
            printf("failed to get a handle to opengl32.dll! getlasterror: 0x%x\n", GetLastError());
            fail();
        }
        // no reason for it to fail now if it could get the dll's address...
        glFlush = (void(__stdcall*)())GetProcAddress(ogl32_handle, "glFlush");
        glGetIntegerv = (void(__stdcall*)(DWORD, int*))GetProcAddress(ogl32_handle, "glGetIntegerv");
        glReadPixels = (void(__stdcall*)(int, int, DWORD, DWORD, DWORD, DWORD, void*))GetProcAddress(ogl32_handle, "glReadPixels");
        _wglGetProcAddress = (PROC(WINAPI*)(LPCSTR))GetProcAddress(ogl32_handle, "wglGetProcAddress");

        printf("overwriting RendererOpenGl vtable entry\n");
        auto custom_glswap_ptr = custom_glswap;
        QPatch glswap_patch((void*)(base_addr + GLSWAP_PTR), (BYTE*)&custom_glswap_ptr, sizeof(custom_glswap_ptr));
        glswap_patch.patch();

        printf("hooking GameState's constructor\n");
#ifdef STDCALL_GAMESTATE_CTOR
        gamestate_ctor = (void(__stdcall*)(void*, void*))(base_addr + GAMESTATE_CTOR_PTR);
#else
        gamestate_ctor = (void(__thiscall*)(void*, DWORD*))(base_addr + GAMESTATE_CTOR_PTR);
#endif
        MAKE_HOOK(gamestate_ctor);

        printf("hooking PlayingState's constructor\n");
        playingstate_ctor = (void(__thiscall*)(void*))(base_addr + PLAYINGSTATE_CTOR_PTR);
        MAKE_HOOK(playingstate_ctor);

        break;
    }
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

