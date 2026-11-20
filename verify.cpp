#include <Windows.h>
#include <Psapi.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

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
    std::printf("pid=%lu client=0x%llx\n",pid,(unsigned long long)client);

    for(uintptr_t off : { (uintptr_t)0x21BF9B0, (uintptr_t)0x2709010 }){
        uintptr_t p=rd<uintptr_t>(client+off);
        int counts[3]={0,0,0};
        for(int cb : {0,1,2}){
            int base = cb==0?0:(cb==1?8:0x10);
            for(int b=0;b<32;b++){
                uintptr_t chunk=rd<uintptr_t>(p+base+8i64*b);
                if(!good(chunk))break;
                static uint8_t blob[0xE000];
                if(!rp(chunk,blob,sizeof blob))break;
                for(int j=0;j<512;j++){
                    uintptr_t e=0;std::memcpy(&e,blob+0x70*j,8);
                    if(!good(e))continue;
                    uintptr_t sn=rd<uintptr_t>(e+0x330);
                    if(good(sn)&&rd<uintptr_t>(sn+0x30)==e)counts[cb]++;
                }
            }
        }
        std::printf("entlist client+0x%llx -> 0x%llx pawns[base0=%d base8=%d base10=%d]\n",
            (unsigned long long)off,(unsigned long long)p,counts[0],counts[1],counts[2]);
    }

    for(uintptr_t off : { (uintptr_t)0x21C6E60, (uintptr_t)0x252B0C8 }){
        uintptr_t c=rd<uintptr_t>(client+off);
        char name[64]={}; rp(c+0x6FC,name,48);
        uint8_t local=rd<uint8_t>(c+0x790);
        std::printf("ctrl client+0x%llx -> 0x%llx name='%s' local=%u\n",
            (unsigned long long)off,(unsigned long long)c,name,local);
    }

    for(uintptr_t off : { (uintptr_t)0x255D350, (uintptr_t)0x255D390, (uintptr_t)0x255D3D0, (uintptr_t)0x256B708, (uintptr_t)0x256B748, (uintptr_t)0x256B788 }){
        float m[16]{}; rp(client+off,m,sizeof m);
        std::printf("mat client+0x%llx: [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f]\n",
            (unsigned long long)off,m[0],m[1],m[2],m[3],m[4],m[5],m[6],m[7],m[8],m[9],m[10],m[11]);
    }
    return 0;
}
