"""Compile and execute the real early wrapper against a bounded SDK fixture.

Original exception pointers live at actual S3 DRAM addresses using host mmap.
This checks capture, invalid-pointer behavior, nested forwarding and retention;
it cannot establish on-device SDK panic entry, RTC retention or physical reset.
"""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SDK = r"""
#pragma once
#include <cstdint>
#define IRAM_ATTR
constexpr int PANIC_EXCEPTION_DEBUG = 0;
constexpr int PANIC_EXCEPTION_ABORT = 3;
extern bool g_panic_abort;
struct panic_info_t {
 int core; int exception; const char* reason; const char* description;
 void (*details)(const void*); void (*state)(const void*);
 const void* addr; const void* frame; bool pseudo_excause;
};
struct XtExcFrame {
 uint32_t exit,pc,ps,a0,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12,a13,a14,a15,sar,exccause,excvaddr;
};
"""
CONSOLE = r"""
#pragma once
#include <cstdio>
#include <string>
#include <string_view>
#include <functional>
#include <initializer_list>
#include <cstring>
namespace fxe {
class Console {
 public:
  struct Args {
    struct Arg { const char* ptr=nullptr; uint8_t len=0; };
    uint8_t argc=0; Arg argv[8]{}; bool overflow=false;
    bool Has(size_t i) const { return i<argc && argv[i].ptr; }
    std::string_view Get(size_t i) const { return Has(i)?std::string_view(argv[i].ptr,argv[i].len):std::string_view(); }
  };
  std::string output, commandName;
  std::function<bool(const Args&)> handler;
  bool registrationResult=true;
  template<typename Handler> bool RegisterCommand(const char* name,const char*,Handler callback) {
    commandName=name; handler=callback; return registrationResult;
  }
  bool Run(std::initializer_list<const char*> words, bool overflow=false) {
    Args args; args.overflow=overflow;
    for(const char* word:words) { args.argv[args.argc++]={word,static_cast<uint8_t>(std::strlen(word))}; }
    return handler(args);
  }
  template<typename... Args> void PrintLn(const char* format, Args... args) {
    char line[512]; std::snprintf(line,sizeof(line),format,args...);
    output += line; output += '\n';
  }
};
}
"""
MAIN = r"""
#include <sys/mman.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <type_traits>
#include "sdk.hpp"
#include "foxie_embedded/console.hpp"
#include "runtime/panic_capture_internal.hpp"
#include "runtime/progress_watchdog.hpp"
using namespace runtime::panic_diagnostic;
extern "C" void* volatile pxCurrentTCB[2] = {nullptr, nullptr};
extern "C" void __wrap_esp_panic_handler(panic_info_t*);
bool g_panic_abort = false;
std::atomic<unsigned> forwards{0};
bool nested = false;
struct Forwarded {};
extern "C" [[noreturn]] void __real_esp_panic_handler(panic_info_t* info) {
 const unsigned count = ++forwards;
 if (nested && count == 1U) {
   info->addr = reinterpret_cast<void*>(0x42009999U);
   try { __wrap_esp_panic_handler(info); } catch (Forwarded&) {}
 }
 throw Forwarded{};
}
void Invoke(panic_info_t* info) { try { __wrap_esp_panic_handler(info); } catch (Forwarded&) {} }
void Seal() { g_capsule.checksum = Checksum(g_capsule); g_capsule.magic = MAGIC; }
#define CHECK(v) do { if (!(v)) { std::printf("FAIL line %u: %s\n", __LINE__, #v); return 1; } } while (0)
int RunProbe(const char* mode);
int main(int argc, char** argv) {
 CHECK(argc == 2);
 if (std::strncmp(argv[1],"probe_",6U)==0) return RunProbe(argv[1]);
 static_assert(std::is_trivially_default_constructible<Capsule>::value, "no RTC constructor");
 static_assert(sizeof(Capsule) == 196U, "extended details preserve the retained ABI");
 const char* mode = argv[1];
 const auto Is = [&](const char* value) { return std::strcmp(mode,value) == 0; };
 auto* memory = mmap(reinterpret_cast<void*>(0x3fc88000U), 0x78000U,
    PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
 CHECK(memory == reinterpret_cast<void*>(0x3fc88000U));
 auto* info = reinterpret_cast<panic_info_t*>(0x3fc89000U);
 auto* frame = reinterpret_cast<XtExcFrame*>(0x3fcf0000U);
 const uint32_t sp = 0x3fcf1000U;
 *info = {1, 4, nullptr, nullptr, nullptr, nullptr, reinterpret_cast<void*>(0x42001234U), frame, false};
 *frame = {}; frame->pc = 0x42001234U; frame->a1 = sp;
 frame->a0 = 0x82002345U; frame->exccause = 28U; frame->excvaddr = 4U;
 auto* saved = reinterpret_cast<uint32_t*>(sp - 16U);
 saved[0] = 0U; saved[1] = sp + 32U;
 auto* tcb = reinterpret_cast<uint32_t*>(Is("tcb_upper") ? 0x3fcfffb0U : 0x3fcf2000U);
 std::memset(tcb,0,80U);tcb[0]=0x3fcdd170U;tcb[13]=0x3fcdd180U;tcb[18]=1U;tcb[19]=0x3fcde17fU;
 std::memcpy(tcb+14,"raw_task",9U);
 pxCurrentTCB[0] = pxCurrentTCB[1] = tcb;
 if (Is("debug") || Is("legacy24")) { info->exception = 0; info->pseudo_excause = true; frame->exccause = 1U; }
 if (Is("abort") || Is("abort_invalid")) g_panic_abort = true;
 if (Is("abort_invalid") || Is("bounds")) info->frame = reinterpret_cast<void*>(0x3fcffff0U);
 if (Is("pseudo")) info->pseudo_excause = true;
 if (Is("null")) info->frame = nullptr;
 if (Is("misaligned")) info->frame = reinterpret_cast<void*>(0x3fcf0001U);
 if (Is("bad_stack")) frame->a1 = 4U;
 if (Is("invalid_core")) info->core = 7;
 if (Is("tcb_null")) pxCurrentTCB[1] = nullptr;
 if (Is("tcb_misaligned")) pxCurrentTCB[1] = reinterpret_cast<void*>(0x3fcf2001U);
 if (Is("tcb_tail")) pxCurrentTCB[1] = reinterpret_cast<void*>(0x3fcfffb4U);
 if (Is("name16")) std::memcpy(tcb+14,"abcdefghijklmnop",16U);
 if (Is("nested")) nested = true;
 if (Is("limit")) {
   for (unsigned i=0; i<10U; ++i) {
     auto* words = reinterpret_cast<uint32_t*>(sp + 32U*i -16U);
     words[0] = 0x82002345U; words[1] = sp +32U*(i+1U);
   }
 }
 uint32_t preserved = 0U;
 uint32_t preservedWords[sizeof(Capsule)/4U]{};
 if (Is("details") || Is("legacy24") || Is("legacy_other") || Is("extended_partial") || Is("future") || Is("interrupted")) {
   Capture(g_capsule, Is("extended_partial") ? reinterpret_cast<panic_info_t*>(1U) : info,
           pxCurrentTCB, FaultRegisters{0x80000104U,0x3fcdd180U,0xc000001bU});
   if (Is("legacy24") || Is("legacy_other")) {
     g_capsule.flags = 24U;g_capsule.exception=0U;g_capsule.cause=0xffffffffU;
     auto* words = reinterpret_cast<volatile uint32_t*>(g_capsule.observations);
     for (unsigned i=0; i<sizeof(g_capsule.observations)/4U; ++i) words[i]=0U;
     if (Is("legacy_other")) g_capsule.observations[0].task=99U;
     Seal();
   }
   if (Is("future")) {g_capsule.flags=256U;Seal();}
   if (Is("interrupted")) g_capsule.magic=0U;
   if (Is("interrupted")) CHECK(ShouldCapture(g_capsule));
   else {
     CHECK(!ShouldCapture(g_capsule));preserved=g_capsule.checksum;
     const auto* words=reinterpret_cast<const volatile uint32_t*>(&g_capsule);
     for(unsigned i=0;i<sizeof(Capsule)/4U;++i) preservedWords[i]=words[i];
   }
 }
 if (Is("corrupt")) {g_capsule.magic=MAGIC;g_capsule.flags=Extended;g_capsule.checksum=42U;}
 if (Is("invalid")) info = reinterpret_cast<panic_info_t*>(1U);
 if (Is("concurrent")) {
   auto* second=reinterpret_cast<panic_info_t*>(0x3fc89100U);*second=*info;
   second->addr=reinterpret_cast<void*>(0x42005555U);
   std::thread first(Invoke,info), other(Invoke,second);first.join();other.join();
 } else Invoke(info);
 CHECK(forwards == ((nested || Is("concurrent")) ? 2U : 1U));
 CHECK(Valid(g_capsule));CHECK(g_capsule.knownTask == 0xffffffffU);
 if (preserved != 0U) {
   CHECK(g_capsule.checksum == preserved);
   const auto* words=reinterpret_cast<const volatile uint32_t*>(&g_capsule);
   for(unsigned i=0;i<sizeof(Capsule)/4U;++i) CHECK(words[i]==preservedWords[i]);
 }
 if (!Is("legacy24") && !Is("legacy_other") && !Is("future")) {
   CHECK(g_capsule.flags & Extended);
   for(unsigned i=0;i<2U;++i) CHECK(g_capsule.observations[i].revision==2U && g_capsule.observations[i].request==0U && g_capsule.observations[i].step==0U);
 }
 if (Is("extended_partial") || Is("invalid")) {
   CHECK((g_capsule.flags & (Extended|InvalidInfo|RawTcbInvalid))==(Extended|InvalidInfo|RawTcbInvalid));
 } else if (!Is("legacy24") && !Is("legacy_other") && !Is("future")) {
   CHECK(g_capsule.address == 0x42001234U || (Is("concurrent") && g_capsule.address==0x42005555U));
   const bool invalidTcb=Is("tcb_null")||Is("tcb_misaligned")||Is("tcb_tail")||Is("invalid_core");
   CHECK(bool(g_capsule.flags & RawTcbInvalid)==invalidTcb);
   if (!invalidTcb) {
     CHECK(DetailWord(g_capsule,4U)==tcb[0] && DetailWord(g_capsule,5U)==tcb[13]);
     CHECK(DetailWord(g_capsule,6U)==tcb[19] && DetailWord(g_capsule,7U)==tcb[18]);
     for(unsigned i=0;i<4U;++i) CHECK(DetailWord(g_capsule,8U+i)==tcb[14U+i]);
   }
   const bool invalidFrame=Is("abort_invalid")||Is("bounds")||Is("null")||Is("misaligned");
   CHECK(bool(g_capsule.flags & InvalidFrame)==invalidFrame);
   if (!invalidFrame) {
     CHECK(g_capsule.pc==0x42001234U && DetailWord(g_capsule,3U)==frame->exccause);
     const bool unknownCause=Is("debug")||Is("pseudo")||Is("abort");
     CHECK(g_capsule.cause==(unknownCause?0xffffffffU:28U));
     if(Is("bad_stack")) CHECK(g_capsule.flags & TruncatedTrace);
     else if(Is("limit")) CHECK(g_capsule.traceCount==8U && (g_capsule.flags & TruncatedTrace));
     else CHECK(g_capsule.traceCount==2U && g_capsule.tracePc[1]==0x42002345U && g_capsule.traceSp[1]==sp+32U);
   }
   if(Is("abort")||Is("abort_invalid")) CHECK((g_capsule.flags & Abort) && g_capsule.exception==3U);
   if(Is("debug")) CHECK(g_capsule.exception==0U && (g_capsule.flags & PseudoCause));
   if(Is("details")) CHECK(DetailWord(g_capsule,0U)==0x80000104U && DetailWord(g_capsule,1U)==0x3fcdd180U && DetailWord(g_capsule,2U)==0xc000001bU);
 }
 const uint32_t checksum = g_capsule.checksum;
#if defined(LITCH_BOOT_ENTRY_DIAGNOSTIC)
 // Exercise the production witness reader and Setup writer independently of
 // PAN2. This does not emulate the Xtensa entry or prove physical retention.
 litch_boot_entry_witness.magic = ENTRY_MAGIC;
 litch_boot_entry_witness.token = 0x12345678U;
 litch_boot_entry_witness.inverseToken = ~0x12345678U;
 litch_boot_entry_witness.stage = 1U;
 litch_boot_entry_witness.inverseStage = ~1U;
 litch_boot_entry_witness.resetState = (12U << 6U) | 15U;
 litch_boot_entry_witness.inverseResetState = ~litch_boot_entry_witness.resetState;
 // Exact pinned SDK encoding: marker bit31, hint9 in bits30:16 and14:0.
 litch_boot_entry_witness.resetHint = 0x80090009U;
 litch_boot_entry_witness.inverseResetHint = ~litch_boot_entry_witness.resetHint;
 litch_boot_entry_witness.brownOut = 0xc4000000U;
 litch_boot_entry_witness.inverseBrownOut = ~litch_boot_entry_witness.brownOut;
 CHECK(EntryWitnessValid(litch_boot_entry_witness));
 CHECK(EntryStageKnown(litch_boot_entry_witness));
 CHECK(EntryResetHintKnown(litch_boot_entry_witness));
 fxe::Console entryConsole; PrintRetained(entryConsole);
 CHECK(entryConsole.output.find("capture=app_entry_pre_sdk reset_state=0000030f reset_hint=80090009 brown_out=c4000000")!=std::string::npos);
 CHECK(entryConsole.output.find("cpu0_raw=15 cpu1_raw=12 hint_valid=1 hint_reason=9")!=std::string::npos);
 // The SDK ignores bit15 between the two 15-bit fields.
 litch_boot_entry_witness.resetHint = 0x80098009U;
 litch_boot_entry_witness.inverseResetHint = ~litch_boot_entry_witness.resetHint;
 CHECK(EntryResetHintKnown(litch_boot_entry_witness));
 // Raw RTC watchdog 9 is not the application's brownout enum 9. Invalid
 // duplicate hint halves are reported as unknown, not consumed as a reason.
 litch_boot_entry_witness.resetState = 9U;
 litch_boot_entry_witness.inverseResetState = ~9U;
 for (const uint32_t hint : {0x80048009U, 0x80040009U, 0x00090009U}) {
   // Former fixture's shifted copy, mismatched copies, and absent SDK marker.
   litch_boot_entry_witness.resetHint = hint;
   litch_boot_entry_witness.inverseResetHint = ~hint;
   CHECK(EntryWitnessValid(litch_boot_entry_witness) && !EntryResetHintKnown(litch_boot_entry_witness));
   fxe::Console watchdogConsole; PrintRetained(watchdogConsole);
   CHECK(watchdogConsole.output.find("cpu0_raw=9 cpu1_raw=0 hint_valid=0 hint_reason=unknown")!=std::string::npos);
 }
 for (unsigned index : {5U, 7U, 9U}) {
   auto* words = reinterpret_cast<volatile uint32_t*>(&litch_boot_entry_witness);
   words[index] ^= 1U;
   CHECK(!EntryWitnessValid(litch_boot_entry_witness));
   words[index] ^= 1U;
 }
 litch_boot_entry_witness.magic = 0x454e5431U; // Retired ENT1 cannot date ENT2.
 CHECK(!EntryWitnessValid(litch_boot_entry_witness));
 litch_boot_entry_witness.magic = ENTRY_MAGIC;
 NoteSetupEntry();
 CHECK(litch_boot_entry_witness.stage == 2U && EntryStageKnown(litch_boot_entry_witness));
 CHECK(Valid(g_capsule) && g_capsule.checksum == checksum);
 litch_boot_entry_witness.inverseStage = 0U;
 CHECK(EntryWitnessValid(litch_boot_entry_witness) && !EntryStageKnown(litch_boot_entry_witness));
 litch_boot_entry_witness.inverseToken = 0U;
 litch_boot_entry_witness.stage = 1U;
 CHECK(!EntryWitnessValid(litch_boot_entry_witness));
 NoteSetupEntry();
 CHECK(litch_boot_entry_witness.stage == 1U);
#endif
 runtime::RetainedProgress progress{};
 runtime::BeginProgressBoot(progress,6U);runtime::BeginProgressBoot(progress,1U);
 CHECK(Valid(g_capsule) && g_capsule.checksum==checksum);
 fxe::Console console;PrintRetained(console);
 CHECK(console.output.find("age=unknown elf=external_pin_required")!=std::string::npos);
 if(Is("legacy24")) CHECK(console.output.find("preservation=first_fault")!=std::string::npos);
 if(Is("name16")) CHECK(console.output.find("raw_inline_name_ascii=abcdefghijklmnop nul_terminated=0 task_identity=unknown")!=std::string::npos);
 if(Is("debug")) CHECK(console.output.find("semantic_scope=classified_debug")!=std::string::npos);
 if(Is("normal")) CHECK(console.output.find("semantic_scope=nondebug_do_not_classify")!=std::string::npos);
 if(Is("extended_partial")) CHECK(console.output.find("raw_tcb_fields=unavailable")!=std::string::npos && console.output.find("raw_frame_cause=unavailable")!=std::string::npos);
 CHECK(Readable(0x3fcffffcU,4U) && !Readable(0x3fcffffcU,8U));
 CHECK(!Readable(0x3fc88000U,static_cast<size_t>(-1)));
 g_capsule.pc^=1U;CHECK(!Valid(g_capsule));
 std::printf("PASS original fault-only capture %s\n",mode);
}
int RunProbe(const char* mode) {
 const auto Is=[&](const char* value){return std::strcmp(mode,value)==0;};
 static_assert(sizeof(StackProbe)==256U && std::is_trivially_default_constructible<StackProbe>::value,"no RTC constructor");
 auto* memory=mmap(reinterpret_cast<void*>(0x3fc88000U),0x78000U,
   PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0);
 CHECK(memory==reinterpret_cast<void*>(0x3fc88000U));
 auto* info=reinterpret_cast<panic_info_t*>(0x3fc89000U);
 auto* frame=reinterpret_cast<XtExcFrame*>(0x3fcf0000U);
 const uint32_t sp=0x3fcf1430U;
 *info={1,0,nullptr,nullptr,nullptr,nullptr,reinterpret_cast<void*>(0x403833deU),frame,true};
 *frame={};frame->pc=0x403833deU;frame->a1=sp;frame->a0=0x403818c3U;
 frame->ps=0x60025U;frame->sar=17U;frame->exccause=1U;
 auto* registers=&frame->a2;
 for(unsigned i=0;i<14U;++i) registers[i]=0x90000000U+i;
 auto* stack=reinterpret_cast<uint32_t*>(sp-16U);
 for(unsigned i=0;i<40U;++i) stack[i]=0xa0000000U+i;
 // Real observed windowed/call0 seam: first saved SP works, next saved
 // word is a call0 value, not a windowed link. Keep the raw continuation.
 stack[0]=0x40380000U;stack[1]=sp+48U;stack[13]=0U;
 auto* tcb=reinterpret_cast<uint32_t*>(0x3fcf2000U);
 std::memset(tcb,0,80U);tcb[0]=sp+48U;tcb[13]=0x3fcf141cU;tcb[19]=0x3fcf1810U;tcb[18]=1U;
 pxCurrentTCB[1]=tcb;
 fxe::Console console;
 if(Is("probe_registration")) console.registrationResult=false;
 RegisterStackProbeCommand(console);
 if(Is("probe_registration")) {
   CHECK(console.output.find("registration=failed")!=std::string::npos);
   CHECK(!ProbeValid(g_stackProbe) && !Valid(g_capsule));
   return 0;
 }
 CHECK(console.commandName=="panicprobe");
 CHECK(console.Run({"panicprobe","arm","10","0","0"}));
 CHECK(ProbeValid(g_stackProbe) && g_stackProbe.magic==PROBE_ARMED && !Valid(g_capsule));
 CHECK(console.output.find("retained=unavailable")<console.output.find("armed=1 token=00000010"));
 if(Is("probe_reject")) {
   uint32_t before[64]{};const auto* words=reinterpret_cast<const volatile uint32_t*>(&g_stackProbe);
   for(unsigned i=0;i<64U;++i) before[i]=words[i];
   for(const char* token:{"0","10","f","-1","100000000","0x","xyz"}) CHECK(!console.Run({"panicprobe","arm",token,"0","0"}));
   CHECK(!console.Run({"panicprobe","arm","11","1","0"}));
   CHECK(!console.Run({"panicprobe","arm","11","0","1"}));
   CHECK(!console.Run({"panicprobe","arm","11","0","0","extra"}));
   CHECK(!console.Run({"panicprobe","arm","11","0","0"},true));
   for(unsigned i=0;i<64U;++i) CHECK(before[i]==words[i]);
   CHECK(!Valid(g_capsule));
   return 0;
 }
 const bool unarmed=Is("probe_unarmed")||Is("probe_partial")||Is("probe_zero_arm")||
    Is("probe_unarmed_nested")||Is("probe_unarmed_bounds");
 if(Is("probe_unarmed")||Is("probe_unarmed_nested")||Is("probe_unarmed_bounds")) {
   auto* words=reinterpret_cast<volatile uint32_t*>(&g_stackProbe);
   for(unsigned i=0;i<64U;++i) words[i]=0U; // Cold/lost pre-reset RTC state.
 }
 if(Is("probe_zero_arm")) {
   g_stackProbe.token=0U;PublishProbe(g_stackProbe,PROBE_ARMED);
   CHECK(!ProbeValid(g_stackProbe)); // Valid checksum never permits ARM token0.
 }
 if(Is("probe_partial")) g_stackProbe.checksum^=1U;
 if(Is("probe_invalid_info")) info=reinterpret_cast<panic_info_t*>(1U);
 if(Is("probe_invalid_frame")||Is("probe_unarmed_bounds")) info->frame=reinterpret_cast<void*>(0x3fcffff0U);
 if(Is("probe_invalid_tcb")) pxCurrentTCB[1]=reinterpret_cast<void*>(0x3fcfffb4U);
 if(Is("probe_reversed")) tcb[19]=tcb[13]-1U;
 if(Is("probe_sp_outside")) tcb[13]=sp+4U;
 if(Is("probe_misaligned_base")) tcb[13]+=1U;
 if(Is("probe_misaligned_sp")) frame->a1=sp+1U;
 if(Is("probe_bad_end")) tcb[19]=0x60000000U;
 if(Is("probe_byte_end")) tcb[19]=sp+6U;
 if(Is("probe_base_clip")) tcb[13]=sp-4U;
 if(Is("probe_upper_bound")) {
   frame->a1=0x3fcffffcU;tcb[13]=frame->a1;tcb[19]=0x3fcfffffU;
   *reinterpret_cast<uint32_t*>(frame->a1)=0x12345678U;
 }
 if(Is("probe_magic")) {
   // Model interruption after publishing ARMED but before clearing the prior
   // PAN2: accepted marker states must not share a payload-only checksum.
   Capture(g_capsule,info,pxCurrentTCB,FaultRegisters{});
   g_stackProbe.panChecksum=g_capsule.checksum;g_stackProbe.panPc=g_capsule.pc;
   PublishProbe(g_stackProbe,PROBE_ARMED);
   CHECK(ProbeValid(g_stackProbe) && !ProbeBound(g_stackProbe,g_capsule));
   g_stackProbe.magic=PROBE_CAPTURED; // Exactly one flipped bit (0x200).
   CHECK(!ProbeValid(g_stackProbe) && !ProbeBound(g_stackProbe,g_capsule));
   g_stackProbe.magic=PROBE_ARMED;CaptureStackProbe(g_stackProbe,g_capsule);
   CHECK(ProbeBound(g_stackProbe,g_capsule));
   g_stackProbe.magic=PROBE_ARMED;
   CHECK(!ProbeValid(g_stackProbe) && !ProbeBound(g_stackProbe,g_capsule));
   return 0;
 }
 if(Is("probe_preserve")||Is("probe_preserve_pair")) {
   Capture(g_capsule,info,pxCurrentTCB,FaultRegisters{});
   if(Is("probe_preserve_pair")) CaptureStackProbe(g_stackProbe,g_capsule);
   CHECK(Valid(g_capsule) && ProbeValid(g_stackProbe));
   uint32_t priorPan[sizeof(Capsule)/4U]{},priorProbe[64]{};
   const auto* panWords=reinterpret_cast<const volatile uint32_t*>(&g_capsule);
   const auto* probeWords=reinterpret_cast<const volatile uint32_t*>(&g_stackProbe);
   for(unsigned i=0;i<sizeof(Capsule)/4U;++i) priorPan[i]=panWords[i];
   for(unsigned i=0;i<64U;++i) priorProbe[i]=probeWords[i];
   info->addr=reinterpret_cast<void*>(0x42009999U);frame->pc=0x42009999U;
   Invoke(info);
   for(unsigned i=0;i<sizeof(Capsule)/4U;++i) CHECK(priorPan[i]==panWords[i]);
   for(unsigned i=0;i<64U;++i) CHECK(priorProbe[i]==probeWords[i]);
   CHECK(ProbeBound(g_stackProbe,g_capsule)==Is("probe_preserve_pair"));
   return 0;
 }
 if(Is("probe_nested")||Is("probe_unarmed_nested")) nested=true;
 if(Is("probe_concurrent")) {
   auto* second=reinterpret_cast<panic_info_t*>(0x3fc89100U);*second=*info;
   std::thread one(Invoke,info),two(Invoke,second);one.join();two.join();
 } else Invoke(info);
 CHECK(Valid(g_capsule));
 CHECK(ProbeBound(g_stackProbe,g_capsule) && g_stackProbe.token==(unarmed?0U:0x10U));
 CHECK(forwards==((Is("probe_nested")||Is("probe_unarmed_nested")||Is("probe_concurrent"))?2U:1U));
 const bool badFrame=Is("probe_invalid_info")||Is("probe_invalid_frame")||Is("probe_unarmed_bounds");
 const bool badStack=badFrame||Is("probe_invalid_tcb")||Is("probe_reversed")||Is("probe_sp_outside")||
    Is("probe_misaligned_base")||Is("probe_misaligned_sp")||Is("probe_bad_end");
 CHECK(bool(g_stackProbe.flags&ProbeInvalidFrame)==badFrame);
 CHECK(bool(g_stackProbe.flags&ProbeInvalidStack)==badStack);
 if(!badFrame) {
   CHECK(g_stackProbe.ps==frame->ps && g_stackProbe.sar==frame->sar);
   for(unsigned i=0;i<14U;++i) CHECK(g_stackProbe.a[i]==registers[i]);
 }
 if(badStack) CHECK(g_stackProbe.stackWords==0U && g_stackProbe.stackAddress==0U);
 else if(Is("probe_byte_end")) {
   CHECK(g_stackProbe.stackAddress==sp-16U && g_stackProbe.stackWords==5U);
   CHECK(g_stackProbe.flags&ProbeStackClipped);
 } else if(Is("probe_base_clip")) {
   CHECK(g_stackProbe.stackAddress==sp-4U && g_stackProbe.stackWords==40U);
   CHECK(g_stackProbe.flags&ProbeStackClipped);
 } else if(Is("probe_upper_bound")) {
   CHECK(g_stackProbe.stackAddress==0x3fcffffcU && g_stackProbe.stackWords==1U);
   CHECK(g_stackProbe.stack[0]==0x12345678U && (g_stackProbe.flags&ProbeStackClipped));
 } else {
   CHECK(g_capsule.traceCount==2U && (g_capsule.flags&TruncatedTrace));
   CHECK(g_stackProbe.stackAddress==sp-16U && g_stackProbe.stackWords==40U);
   CHECK(g_stackProbe.flags==0U);
   for(unsigned i=0;i<40U;++i) CHECK(g_stackProbe.stack[i]==stack[i]);
   // Words beyond the call0 boundary are unavailable to the old unwinder.
   CHECK(g_stackProbe.stack[20]==0xa0000014U && g_stackProbe.stack[39]==0xa0000027U);
 }
 console.output.clear();CHECK(console.Run({"panicprobe","status"}));
 CHECK(console.output.find(unarmed?"state=captured_bound token=00000000":"state=captured_bound token=00000010")!=std::string::npos);
 CHECK(console.output.find(unarmed?"trial=unarmed_no_attribution age=unknown":"age=external_trial_required")!=std::string::npos);
 if(unarmed) {
   g_stackProbe.magic=PROBE_ARMED;
   CHECK(!ProbeValid(g_stackProbe) && !ProbeBound(g_stackProbe,g_capsule));
   console.output.clear();PrintRetained(console);
   CHECK(console.output.find("state=unavailable")!=std::string::npos);
   CHECK(console.output.find("stack_at=")==std::string::npos);
   g_stackProbe.magic=PROBE_CAPTURED;
   CHECK(ProbeBound(g_stackProbe,g_capsule));
 }
 CHECK(console.output.size()<6144U); // Existing bounded USB console queue.
 CHECK(console.output.find("task_identity=unknown")!=std::string::npos);
 if(Is("probe_binding")) {
   const uint32_t originalChecksum=g_capsule.checksum;
   g_capsule.pc^=1U;Seal();
   CHECK(!ProbeBound(g_stackProbe,g_capsule));
   console.output.clear();PrintRetained(console);
   CHECK(console.output.find("captured_unbound")!=std::string::npos);
   CHECK(console.output.find("panic_probe: a2=")==std::string::npos && console.output.find("stack_at=")==std::string::npos);
   g_stackProbe.panChecksum=g_capsule.checksum;PublishProbe(g_stackProbe,PROBE_CAPTURED);
   CHECK(!ProbeBound(g_stackProbe,g_capsule)); // Matching CRC alone is insufficient.
   g_stackProbe.panPc=g_capsule.pc;PublishProbe(g_stackProbe,PROBE_CAPTURED);
   CHECK(ProbeBound(g_stackProbe,g_capsule));
   g_stackProbe.stack[0]^=1U;CHECK(!ProbeValid(g_stackProbe));
   CHECK(originalChecksum!=g_capsule.checksum);
 }
 if(Is("probe_rearm")) {
   const uint32_t oldChecksum=g_capsule.checksum;
   char crc[9],pc[9];std::snprintf(crc,sizeof(crc),"%08x",oldChecksum);std::snprintf(pc,sizeof(pc),"%08x",g_capsule.pc);
   CHECK(!console.Run({"panicprobe","arm","11","0",pc}));
   CHECK(!console.Run({"panicprobe","arm","11",crc,"0"}));
   CHECK(Valid(g_capsule) && g_capsule.checksum==oldChecksum);
   console.output.clear();CHECK(console.Run({"panicprobe","arm","0x11",crc,pc}));
   CHECK(console.output.find("captured_bound")<console.output.find("armed=1 token=00000011"));
   CHECK(!Valid(g_capsule) && !ProbeBound(g_stackProbe,g_capsule));
   CHECK(ProbeValid(g_stackProbe) && g_stackProbe.magic==PROBE_ARMED);
   CHECK(g_stackProbe.token==0x11U && g_stackProbe.panChecksum==oldChecksum);
 }
 std::printf("PASS bounded private stack probe %s\n",mode);
 return 0;
}

"""


HOST_BUDGET_MAIN = r"""
#include <cstdio>
#include <cstring>
#include <cstdint>
#include "freertos/task.h"
#include "foxie_embedded/console.hpp"
#include "runtime/panic_diagnostic.hpp"
namespace runtime::panic_diagnostic { volatile Capsule g_capsule{}; volatile StackProbe g_stackProbe{}; }
unsigned lookups = 0U, reads = 0U;
bool present = true;
UBaseType_t freeBytes = 0U;
TaskHandle_t xTaskGetHandle(const char* name) {
 ++lookups;
 return present && std::strcmp(name,"nimble_host")==0
     ? reinterpret_cast<TaskHandle_t>(0x1234U) : nullptr;
}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task) {
 ++reads;
 if (task != reinterpret_cast<TaskHandle_t>(0x1234U)) return 0xffffffffU;
 return freeBytes;
}
#define CHECK(v) do { if (!(v)) { std::printf("FAIL line %u: %s\n",__LINE__,#v); return 1; } } while(0)
int main(int argc,char** argv) {
 CHECK(argc==2);
 present=std::strcmp(argv[1],"absent")!=0;
 freeBytes=std::strcmp(argv[1],"above4k")==0?3072U:
           std::strcmp(argv[1],"invalid")==0?8193U:
           std::strcmp(argv[1],"zero")==0?0U:6144U;
 CHECK(lookups==0U && reads==0U);
 fxe::Console console;
 runtime::panic_diagnostic::PrintRetained(console);
 CHECK(lookups==1U && reads==(present?1U:0U));
 CHECK(runtime::panic_diagnostic::g_capsule.magic==0U);
 CHECK(console.output.find("retained=unavailable")!=std::string::npos);
 CHECK(console.output.find("configured_bytes=8192")!=std::string::npos);
 if (!present) CHECK(console.output.find("found=0")!=std::string::npos);
 else if(freeBytes>8192U) CHECK(console.output.find("valid=0 minimum_free_bytes=8193 used_pattern_bytes=unknown")!=std::string::npos);
 else if(freeBytes==3072U) CHECK(console.output.find("valid=1 minimum_free_bytes=3072 used_pattern_bytes=5120")!=std::string::npos);
 else if(freeBytes==0U) CHECK(console.output.find("valid=1 minimum_free_bytes=0 used_pattern_bytes=8192")!=std::string::npos);
 else CHECK(console.output.find("valid=1 minimum_free_bytes=6144 used_pattern_bytes=2048")!=std::string::npos);
 std::printf("PASS on-demand diagnostic host budget %s\n",argv[1]);
}
"""

class Esp32S3PanicCaptureTest(unittest.TestCase):
    def test_original_frame_invalid_inputs_first_fault_and_poweron_retention(self):
        with tempfile.TemporaryDirectory(prefix="litch-panic-capture-") as tmp:
            path = Path(tmp)
            (path / "sdk.hpp").write_text(SDK)
            for name in ("esp_attr.h", "esp_private/panic_internal.h", "xtensa/xtensa_context.h"):
                header = path / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text('#include "sdk.hpp"\n')
            console = path / "foxie_embedded/console.hpp"
            console.parent.mkdir(parents=True, exist_ok=True)
            console.write_text(CONSOLE)
            (path / "main.cpp").write_text(MAIN)
            binary = path / "capture"
            compiled = subprocess.run([
                "c++", "-std=c++17", "-DARDUINO", "-DLITCH_ESP32S3", "-DLITCH_PANIC_DIAGNOSTIC",
                "-DLITCH_BOOT_ENTRY_DIAGNOSTIC",
                "-I", str(path), "-I", str(ROOT / "include"),
                str(ROOT / "src/esp32s3_panic_capture.cpp"),
                str(ROOT / "src/progress_watchdog.cpp"), str(ROOT / "src/panic_diagnostic.cpp"),
                str(path / "main.cpp"), "-pthread", "-o", str(binary),
                *(["-arch", "x86_64", "-Wl,-pagezero_size,0x1000"] if sys.platform == "darwin" else [])
            ], capture_output=True, text=True, timeout=30)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            for mode in ("normal", "debug", "abort", "abort_invalid", "pseudo", "null", "bounds",
                         "misaligned", "bad_stack", "invalid_core", "tcb_null", "tcb_misaligned",
                         "tcb_tail", "tcb_upper", "name16", "details", "nested", "concurrent",
                         "limit", "legacy24", "legacy_other", "extended_partial", "future", "interrupted", "corrupt", "invalid",
                         "probe_success", "probe_registration", "probe_reject", "probe_unarmed", "probe_partial",
                         "probe_zero_arm", "probe_unarmed_nested", "probe_unarmed_bounds", "probe_preserve_pair",
                         "probe_invalid_info", "probe_invalid_frame", "probe_invalid_tcb",
                         "probe_reversed", "probe_sp_outside", "probe_misaligned_base",
                         "probe_misaligned_sp", "probe_bad_end", "probe_byte_end", "probe_base_clip",
                         "probe_upper_bound", "probe_magic", "probe_preserve", "probe_nested", "probe_concurrent",
                         "probe_binding", "probe_rearm"):
                with self.subTest(mode=mode):
                    result = subprocess.run([str(binary), mode], capture_output=True, text=True, timeout=5)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_on_demand_host_budget_handles_missing_task_and_invalid_watermark(self):
        with tempfile.TemporaryDirectory(prefix="litch-host-budget-") as tmp:
            path = Path(tmp)
            for name, body in {
                "foxie_embedded/console.hpp": CONSOLE,
                "freertos/FreeRTOS.h": "#pragma once\n#include <cstdint>\nusing StackType_t=uint8_t; using UBaseType_t=unsigned; using TaskHandle_t=void*;\n",
                "freertos/task.h": '#pragma once\n#include "freertos/FreeRTOS.h"\nTaskHandle_t xTaskGetHandle(const char*);\nUBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t);\n',
            }.items():
                header = path / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text(body)
            (path / "main.cpp").write_text(HOST_BUDGET_MAIN)
            binary = path / "host_budget"
            compiled = subprocess.run([
                "c++", "-std=c++17", "-DARDUINO", "-DLITCH_ESP32S3",
                "-DLITCH_PANIC_DIAGNOSTIC", "-DLITCH_NIMBLE_HOST_BUDGET_DIAGNOSTIC",
                "-DCONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=8192",
                "-I", str(path), "-I", str(ROOT / "include"),
                str(ROOT / "src/panic_diagnostic.cpp"), str(path / "main.cpp"),
                "-o", str(binary),
            ], capture_output=True, text=True, timeout=30)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            for mode in ("absent", "below4k", "above4k", "zero", "invalid"):
                with self.subTest(mode=mode):
                    result = subprocess.run([str(binary), mode], capture_output=True, text=True, timeout=5)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


    def test_release_compiles_out_all_capture_and_command_symbols(self):
        with tempfile.TemporaryDirectory(prefix="litch-panic-release-") as tmp:
            for name in ("esp32s3_panic_capture", "panic_diagnostic"):
                obj = Path(tmp) / (name + ".o")
                compiled = subprocess.run([
                    "c++", "-std=c++17", "-DARDUINO", "-DLITCH_ESP32S3",
                    "-I", str(ROOT / "include"), "-c", str(ROOT / "src" / (name + ".cpp")),
                    "-o", str(obj),
                ], capture_output=True, text=True, timeout=30)
                self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
                # Host-tool startup has the compiler's budget; the five-second
                # bound above applies to execution of the panic fixture itself.
                symbols = subprocess.run(["nm", "-g", str(obj)], capture_output=True, text=True, timeout=30)
                self.assertEqual(symbols.returncode, 0, symbols.stderr)
                for symbol in ("g_capsule", "g_stackProbe", "StackProbeCommand", "RegisterStackProbeCommand", "__wrap_esp_panic_handler"):
                    self.assertNotIn(symbol, symbols.stdout)
