// host_load.cpp — minimal harness that mimics what After Effects does at scan time:
// dlopen the plugin, call PluginDataEntryFunction2, and capture the effect it registers.
// Proves the native binary loads and its entry point executes on the host architecture.
#define MAC_ENV 1
#include "AEConfig.h"
#include "AE_Effect.h"
#include "AE_PluginData.h"
#include <dlfcn.h>
#include <cstdio>
#include <cstring>

static char g_name[256]     = {0};
static char g_match[256]    = {0};
static char g_category[256] = {0};
static char g_entry[256]    = {0};
static A_long g_kind = 0, g_major = 0, g_minor = 0;
static int g_registered = 0;

// Matches PF_PluginDataCB2 from AE_PluginData.h
static A_Err RecordCB(
    PF_PluginDataPtr, const A_u_char* name, const A_u_char* match,
    const A_u_char* category, const A_u_char* entry, A_long kind,
    A_long major, A_long minor, A_long, const A_u_char* /*url*/) {
    std::strncpy(g_name,     (const char*)name,     sizeof(g_name) - 1);
    std::strncpy(g_match,    (const char*)match,    sizeof(g_match) - 1);
    std::strncpy(g_category, (const char*)category, sizeof(g_category) - 1);
    std::strncpy(g_entry,    (const char*)entry,    sizeof(g_entry) - 1);
    g_kind = kind; g_major = major; g_minor = minor;
    g_registered++;
    return 0; // A_Err_NONE
}

typedef A_Err (*EntryFn)(PF_PluginDataPtr, PF_PluginDataCB2, void*, const char*, const char*);

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: %s <path-to-macho>\n", argv[0]); return 2; }
    const char* path = argv[1];

    void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { std::printf("FAIL: dlopen: %s\n", dlerror()); return 1; }
    std::printf("ok:   dlopen loaded native binary\n");

    EntryFn entry = (EntryFn)dlsym(h, "PluginDataEntryFunction2");
    if (!entry) { std::printf("FAIL: dlsym PluginDataEntryFunction2: %s\n", dlerror()); return 1; }
    std::printf("ok:   found PluginDataEntryFunction2\n");

    A_Err err = entry((PF_PluginDataPtr)0x1, &RecordCB, nullptr, "AEHost", "26.0");
    std::printf("ok:   entry point executed, returned %d\n", (int)err);

    int fail = 0;
    if (!g_registered)                 { std::printf("FAIL: nothing registered\n"); fail = 1; }
    if (std::strcmp(g_name, "Specularity"))                    { std::printf("FAIL: name '%s'\n", g_name); fail = 1; }
    if (std::strcmp(g_match, "com.cosmo88.ae.Specularity"))       { std::printf("FAIL: match '%s'\n", g_match); fail = 1; }
    if (g_kind != 'eFKT')              { std::printf("FAIL: kind 0x%x\n", (unsigned)g_kind); fail = 1; }

    std::printf("\nRegistered effect:\n");
    std::printf("  Name:      %s\n", g_name);
    std::printf("  MatchName: %s\n", g_match);
    std::printf("  Category:  %s\n", g_category);
    std::printf("  Entry:     %s\n", g_entry);
    std::printf("  Kind:      %c%c%c%c   API %d.%d\n",
        (g_kind>>24)&0xff,(g_kind>>16)&0xff,(g_kind>>8)&0xff,g_kind&0xff, (int)g_major,(int)g_minor);

    dlclose(h);
    std::printf("\n== %s ==\n", fail ? "FAILURES" : "LOAD TEST PASSED");
    return fail;
}
