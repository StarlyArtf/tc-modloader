// Read sampled instruction pointers directly from a local WPR ETL. The older
// xperf installed on this workstation fails on unrelated modern providers.
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <cstdio>
#include <cstdint>
#include <map>
#include <string>
static DWORD wantedThread=0;
static int64_t traceStart=0;
static std::map<uint64_t,uint64_t> samples;
static uint64_t total=0;
static void WINAPI eventRecord(EVENT_RECORD* event) {
    // Classic PerfInfo SampledProfile, opcode 46: pointer-sized IP followed
    // by the sampled thread ID. Reject short records before decoding.
    const GUID perf={0xce1dbfb4,0x137e,0x4da6,{0x87,0xb0,0x3f,0x59,0xaa,0x10,0x2c,0xbc}};
    if(event->EventHeader.ProviderId!=perf || event->EventHeader.EventDescriptor.Opcode!=46 || event->UserDataLength<12 ||
       (event->EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER))return;
    uint64_t ip=0;DWORD thread=0;
    memcpy(&ip,event->UserData,8);memcpy(&thread,static_cast<char*>(event->UserData)+8,4);
    const auto elapsed=(event->EventHeader.TimeStamp.QuadPart-traceStart)/10000000.0;
    if(thread!=wantedThread || elapsed<25 || elapsed>37)return;
    ++samples[ip];++total;
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=3)return 2;
    wantedThread=static_cast<DWORD>(wcstoul(argv[2],nullptr,10));
    EVENT_TRACE_LOGFILEW trace{};trace.LogFileName=argv[1];
    trace.ProcessTraceMode=PROCESS_TRACE_MODE_EVENT_RECORD;
    trace.EventRecordCallback=&eventRecord;
    TRACEHANDLE handle=OpenTraceW(&trace);
    if(handle==INVALID_PROCESSTRACE_HANDLE)return 3;
    traceStart=trace.LogfileHeader.StartTime.QuadPart;
    const ULONG result=ProcessTrace(&handle,1,nullptr,nullptr);
    CloseTrace(handle);
    if(result!=ERROR_SUCCESS) {std::fprintf(stderr,"ProcessTrace error %lu\n",result);return 4;}
    std::printf("IP,Count\n");
    for(auto [ip,count]:samples)std::printf("0x%llx,%llu\n",static_cast<unsigned long long>(ip),static_cast<unsigned long long>(count));
    std::fprintf(stderr,"thread=%lu samples=%llu\n",wantedThread,static_cast<unsigned long long>(total));
    return total?0:5;
}
