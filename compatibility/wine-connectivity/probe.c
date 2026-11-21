#include "initguid.h"
#include "private.h"
#include <stdio.h>
#include <stdarg.h>

static FILE *report;
static void record(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(report, format, args);
    va_end(args);
    fflush(report);
}

int main(void)
{
    report = fopen("Z:\\opt\\game\\probe-result.txt", "wb");
    if (!report) return 2;
    record("START: networking API probe\n");
    HMODULE module;
    HRESULT (WINAPI *get_factory)(HSTRING,IActivationFactory **);
    HSTRING name;
    IActivationFactory *factory;
    INetworkInformationStatics *statics;
    IConnectionProfile *profile;
    IConnectionCost *cost;
    NetworkCostType type;
    boolean flag;
    HRESULT hr;
#define CHECK(x) do { record("CALL: %s\n", #x); hr=(x); if(FAILED(hr)){record("FAIL %s: 0x%08lx\n",#x,(unsigned long)hr);return 1;} }while(0)
    CHECK(CoInitializeEx(NULL, COINIT_MULTITHREADED));
    module=LoadLibraryW(L"Z:\\opt\\game\\windows.networking.connectivity.dll");
    if(!module){record("FAIL LoadLibrary: %lu\n",GetLastError());return 1;}
    UINT (WINAPI *fix_version)(void) = (void *)GetProcAddress(module,"VrcmConnectivityFixVersion");
    if (!fix_version || fix_version() != 1)
    { record("FAIL: Wine loaded an unpatched library instead of the replacement.\n"); return 1; }
    record("IDENTITY: VRConMac connectivity fix version 1 verified\n");
    WCHAR module_path[1024];
    if (GetModuleFileNameW(module, module_path, ARRAYSIZE(module_path)))
        record("MODULE: %ls\n", module_path);
    get_factory=(void *)GetProcAddress(module,"DllGetActivationFactory");
    if(!get_factory){record("FAIL: activation factory export missing\n");return 1;}
    const WCHAR cls[]=L"Windows.Networking.Connectivity.NetworkInformation";
    CHECK(WindowsCreateString(cls,ARRAYSIZE(cls)-1,&name));
    CHECK(get_factory(name,&factory));
    CHECK(IActivationFactory_QueryInterface(factory,&IID_INetworkInformationStatics,(void **)&statics));
    CHECK(INetworkInformationStatics_GetInternetConnectionProfile(statics,&profile));
    if(!profile){record("FAIL: No active internet profile; cannot verify cost path.\n");return 1;}
    if(IConnectionProfile_GetConnectionCost(profile,NULL)!=E_POINTER){record("FAIL: missing null-pointer validation\n");return 1;}
    CHECK(IConnectionProfile_GetConnectionCost(profile,&cost));
    CHECK(IConnectionCost_get_NetworkCostType(cost,&type));
    CHECK(IConnectionCost_get_Roaming(cost,&flag));
    CHECK(IConnectionCost_get_OverDataLimit(cost,&flag));
    CHECK(IConnectionCost_get_ApproachingDataLimit(cost,&flag));
    IConnectionCost_Release(cost);IConnectionProfile_Release(profile);
    INetworkInformationStatics_Release(statics);IActivationFactory_Release(factory);
    WindowsDeleteString(name);FreeLibrary(module);CoUninitialize();
    record("PASS: GetConnectionCost and all billing properties succeeded (cost type %d).\n",type);
    fclose(report);
    return 0;
}
