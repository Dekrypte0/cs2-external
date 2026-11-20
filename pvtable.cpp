#include <Windows.h>
#include <Psapi.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "Psapi.lib")
static HANDLE gh; static uintptr_t client;
static bool rp(uintptr_t a, void* b, size_t n){return a&&ReadProcessMemory(gh,(LPCVOID)a,b,n,nullptr)!=0;}
template<class T> static T rd(uintptr_t a){T v{};rp(a,&v,sizeof v);return v;}
static bool good(uintptr_t p){return p>=0x10000&&p<=0x7FFFFFFFFFFFULL;}

int main(){
    HANDLE s=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    PROCESSENTRY32W pe{};pe.dwSize=sizeof(pe);DWORD pid=0;
    if(Process32FirstW(s,&pe))do{if(!_wcsicmp(pe.szExeFile,L"cs2.exe")){pid=pe.th32ProcessID;break;}}while(Process32NextW(s,&pe));
    CloseHandle(s);
    gh=OpenProcess(PROCESS_VM_READ|PROCESS_QUERY_INFORMATION,FALSE,pid);
    HMODULE mods[2048];DWORD need;EnumProcessModulesEx(gh,mods,sizeof mods,&need,LIST_MODULES_ALL);
    wchar_t nm[MAX_PATH];
    for(DWORD i=0;i<need/sizeof(HMODULE);i++){
        GetModuleFileNameExW(gh,mods[i],nm,MAX_PATH);
        wchar_t* sl=wcsrchr(nm,L'\\');
        if(sl&&!_wcsicmp(sl+1,L"client.dll")){client=(uintptr_t)mods[i];break;}
    }
    uintptr_t sys=rd<uintptr_t>(client+0x21BF9B0);
    for(int b=0;b<32;b++){
        uintptr_t chunk=rd<uintptr_t>(sys+0x10+8i64*b);
        if(!good(chunk))break;
        static uint8_t blob[0xE000];
        if(!rp(chunk,blob,sizeof blob))break;
        for(int j=0;j<512;j++){
            uintptr_t e=0;std::memcpy(&e,blob+0x70*j,8);
            if(!good(e))continue;
            uintptr_t sn=rd<uintptr_t>(e+0x330);
            if(!good(sn)||rd<uintptr_t>(sn+0x30)!=e)continue;
            int hp=rd<int>(e+0x34C);
            int idx=(b<<9)|j;
            uintptr_t vt=rd<uintptr_t>(e);
            uint32_t hc=rd<uint32_t>(e+0x14BC);
            std::printf("idx=%-5d hp=%4d vtrel=0x%llx hctrl=0x%x slot=%u\n",
                idx,hp,(unsigned long long)(vt-client),hc,hc&0x3FFF);
        }
    }
    return 0;
}
