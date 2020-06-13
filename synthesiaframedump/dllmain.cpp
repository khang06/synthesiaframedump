#include <Windows.h>
#include "Hooks.h"

extern "C" __declspec(dllexport) void dummyexport() {}

// why do i even have to make 3 macros for 1???
#define MAKE_HOOK(x) MAKE_HOOK_HIDDEN1(x).setSubs((void*)x, (void*)MAKE_HOOK_HIDDEN2(x)); \
                     MAKE_HOOK_HIDDEN1(x).installHook();
#define MAKE_HOOK_HIDDEN1(x) hook_##x
#define MAKE_HOOK_HIDDEN2(x) custom_##x

// gl constants defined here so i don't have to require glfw
#define GL_VIEWPORT 0x0BA2
#define GL_BGRA 0x80E1
#define GL_UNSIGNED_BYTE 0x1401

// offsets for synthesia 10.1.3320, can easily be adjusted
// relative to base address
#define GLSWAP_PTR 0x539938
#define HDC_OFFSET 0x2C
#define GAMESTATE_CTOR_PTR 0x18A2B0
#define PLAYINGSTATE_CTOR_PTR 0x18B110

HANDLE video_pipe = NULL;
int gl_viewport[4];
int vp_width;
int vp_height;

bool resolution_shown = false;
bool ingame = false;

void* fb_data = nullptr;

C_Hook hook_qpc;
auto rtl_qpc = (BOOL(__stdcall*)(LARGE_INTEGER*))nullptr;
LARGE_INTEGER fake_qpc;
__int64 fake_qpc_interval;

C_Hook hook_gamestate_ctor;
auto gamestate_ctor = (void(__thiscall*)(void*, DWORD*))nullptr;

C_Hook hook_playingstate_ctor;
auto playingstate_ctor = (void(__thiscall*)(void*))nullptr;

auto glFlush = (void(__stdcall*)())nullptr;
auto glGetIntegerv = (void(__stdcall*)(DWORD, int*))nullptr;
auto glReadPixels = (void(__stdcall*)(int, int, DWORD, DWORD, DWORD, DWORD, void*))nullptr;

void fail() {
    printf("exiting in 5 seconds...\n");
    Sleep(5000);
    exit(1);
}

BOOL __stdcall custom_qpc(LARGE_INTEGER* out) {
    /*
    LARGE_INTEGER qpc_ret;
    bool ret = rtl_qpc(&qpc_ret);
    if (!ret)
        return false;
    out->QuadPart = qpc_ret.QuadPart / 2;
    */
    fake_qpc.QuadPart += fake_qpc_interval;
    *out = fake_qpc;
    return true;
}

// supposed to be __thiscall, but i have to use this hacky workaround
BOOL __fastcall custom_glswap(char* thisptr, void*) {
    fake_qpc.QuadPart += fake_qpc_interval;

    if (!resolution_shown) {
        // all of this has to be in this function because getting the viewport during normal init won't work
        // something to do with being in the middle of glBegin
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
void __fastcall custom_gamestate_ctor(void* thisptr, void*, DWORD* unk) {
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

        printf("synthesiaframedump ");
        printf(__DATE__);
        printf("\n");

        printf("***make sure that you're running synthesia in opengl and NOT directx9, or else this won't work!!!***\n");

        printf("getting initial performance counter\n");
        bool qpc_ret = QueryPerformanceCounter(&fake_qpc);
        if (!qpc_ret) {
            printf("failed to get the initial performance counter! getlasterror: 0x%x\n", GetLastError());
            fail();
        }

        printf("getting performance counter frequency\n");
        LARGE_INTEGER pf;
        bool qpf_ret = QueryPerformanceFrequency(&pf);
        if (!qpf_ret) {
            printf("failed to get the performance counter frequency! getlasterror: 0x%x\n", GetLastError());
            fail();
        }
        //fake_qpc_interval = pf.QuadPart / 60;
        fake_qpc_interval = 20;
        //printf("%lld\n", fake_qpc_interval);

        // i don't want to flush instruction cache by patching stuff extremely often so this won't unhook on call
        printf("hooking QueryPerformanceCounter\n");
        auto qpc = ((BOOL(__stdcall*)(LARGE_INTEGER*))GetProcAddress(GetModuleHandle(L"kernel32.dll"), "QueryPerformanceCounter"));
        MAKE_HOOK(qpc);
        rtl_qpc = ((BOOL(__stdcall*)(LARGE_INTEGER*))GetProcAddress(GetModuleHandle(L"ntdll.dll"), "RtlQueryPerformanceCounter"));

        // synthesia has aslr on
        printf("getting synthesia's base address\n");
        char* base_addr = (char*)GetModuleHandle(NULL);
        if (!base_addr) {
            printf("failed to get the base address! getlasterror: 0x%x\n", GetLastError());
            fail();
        }
        printf("base_addr: 0x%p\n", base_addr);

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

        printf("overwriting RendererOpenGl vtable entry\n");
        auto custom_glswap_ptr = custom_glswap;
        QPatch glswap_patch((void*)(base_addr + GLSWAP_PTR), (BYTE*)&custom_glswap_ptr, sizeof(custom_glswap_ptr));
        glswap_patch.patch();

        printf("hooking GameState's constructor\n");
        gamestate_ctor = (void(__thiscall*)(void*, DWORD*))(base_addr + GAMESTATE_CTOR_PTR);
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

