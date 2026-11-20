// Host preview: fake LIPC. Prints every call to stdout and keeps a tiny property map.
#include "openlipc/openlipc.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

static std::map<std::string, int> g_int = { {"com.lab126.powerd/flIntensity", 7} };
static std::map<std::string, std::string> g_str;
static int g_dummy;

static std::string key(const char* s, const char* p) { return std::string(s ? s : "") + "/" + (p ? p : ""); }

extern "C" {

LIPC* LipcOpenNoName(void) { printf("LIPC open (noname)\n"); fflush(stdout); return &g_dummy; }
LIPC* LipcOpen(const char* service) { printf("LIPC open %s\n", service ? service : "(null)"); fflush(stdout); return &g_dummy; }
LIPC* LipcOpenEx(const char* service, LIPCcode* code) { if (code) *code = LIPC_OK; return LipcOpen(service); }
void LipcClose(LIPC*) { printf("LIPC close\n"); fflush(stdout); }
const char* LipcGetErrorString(LIPCcode) { return "fake"; }

LIPCcode LipcSetIntProperty(LIPC*, const char* service, const char* prop, int value) {
    g_int[key(service, prop)] = value;
    printf("LIPC set %s %s=%d\n", service, prop, value); fflush(stdout);
    return LIPC_OK;
}
LIPCcode LipcGetIntProperty(LIPC*, const char* service, const char* prop, int* value) {
    std::map<std::string, int>::iterator it = g_int.find(key(service, prop));
    int v = it == g_int.end() ? 0 : it->second;
    if (value) *value = v;
    printf("LIPC get %s %s -> %d\n", service, prop, v); fflush(stdout);
    return LIPC_OK;
}
LIPCcode LipcSetStringProperty(LIPC*, const char* service, const char* prop, const char* value) {
    g_str[key(service, prop)] = value ? value : "";
    printf("LIPC set %s %s=%s\n", service, prop, value ? value : ""); fflush(stdout);
    return LIPC_OK;
}
LIPCcode LipcGetStringProperty(LIPC*, const char* service, const char* prop, char** value) {
    std::map<std::string, std::string>::iterator it = g_str.find(key(service, prop));
    std::string v = it == g_str.end() ? "" : it->second;
    if (value) *value = strdup(v.c_str());
    printf("LIPC get %s %s -> %s\n", service, prop, v.c_str()); fflush(stdout);
    return LIPC_OK;
}
void LipcFreeString(char* s) { free(s); }

}
