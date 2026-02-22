/*
 * libbase_stubs.cc -- Stub implementations for SemperOS libbase functions
 *
 * These are functions from SemperOS's base library that the kernel
 * references. On sel4 we provide minimal implementations backed by
 * our existing C++ runtime (bump allocator from cxx_runtime.cc) or
 * simple stubs.
 */

extern "C" {
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
}

/* Placement new for this file */
#ifndef _PLACEMENT_NEW_DEFINED
#define _PLACEMENT_NEW_DEFINED
inline void *operator new(unsigned long, void *p) noexcept { return p; }
#endif

#include <base/Common.h>
#include <base/Config.h>
#include <base/Errors.h>
#include <base/Heap.h>
#include <base/Backtrace.h>
#include <base/stream/OStream.h>
#include <base/stream/Serial.h>
#include <base/util/Random.h>
#include <base/util/String.h>
#include <base/WorkLoop.h>
#include <thread/ThreadManager.h>

/* ================================================================
 * m3::Heap -- redirect to standard malloc/free (backed by musl libc)
 *
 * Header declares:
 *   static bool _ready;
 *   static Area *_begin;     (Area is Heap::Area, a private struct)
 *   static Area *_end;
 * ================================================================ */
namespace m3 {

bool Heap::_ready = false;
Heap::Area *Heap::_begin = nullptr;
Heap::Area *Heap::_end = nullptr;

void *Heap::try_alloc(size_t size) {
    return malloc(size);
}

void *Heap::alloc(size_t size) {
    return malloc(size);
}

void *Heap::calloc(size_t n, size_t size) {
    return ::calloc(n, size);
}

void *Heap::realloc(void *p, size_t size) {
    return ::realloc(p, size);
}

void Heap::free(void *p) {
    ::free(p);
}

size_t Heap::contiguous_mem() {
    return 0;
}

size_t Heap::free_memory() {
    return 0;
}

uintptr_t Heap::end() {
    return reinterpret_cast<uintptr_t>(_end);
}

void Heap::print(OStream &) {
    /* no-op on sel4 */
}

/* ================================================================
 * m3::Errors
 * ================================================================ */
Errors::Code Errors::last;

const char *Errors::to_string(Code code) {
    switch(code) {
        case NO_ERROR:      return "No error";
        case INV_ARGS:      return "Invalid arguments";
        case OUT_OF_MEM:    return "Out of memory";
        default:            return "Unknown error";
    }
}

/* ================================================================
 * m3::Serial -- redirect to printf
 *
 * Header declares:
 *   static Serial *_inst;      (pointer)
 *   static const char *_colors[];
 *   void flush();
 *   virtual char read() override;
 *   virtual bool putback(char c) override;
 *   virtual void write(char c) override;
 *   static void init(const char *path, int core);
 *
 * Serial has a private constructor and uses virtual diamond
 * inheritance (IStream + OStream -> IOSBase).
 * We allocate a static buffer and use placement new.
 * ================================================================ */
static char _serial_storage[sizeof(Serial)] __attribute__((aligned(16)));
static bool _serial_inited = false;
const char *Serial::_colors[] = { nullptr };

/* Initialize Serial eagerly — KLOG is called during static init */
static void _ensure_serial() {
    if(!_serial_inited) {
        Serial::init("", 0);
    }
}

Serial *Serial::_inst = nullptr;

void Serial::init(const char *, int) {
    if(!_serial_inited) {
        _inst = new (_serial_storage) Serial();
        _serial_inited = true;
    }
}

void Serial::flush() {
    fflush(stdout);
}

char Serial::read() {
    return '\0';
}

bool Serial::putback(char) {
    return false;
}

void Serial::write(char c) {
    putchar(c);
}

/* ================================================================
 * m3::OStream -- output formatting
 *
 * Header declares (private, non-const):
 *   static char _hexchars_big[];
 *   static char _hexchars_small[];
 *
 * All print methods return int.
 * FormatParams flag is CAPHEX (not CAPITALIZE).
 * ================================================================ */
char OStream::_hexchars_small[] = "0123456789abcdef";
char OStream::_hexchars_big[]   = "0123456789ABCDEF";

int OStream::printn(long n) {
    if(n < 0) {
        write('-');
        return 1 + printu(static_cast<ulong>(-n), 10, _hexchars_small);
    }
    return printu(static_cast<ulong>(n), 10, _hexchars_small);
}

int OStream::printlln(llong n) {
    if(n < 0) {
        write('-');
        return 1 + printllu(static_cast<ullong>(-n), 10, _hexchars_small);
    }
    return printllu(static_cast<ullong>(n), 10, _hexchars_small);
}

int OStream::printu(ulong n, uint base, char *chars) {
    int count = 0;
    if(n >= base)
        count += printu(n / base, base, chars);
    write(chars[n % base]);
    return count + 1;
}

int OStream::printllu(ullong n, uint base, char *chars) {
    int count = 0;
    if(n >= base)
        count += printllu(n / base, base, chars);
    write(chars[n % base]);
    return count + 1;
}

int OStream::printptr(uintptr_t u, uint flags) {
    write('0');
    write('x');
    return 2 + printu(u, 16, (flags & FormatParams::CAPHEX) ? _hexchars_big : _hexchars_small);
}

int OStream::printfloat(float d, uint precision) {
    int count = 0;
    if(d < 0) {
        write('-');
        count++;
        d = -d;
    }
    ulong val = static_cast<ulong>(d);
    count += printu(val, 10, _hexchars_small);
    write('.');
    count++;
    d -= static_cast<float>(val);
    for(uint i = 0; i < precision; i++) {
        d *= 10;
        val = static_cast<ulong>(d);
        write(_hexchars_small[val % 10]);
        count++;
        d -= static_cast<float>(val);
    }
    return count;
}

int OStream::puts(const char *str, ulong prec) {
    int count = 0;
    if(str) {
        while(*str && prec-- > 0) {
            write(*str++);
            count++;
        }
    }
    return count;
}

int OStream::printsignedprefix(long n, uint flags) {
    if(n >= 0) {
        if(flags & FormatParams::FORCESIGN) {
            write('+');
            return 1;
        }
        if(flags & FormatParams::SPACESIGN) {
            write(' ');
            return 1;
        }
    }
    else {
        write('-');
        return 1;
    }
    return 0;
}

int OStream::putspad(const char *s, uint pad, uint prec, uint flags) {
    int count = 0;
    if(~prec == 0)
        prec = s ? strlen(s) : 0;
    size_t slen = s ? strlen(s) : 0;
    if(slen > prec)
        slen = prec;
    if(pad > slen && !(flags & FormatParams::PADRIGHT))
        count += printpad(pad - slen, flags);
    count += puts(s, prec);
    if(pad > slen && (flags & FormatParams::PADRIGHT))
        count += printpad(pad - slen, flags);
    return count;
}

int OStream::printnpad(long n, uint pad, uint flags) {
    int count = 0;
    /* Calculate width needed */
    long tmp = n < 0 ? -n : n;
    uint width = 0;
    do {
        width++;
        tmp /= 10;
    } while(tmp > 0);
    if(n < 0 || (flags & (FormatParams::FORCESIGN | FormatParams::SPACESIGN)))
        width++;

    if(pad > width && !(flags & FormatParams::PADRIGHT)) {
        if(flags & FormatParams::PADZEROS) {
            count += printsignedprefix(n, flags);
            count += printpad(pad - width, flags);
        }
        else {
            for(uint i = width; i < pad; i++) {
                write(' ');
                count++;
            }
            count += printsignedprefix(n, flags);
        }
    }
    else {
        count += printsignedprefix(n, flags);
    }

    ulong val = n < 0 ? static_cast<ulong>(-n) : static_cast<ulong>(n);
    count += printu(val, 10, _hexchars_small);

    if(pad > width && (flags & FormatParams::PADRIGHT)) {
        for(uint i = width; i < pad; i++) {
            write(' ');
            count++;
        }
    }
    return count;
}

int OStream::printupad(ulong u, uint base, uint pad, uint flags) {
    int count = 0;
    /* Calculate width needed */
    ulong tmp = u;
    uint width = 0;
    do {
        width++;
        tmp /= base;
    } while(tmp > 0);
    if(flags & FormatParams::PRINTBASE) {
        if(base == 16)
            width += 2;
        else if(base == 8)
            width += 1;
    }

    if(pad > width && !(flags & FormatParams::PADRIGHT)) {
        count += printpad(pad - width, flags);
    }

    if(flags & FormatParams::PRINTBASE) {
        if(base == 16) {
            write('0');
            write((flags & FormatParams::CAPHEX) ? 'X' : 'x');
            count += 2;
        }
        else if(base == 8) {
            write('0');
            count += 1;
        }
    }

    count += printu(u, base, (flags & FormatParams::CAPHEX) ? _hexchars_big : _hexchars_small);

    if(pad > width && (flags & FormatParams::PADRIGHT)) {
        for(uint i = width; i < pad; i++) {
            write(' ');
            count++;
        }
    }
    return count;
}

int OStream::printpad(int count, uint flags) {
    char c = (flags & FormatParams::PADZEROS) ? '0' : ' ';
    for(int i = 0; i < count; i++)
        write(c);
    return count;
}

void OStream::dump(const void *data, size_t size) {
    const uint8_t *bytes = reinterpret_cast<const uint8_t*>(data);
    for(size_t i = 0; i < size; i++) {
        if((i % 16) == 0) {
            if(i > 0)
                write('\n');
            printptr(i, 0);
            write(':');
            write(' ');
        }
        write(_hexchars_small[bytes[i] >> 4]);
        write(_hexchars_small[bytes[i] & 0xf]);
        write(' ');
    }
    if(size > 0)
        write('\n');
}

OStream::FormatParams::FormatParams(const char *fmt)
    : _base(10), _flags(0), _pad(0), _prec(static_cast<uint>(-1)) {
    if(!fmt)
        return;
    /* Parse format string (simplified) */
    while(*fmt) {
        if(*fmt == 'x' || *fmt == 'X') {
            _base = 16;
            if(*fmt == 'X')
                _flags |= CAPHEX;
        }
        else if(*fmt == 'o')
            _base = 8;
        else if(*fmt == 'b')
            _base = 2;
        else if(*fmt == 'p') {
            _base = 16;
            _flags |= POINTER;
        }
        else if(*fmt == '-')
            _flags |= PADRIGHT;
        else if(*fmt == '+')
            _flags |= FORCESIGN;
        else if(*fmt == ' ')
            _flags |= SPACESIGN;
        else if(*fmt == '#')
            _flags |= PRINTBASE;
        else if(*fmt >= '0' && *fmt <= '9') {
            if(_pad == 0 && *fmt == '0')
                _flags |= PADZEROS;
            _pad = _pad * 10 + (*fmt - '0');
        }
        fmt++;
    }
}

OStream &operator<<(OStream &os, const String &s) {
    if(s.c_str())
        os << s.c_str();
    return os;
}

/* ================================================================
 * m3::Random
 *
 * Header declares:
 *   static uint _randa;
 *   static uint _randc;
 *   static uint _last;
 * ================================================================ */
uint Random::_randa = 1103515245;
uint Random::_randc = 12345;
uint Random::_last = 0;

/* ================================================================
 * m3::Backtrace
 *
 * Header declares:
 *   static const size_t CALL_INSTR_SIZE;
 *   static size_t collect(uintptr_t *addr, size_t max);
 *   static void print(OStream &os);
 * ================================================================ */
const size_t Backtrace::CALL_INSTR_SIZE = 5;

size_t Backtrace::collect(uintptr_t *, size_t) {
    return 0; /* no-op on sel4 */
}

void Backtrace::print(OStream &) {
    /* no-op on sel4 */
}

/* ================================================================
 * m3::WorkLoop
 *
 * Header declares:
 *   void add(WorkItem *item, bool permanent);
 *   void remove(WorkItem *item);
 *   virtual void run();
 * ================================================================ */
void WorkLoop::add(WorkItem *, bool) {
    /* stub */
}

void WorkLoop::remove(WorkItem *) {
    /* stub */
}

void WorkLoop::run() {
    /* stub */
}

/* ================================================================
 * m3::ThreadManager -- single-threaded stub
 *
 * The header (ThreadManager.h) defines the constructor,
 * current(), wait_for(), notify(), yield(), and stop() ALL
 * inline. We must NOT redefine them here.
 *
 * We only need to provide:
 *   - The static member `inst`
 * ================================================================ */
ThreadManager ThreadManager::inst;

/* ================================================================
 * m3::Thread -- static members and non-inline definitions
 *
 * Header declares:
 *   static int _next_id;
 *
 *   public:  explicit Thread(thread_func func, void *arg);
 *   public:  ~Thread();
 *   private: explicit Thread();   <-- defined inline in header
 *
 * The private Thread() default constructor is defined inline in
 * the header and used by ThreadManager (friend). We do NOT
 * redefine it.
 *
 * We need to provide the public constructor and destructor.
 * ================================================================ */
int Thread::_next_id = 1;

Thread::Thread(thread_func func, void *arg)
    : _id(_next_id++), _regs(), _stack(nullptr), _event(nullptr), _content(false) {
    /* Allocate stack and initialize registers for cooperative switch */
    _stack = new word_t[T_STACK_WORDS];
    thread_init(func, arg, &_regs, _stack);
    memset(_msg, 0, MAX_MSG_SIZE);
}

Thread::~Thread() {
    delete[] _stack;
}

} /* namespace m3 */

/* ================================================================
 * thread_save / thread_resume -- assembly stubs
 *
 * Declared in thread/arch/sel4/Thread.h as:
 *   extern "C" bool thread_save(Regs *regs);
 *   extern "C" bool thread_resume(Regs *regs);
 *
 * Also need thread_init (declared in same header):
 *   void thread_init(_thread_func func, void *arg, Regs *regs, word_t *stack);
 * ================================================================ */
extern "C" {

bool thread_save(m3::Regs *) {
    return true; /* always "returns first time" */
}

bool thread_resume(m3::Regs *) {
    return false;
}

} /* extern "C" */

namespace m3 {

void thread_init(_thread_func, void *, Regs *regs, word_t *) {
    /* Zero out regs -- on sel4 we are single-threaded so
     * cooperative switching is stubbed out */
    memset(regs, 0, sizeof(Regs));
}

} /* namespace m3 */

/* ================================================================
 * m3::Machine
 *
 * Header declares:
 *   static NORETURN void shutdown();
 *   static int write(const char *str, size_t len);   <-- returns int
 *   static ssize_t read(char *buf, size_t len);
 * ================================================================ */
namespace m3 {

NORETURN void Machine::shutdown() {
    printf("[SemperKernel] Machine::shutdown()\n");
    while(1) {}
}

int Machine::write(const char *buf, size_t len) {
    for(size_t i = 0; i < len; i++)
        putchar(buf[i]);
    return 0;
}

ssize_t Machine::read(char *, size_t) {
    return 0;
}

NORETURN void Env::exit(int code) {
    printf("[SemperKernel] Env::exit(%d)\n", code);
    while(1) {}
}

/* ================================================================
 * m3::env() — static Env instance for sel4
 * ================================================================ */
class Sel4EnvBackend : public BaremetalEnvBackend {
    friend Env *env();
public:
    explicit Sel4EnvBackend() { _workloop = nullptr; }
    void init() override {}
    void reinit() override {}
    void exit(int) override { while(1) {} }
};

static WorkLoop _sel4_workloop;
static Sel4EnvBackend _sel4_backend;
static Env _sel4_env;

static bool _env_initialized = false;

Env *env() {
    if(!_env_initialized) {
        memset(&_sel4_env, 0, sizeof(_sel4_env));
        _sel4_backend._workloop = &_sel4_workloop;
        _sel4_env.backend = &_sel4_backend;
        _sel4_env.coreid = 0;
        _sel4_env.pe = PEDesc(PEType::COMP_IMEM);
        _env_initialized = true;
    }
    return &_sel4_env;
}

} /* namespace m3 */

/* ================================================================
 * kernel:: stubs — arch-specific kernel functions
 * ================================================================ */
#include "pes/KPE.h"
#include "ddl/MHTInstance.h"

namespace kernel {

/* KPE::start — in baremetal, this wakes a PE to run another kernel */
void KPE::start(int, char**, size_t, m3::PEDesc[]) {
    KLOG(KPES, "KPE::start [sel4 stub]");
}

/* MHTInstance 3-arg constructor — loads partition data from memory.
 * On sel4 we don't have pre-loaded partitions, so just call default init. */
MHTInstance::MHTInstance(uint64_t, uint64_t, size_t)
    : MHTInstance()
{
}

} /* namespace kernel */
