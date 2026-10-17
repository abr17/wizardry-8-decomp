/* Differential test harness core (mod, 2026-10-13). See w8diff_core.h and
   wiz8-widescreen docs/w8diff.md. Built with MSVC x86 in both products: it relies on __try
   and on __asm for the generic call.

   Plan file (text, one command per line, numbers in hex without 0x):
     W8DIFF 1
     cw <hex>                        FPU control word set around every call
     ready <pexpr> <hex> <frames>    start once *(u32*)pexpr == hex for N frames
     region <name> <pexpr> <len>     named range, echoed to the output header
     case <id>
       fn <c|s|t|f> <pexpr>          cdecl, stdcall, thiscall (arg 0 = this),
                                     fastcall (args 0,1 = ecx,edx); no fn = no call
       arg <hex> | arg &<pexpr>      next 32-bit argument
       set <pexpr> <hexbytes>        write before the call, restored after
       setb <pexpr> <id>             same, bytes of header line 'blob <id> <hex>'
       setp <pexpr> <pexpr2>         write the dword value of pexpr2 (wire a scratch
                                     struct into a global or a pointer field)
       fill <pexpr> <len> <byte>     memset before the call, restored after
       keep <pexpr> <len>            restored after the call
       cap <pexpr> <len>             restored after; changed bytes are written out
       dump <pexpr> <len>            bytes after the call are written out
       out <p|w|c>                   eax is a pointer / wide string / char string
       seed <hex>                    srand() right before the call
     end
   A pointer expression is a base - hex address or S<n> (scratch buffer n,
   zeroed for every case) - followed by +hex, -hex and * (read a dword there).

   Header 'resume <n>': skip n cases and append (restart after a hang).
   Output: W8DIFF-OUT header, G lines for regions, then per case 'S <id>'
   (written and flushed before the call) and
     R <id> <status> a=<assert lines> eax= edx= fp=<st0 bits or -> fd=<fpu delta>
     C <id> <cap#> <offset>:<bytes> ...      D <id> <dump#> <bytes>
   status: ok, E<code>@<eip> (exception in the call), X<op#> (bad pointer in
   the set-up, nothing was called). */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "w8diff_core.h"

#define W8D_MAX_ARGS 24
#define W8D_MAX_OPS 256
#define W8D_SCRATCH_COUNT 8
#define W8D_SCRATCH_SIZE 0x10000
#define W8D_MAX_ASSERTS 8

enum { CONV_C, CONV_S, CONV_T, CONV_F };
enum { OP_SET, OP_FILL, OP_KEEP, OP_CAP, OP_DUMP, OP_SETP };

typedef struct {
    int kind;
    const char* expr;
    unsigned len;
    const char* hex; /* OP_SET data */
    const unsigned char* blob; /* OP_SET from a header blob (setb) */
    const char* value_expr;    /* OP_SETP: dword = this pointer expression */
    unsigned char fill;
    unsigned char* ptr;
    unsigned char* saved;
} W8DOp;

typedef struct {
    const char* id;
    int has_fn;
    int conv;
    const char* fn;
    int nargs;
    const char* args[W8D_MAX_ARGS];
    int nops;
    W8DOp ops[W8D_MAX_OPS];
    int out_kind;
    int has_seed;
    unsigned seed;
} W8DCase;

static unsigned char g_scratch[W8D_SCRATCH_COUNT][W8D_SCRATCH_SIZE];

/* Header 'blob <id> <hex>': byte strings cases copy with 'setb <pexpr> <id>'. */
#define W8D_MAX_BLOBS 256
static unsigned char* g_blob[W8D_MAX_BLOBS];
static unsigned g_blob_len[W8D_MAX_BLOBS];

static int g_state; /* 0 idle, 1 waiting for ready, 2 running */
static DWORD g_last_check;
static char* g_plan;
static char* g_cursor;
static char* g_pending;
static FILE* g_out;
static unsigned g_case_count;
static unsigned g_skip; /* header 'resume <n>': cases already done by a killed run */
static int g_has_cw;
static unsigned short g_cw;
static const char* g_ready_expr;
static unsigned g_ready_value;
static unsigned g_ready_frames;
static unsigned g_ready_count;
static DWORD g_ready_started;

static int g_assert_count;
static long g_assert_lines[W8D_MAX_ASSERTS];
static unsigned char* g_assert_fn;
static unsigned char g_assert_orig[5];

static void Path(const W8DiffHost* host, const char* kind, char* out)
{
    _snprintf(out, MAX_PATH, "%sw8diff_%s%s", host->dir, kind, host->name);
    out[MAX_PATH - 1] = 0;
}

static unsigned Hex(const char** s)
{
    unsigned v = 0;
    for (;;) {
        char c = **s;
        if (c >= '0' && c <= '9') {
            v = v * 16 + (unsigned)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            v = v * 16 + (unsigned)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            v = v * 16 + (unsigned)(c - 'A' + 10);
        } else {
            return v;
        }
        ++*s;
    }
}

/* Evaluate a pointer expression. Returns 0 on a bad dereference. */
static int Eval(const char* s, unsigned* out)
{
    unsigned v;
    if (*s == 'S') {
        unsigned n;
        ++s;
        n = Hex(&s);
        if (n >= W8D_SCRATCH_COUNT) {
            return 0;
        }
        v = (unsigned)(UINT_PTR)g_scratch[n];
    } else {
        v = Hex(&s);
    }
    while (*s && *s != ' ') {
        char op = *s++;
        if (op == '+') {
            v += Hex(&s);
        } else if (op == '-') {
            v -= Hex(&s);
        } else if (op == '*') {
            __try {
                v = *(const unsigned*)(UINT_PTR)v;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        } else {
            return 0;
        }
    }
    *out = v;
    return 1;
}

static void __cdecl AssertHook(const char* expression, const char* file, long line,
                               const char* message, ...)
{
    (void)expression;
    (void)file;
    (void)message;
    if (g_assert_count < W8D_MAX_ASSERTS) {
        g_assert_lines[g_assert_count] = line;
    }
    ++g_assert_count;
}

static void PatchAsserts(int on)
{
    DWORD old;
    if (g_assert_fn == 0) {
        HMODULE sr = GetModuleHandleA("sr.dll");
        if (sr == 0) {
            return;
        }
        g_assert_fn = (unsigned char*)GetProcAddress(sr, "?srAssertFail@@YAXPBD0J0ZZ");
        if (g_assert_fn == 0) {
            return;
        }
        memcpy(g_assert_orig, g_assert_fn, 5);
    }
    if (!VirtualProtect(g_assert_fn, 5, PAGE_EXECUTE_READWRITE, &old)) {
        return;
    }
    if (on) {
        int rel = (int)((UINT_PTR)AssertHook - ((UINT_PTR)g_assert_fn + 5));
        g_assert_fn[0] = 0xE9;
        memcpy(g_assert_fn + 1, &rel, 4);
    } else {
        memcpy(g_assert_fn, g_assert_orig, 5);
    }
    VirtualProtect(g_assert_fn, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_assert_fn, 5);
}

/* Push the stack arguments, load ecx/edx, call, and put esp back from a saved
   copy - which suits every convention, whoever pops the arguments. A value the
   call left on the x87 stack (a float return) is popped into *fp. */
static void RawCall(unsigned fn, unsigned reg_ecx, unsigned reg_edx, const unsigned* stack_args,
                    int nstack, unsigned* r_eax, unsigned* r_edx, unsigned* sw_before,
                    unsigned* sw_after)
{
    unsigned saved_esp = 0;
    unsigned a = 0;
    unsigned d = 0;
    unsigned sw0 = 0;
    unsigned sw1 = 0;
    __asm {
        fnstsw ax
        and eax, 0xffff
        mov sw0, eax
        mov saved_esp, esp
        mov ecx, nstack
        mov esi, stack_args
    push_loop:
        test ecx, ecx
        jz do_call
        dec ecx
        push dword ptr [esi + ecx * 4]
        jmp push_loop
    do_call:
        mov ecx, reg_ecx
        mov edx, reg_edx
        mov eax, fn
        call eax
        mov esp, saved_esp
        mov a, eax
        mov d, edx
        fnstsw ax
        and eax, 0xffff
        mov sw1, eax
    }
    *r_eax = a;
    *r_edx = d;
    *sw_before = sw0;
    *sw_after = sw1;
}

static void PopFp(unsigned char* bits)
{
    double v = 0.0;
    __asm {
        fstp v
    }
    memcpy(bits, &v, 8);
}

static unsigned short GetCw(void)
{
    unsigned short cw = 0;
    __asm {
        fnstcw cw
    }
    return cw;
}

static void SetCw(unsigned short cw)
{
    __asm {
        fnclex
        fldcw cw
    }
}

static void ResetFpu(unsigned short cw)
{
    __asm {
        fninit
        fldcw cw
    }
}

static int GuardedCall(unsigned fn, unsigned ecx, unsigned edx, const unsigned* args, int n,
                       unsigned* eax_out, unsigned* edx_out, unsigned* sw0, unsigned* sw1,
                       unsigned* code, unsigned* eip)
{
    __try {
        RawCall(fn, ecx, edx, args, n, eax_out, edx_out, sw0, sw1);
        return 1;
    } __except (*code = GetExceptionCode(),
                *eip = (unsigned)(GetExceptionInformation())->ContextRecord->Eip,
                EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static int CopyGuarded(void* dst, const void* src, unsigned len)
{
    __try {
        memcpy(dst, src, len);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static void WriteHex(const unsigned char* p, unsigned len)
{
    static const char digits[] = "0123456789abcdef";
    char buf[512];
    unsigned i;
    unsigned n = 0;
    for (i = 0; i < len; ++i) {
        buf[n++] = digits[p[i] >> 4];
        buf[n++] = digits[p[i] & 15];
        if (n >= sizeof(buf) - 2) {
            fwrite(buf, 1, n, g_out);
            n = 0;
        }
    }
    fwrite(buf, 1, n, g_out);
}

static unsigned Nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return (unsigned)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (unsigned)(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return (unsigned)(c - 'A' + 10);
    }
    return 0;
}

/* Next line of the plan, nul-terminated in place; 0 at the end. */
static char* NextLine(void)
{
    char* line;
    char* end;
    while (g_cursor && *g_cursor) {
        line = g_cursor;
        end = strchr(line, '\n');
        if (end) {
            *end = 0;
            g_cursor = end + 1;
        } else {
            g_cursor = line + strlen(line);
        }
        end = line + strlen(line);
        while (end > line && (end[-1] == '\r' || end[-1] == ' ')) {
            *--end = 0;
        }
        if (*line && *line != '#') {
            return line;
        }
    }
    return 0;
}

static const char* Word(char** s)
{
    char* w = *s;
    char* e;
    while (*w == ' ') {
        ++w;
    }
    e = w;
    while (*e && *e != ' ') {
        ++e;
    }
    if (*e) {
        *e++ = 0;
    }
    *s = e;
    return w;
}

static int ParseCase(char* first, W8DCase* c)
{
    char* line;
    memset(c, 0, sizeof(*c));
    c->id = Word(&first);
    while ((line = NextLine()) != 0) {
        const char* cmd = Word(&line);
        if (strcmp(cmd, "end") == 0) {
            return 1;
        }
        if (strcmp(cmd, "fn") == 0) {
            const char* conv = Word(&line);
            c->has_fn = 1;
            c->conv = conv[0] == 's' ? CONV_S : conv[0] == 't' ? CONV_T : conv[0] == 'f' ? CONV_F
                                                                                     : CONV_C;
            c->fn = Word(&line);
        } else if (strcmp(cmd, "arg") == 0) {
            if (c->nargs < W8D_MAX_ARGS) {
                c->args[c->nargs++] = Word(&line);
            }
        } else if (strcmp(cmd, "seed") == 0) {
            const char* v = Word(&line);
            c->has_seed = 1;
            c->seed = Hex(&v);
        } else if (strcmp(cmd, "out") == 0) {
            c->out_kind = Word(&line)[0];
        } else if (c->nops < W8D_MAX_OPS) {
            W8DOp* op = &c->ops[c->nops];
            const char* v;
            memset(op, 0, sizeof(*op));
            if (strcmp(cmd, "set") == 0) {
                op->kind = OP_SET;
                op->expr = Word(&line);
                op->hex = Word(&line);
                op->len = (unsigned)strlen(op->hex) / 2;
            } else if (strcmp(cmd, "setp") == 0) {
                op->kind = OP_SETP;
                op->expr = Word(&line);
                op->value_expr = Word(&line);
                op->len = 4;
            } else if (strcmp(cmd, "setb") == 0) {
                unsigned id;
                op->kind = OP_SET;
                op->expr = Word(&line);
                v = Word(&line);
                id = Hex(&v);
                if (id >= W8D_MAX_BLOBS || g_blob[id] == 0) {
                    continue;
                }
                op->blob = g_blob[id];
                op->len = g_blob_len[id];
            } else {
                op->kind = strcmp(cmd, "fill") == 0   ? OP_FILL
                           : strcmp(cmd, "keep") == 0 ? OP_KEEP
                           : strcmp(cmd, "cap") == 0  ? OP_CAP
                           : strcmp(cmd, "dump") == 0 ? OP_DUMP
                                                      : -1;
                if (op->kind < 0) {
                    continue;
                }
                op->expr = Word(&line);
                v = Word(&line);
                op->len = Hex(&v);
                if (op->kind == OP_FILL) {
                    v = Word(&line);
                    op->fill = (unsigned char)Hex(&v);
                }
            }
            ++c->nops;
        }
    }
    return 0;
}

static void WriteCaps(const W8DCase* c)
{
    int i;
    int cap_index = 0;
    int dump_index = 0;
    for (i = 0; i < c->nops; ++i) {
        const W8DOp* op = &c->ops[i];
        if (op->kind == OP_CAP && op->ptr) {
            unsigned char* now = (unsigned char*)malloc(op->len ? op->len : 1);
            unsigned k = 0;
            fprintf(g_out, "C %s %d", c->id, cap_index);
            if (now && CopyGuarded(now, op->ptr, op->len)) {
                while (k < op->len) {
                    unsigned start;
                    if (now[k] == op->saved[k]) {
                        ++k;
                        continue;
                    }
                    start = k;
                    while (k < op->len && now[k] != op->saved[k]) {
                        ++k;
                    }
                    /* new bytes, then the bytes before the call: compare.py
                       rebuilds a float that changed in one process only */
                    fprintf(g_out, " %x:", start);
                    WriteHex(now + start, k - start);
                    fprintf(g_out, "/");
                    WriteHex(op->saved + start, k - start);
                }
            } else {
                fprintf(g_out, " unreadable");
            }
            fprintf(g_out, "\n");
            free(now);
            ++cap_index;
        } else if (op->kind == OP_DUMP && op->ptr) {
            unsigned char* now = (unsigned char*)malloc(op->len ? op->len : 1);
            fprintf(g_out, "D %s %d ", c->id, dump_index);
            if (now && CopyGuarded(now, op->ptr, op->len)) {
                WriteHex(now, op->len);
            } else {
                fprintf(g_out, "unreadable");
            }
            fprintf(g_out, "\n");
            free(now);
            ++dump_index;
        }
    }
}

static void WriteString(unsigned p, int wide)
{
    unsigned char buf[512];
    unsigned n = 0;
    if (p == 0) {
        fprintf(g_out, " s=null");
        return;
    }
    __try {
        if (wide) {
            const unsigned short* w = (const unsigned short*)(UINT_PTR)p;
            while (n + 2 <= sizeof(buf) && w[n / 2] != 0) {
                memcpy(buf + n, &w[n / 2], 2);
                n += 2;
            }
        } else {
            const unsigned char* s = (const unsigned char*)(UINT_PTR)p;
            while (n < sizeof(buf) && s[n] != 0) {
                buf[n] = s[n];
                ++n;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fprintf(g_out, " s=bad");
        return;
    }
    fprintf(g_out, " s=");
    WriteHex(buf, n);
}

static void WritePointer(unsigned p)
{
    unsigned char buf[32];
    int i;
    for (i = 0; i < W8D_SCRATCH_COUNT; ++i) {
        unsigned base = (unsigned)(UINT_PTR)g_scratch[i];
        if (p >= base && p < base + W8D_SCRATCH_SIZE) {
            fprintf(g_out, " y=S%d+%x", i, p - base);
            return;
        }
    }
    if (p == 0) {
        fprintf(g_out, " y=null");
        return;
    }
    if (CopyGuarded(buf, (const void*)(UINT_PTR)p, sizeof(buf))) {
        fprintf(g_out, " y=");
        WriteHex(buf, sizeof(buf));
    } else {
        fprintf(g_out, " y=bad");
    }
}

static void RunCase(const W8DiffHost* host, W8DCase* c)
{
    unsigned args[W8D_MAX_ARGS];
    unsigned fn = 0;
    int i;
    int bad_op = -1;
    unsigned eax = 0, edx = 0, sw0 = 0, sw1 = 0, code = 0, eip = 0;
    int called = 0;
    int ok = 1;
    unsigned char fp[8];
    int fp_delta = 0;
    unsigned short native_cw = GetCw();

    for (i = 0; i < W8D_SCRATCH_COUNT; ++i) {
        memset(g_scratch[i], 0, W8D_SCRATCH_SIZE);
    }
    for (i = 0; i < c->nops && bad_op < 0; ++i) {
        W8DOp* op = &c->ops[i];
        unsigned addr;
        if (!Eval(op->expr, &addr) || addr == 0) {
            bad_op = i;
            break;
        }
        op->ptr = (unsigned char*)(UINT_PTR)addr;
        if (op->kind == OP_DUMP) {
            continue;
        }
        op->saved = (unsigned char*)malloc(op->len ? op->len : 1);
        if (op->saved == 0 || !CopyGuarded(op->saved, op->ptr, op->len)) {
            free(op->saved);
            op->saved = 0;
            op->ptr = 0;
            bad_op = i;
            break;
        }
        if (op->kind == OP_SETP) {
            unsigned value;
            if (!Eval(op->value_expr, &value)) {
                bad_op = i;
                break;
            }
            if (!CopyGuarded(op->ptr, &value, 4)) {
                bad_op = i;
            }
        }
        if (op->kind == OP_SET || op->kind == OP_FILL) {
            /* Data only: no VirtualProtect, a write to read-only memory is a
               bad set-up, not something to force. */
            __try {
                if (op->kind == OP_SET && op->blob) {
                    memcpy(op->ptr, op->blob, op->len);
                } else if (op->kind == OP_SET) {
                    unsigned k;
                    for (k = 0; k < op->len; ++k) {
                        op->ptr[k] = (unsigned char)(Nibble(op->hex[2 * k]) * 16 +
                                                     Nibble(op->hex[2 * k + 1]));
                    }
                } else {
                    memset(op->ptr, op->fill, op->len);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                bad_op = i;
            }
        }
    }
    if (bad_op < 0 && c->has_fn) {
        for (i = 0; i < c->nargs; ++i) {
            const char* a = c->args[i];
            if (*a == '&') {
                if (!Eval(a + 1, &args[i])) {
                    bad_op = 1000 + i;
                    break;
                }
            } else {
                args[i] = Hex(&a);
            }
        }
        if (bad_op < 0 && !Eval(c->fn, &fn)) {
            bad_op = 2000;
        }
    }

    memset(fp, 0, sizeof(fp));
    if (bad_op < 0 && c->has_fn) {
        unsigned reg_ecx = 0, reg_edx = 0;
        const unsigned* stack_args = args;
        int nstack = c->nargs;
        if (c->conv == CONV_T && nstack > 0) {
            reg_ecx = args[0];
            stack_args = args + 1;
            nstack -= 1;
        } else if (c->conv == CONV_F) {
            if (nstack > 0) {
                reg_ecx = args[0];
            }
            if (nstack > 1) {
                reg_edx = args[1];
            }
            stack_args = args + (nstack > 2 ? 2 : nstack);
            nstack -= nstack > 2 ? 2 : nstack;
        }
        if (c->has_seed && host->seed) {
            host->seed(c->seed);
        }
        g_assert_count = 0;
        if (g_has_cw) {
            SetCw(g_cw);
        } else {
            SetCw(native_cw);
        }
        ok = GuardedCall(fn, reg_ecx, reg_edx, stack_args, nstack, &eax, &edx, &sw0, &sw1, &code,
                         &eip);
        called = 1;
        if (ok) {
            int top0 = (int)((sw0 >> 11) & 7);
            int top1 = (int)((sw1 >> 11) & 7);
            fp_delta = (top0 - top1 + 8) % 8;
            if (fp_delta == 1) {
                PopFp(fp);
            } else if (fp_delta != 0) {
                ResetFpu(native_cw);
            }
        } else {
            ResetFpu(native_cw);
        }
        SetCw(native_cw);
    }

    fprintf(g_out, "R %s ", c->id);
    if (bad_op >= 0) {
        fprintf(g_out, "X%d", bad_op);
    } else if (!called) {
        fprintf(g_out, "nocall");
    } else if (!ok) {
        fprintf(g_out, "E%08x@%08x", code, eip);
    } else {
        fprintf(g_out, "ok");
    }
    fprintf(g_out, " a=");
    if (g_assert_count == 0 || !called) {
        fprintf(g_out, "-");
    } else {
        for (i = 0; i < g_assert_count && i < W8D_MAX_ASSERTS; ++i) {
            fprintf(g_out, i ? ",%ld" : "%ld", g_assert_lines[i]);
        }
        if (g_assert_count > W8D_MAX_ASSERTS) {
            fprintf(g_out, ",+%d", g_assert_count - W8D_MAX_ASSERTS);
        }
    }
    fprintf(g_out, " eax=%08x edx=%08x fp=", eax, edx);
    if (fp_delta == 1) {
        WriteHex(fp, 8);
    } else {
        fprintf(g_out, "-");
    }
    fprintf(g_out, " fd=%d", fp_delta);
    if (called && ok) {
        if (c->out_kind == 'w') {
            WriteString(eax, 1);
        } else if (c->out_kind == 'c') {
            WriteString(eax, 0);
        } else if (c->out_kind == 'p') {
            WritePointer(eax);
        }
    }
    fprintf(g_out, "\n");
    if (bad_op < 0) {
        WriteCaps(c);
    }

    /* Undo in reverse order, so overlapping set-ups unwind correctly. */
    for (i = c->nops - 1; i >= 0; --i) {
        W8DOp* op = &c->ops[i];
        if (op->saved && op->ptr) {
            CopyGuarded(op->ptr, op->saved, op->len);
        }
        free(op->saved);
        op->saved = 0;
        op->ptr = 0;
    }
}

static void Finish(const W8DiffHost* host, const char* why)
{
    char path[MAX_PATH];
    FILE* done;
    if (g_out) {
        fprintf(g_out, "END %u %s\n", g_case_count, why);
        fclose(g_out);
        g_out = 0;
    }
    free(g_plan);
    g_plan = 0;
    g_cursor = 0;
    g_state = 0;
    {
        int b;
        for (b = 0; b < W8D_MAX_BLOBS; ++b) {
            free(g_blob[b]);
            g_blob[b] = 0;
            g_blob_len[b] = 0;
        }
    }
    Path(host, "done_", path);
    strcat(path, ".txt");
    done = fopen(path, "w");
    if (done) {
        fprintf(done, "%u %s\n", g_case_count, why);
        fclose(done);
    }
}

static void Start(const W8DiffHost* host)
{
    char path[MAX_PATH];
    FILE* f;
    long size;
    char* line;
    char* body;

    Path(host, "plan_", path);
    strcat(path, ".txt");
    f = fopen(path, "rb");
    if (!f) {
        return;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_plan = (char*)malloc((size_t)size + 1);
    if (!g_plan) {
        fclose(f);
        return;
    }
    fread(g_plan, 1, (size_t)size, f);
    g_plan[size] = 0;
    fclose(f);
    g_cursor = g_plan;

    /* 'resume <n>' before the first case: run.ps1 restarted us after a hang;
       skip n cases and append to the output it already has. */
    g_skip = 0;
    {
        char* rs = strstr(g_plan, "\nresume ");
        char* first = strstr(g_plan, "\ncase ");
        if (rs && (!first || rs < first)) {
            const char* v = rs + 8;
            g_skip = Hex(&v);
        }
    }
    Path(host, "out_", path);
    strcat(path, ".txt");
    g_out = fopen(path, g_skip ? "ab" : "wb");
    if (!g_out) {
        free(g_plan);
        g_plan = 0;
        return;
    }
    if (!g_skip) {
        fprintf(g_out, "W8DIFF-OUT 1 %s\ncw-native %04x\n", host->name, GetCw());
    }
    g_case_count = 0;
    g_has_cw = 0;
    g_ready_expr = 0;
    g_ready_count = 0;
    g_ready_started = GetTickCount();

    /* Header commands run until the first case; that line is kept for the
       run loop. */
    g_pending = 0;
    while ((line = NextLine()) != 0) {
        const char* cmd;
        if (strncmp(line, "case ", 5) == 0) {
            g_pending = line;
            break;
        }
        body = line;
        cmd = Word(&body);
        if (strcmp(cmd, "cw") == 0) {
            const char* v = Word(&body);
            g_has_cw = 1;
            g_cw = (unsigned short)Hex(&v);
        } else if (strcmp(cmd, "ready") == 0) {
            const char* v;
            g_ready_expr = Word(&body);
            v = Word(&body);
            g_ready_value = Hex(&v);
            v = Word(&body);
            g_ready_frames = Hex(&v);
        } else if (strcmp(cmd, "blob") == 0) {
            const char* v = Word(&body);
            const char* hex = Word(&body);
            unsigned id = Hex(&v);
            unsigned len = (unsigned)strlen(hex) / 2;
            if (id < W8D_MAX_BLOBS) {
                unsigned k;
                free(g_blob[id]);
                g_blob[id] = (unsigned char*)malloc(len ? len : 1);
                g_blob_len[id] = len;
                for (k = 0; g_blob[id] && k < len; ++k) {
                    g_blob[id][k] = (unsigned char)(Nibble(hex[2 * k]) * 16 + Nibble(hex[2 * k + 1]));
                }
            }
        } else if (strcmp(cmd, "region") == 0) {
            const char* name = Word(&body);
            const char* expr = Word(&body);
            const char* v = Word(&body);
            unsigned addr = 0;
            unsigned len = Hex(&v);
            if (g_skip) {
                /* the first run already wrote the header */
            } else if (Eval(expr, &addr)) {
                fprintf(g_out, "G %s %08x %x\n", name, addr, len);
            } else {
                fprintf(g_out, "G %s bad %x\n", name, len);
            }
        }
    }
    g_state = g_ready_expr ? 1 : 2;
}

int W8DiffPoll(const W8DiffHost* host)
{
    DWORD now = GetTickCount();
    if (g_state == 0) {
        char path[MAX_PATH];
        if (now - g_last_check < 250) {
            return 0;
        }
        g_last_check = now;
        Path(host, "", path);
        strcat(path, ".req");
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
            return 0;
        }
        DeleteFileA(path);
        Start(host);
        return g_state != 0;
    }
    if (g_state == 1) {
        unsigned v = 0;
        if (Eval(g_ready_expr, &v)) {
            __try {
                v = *(const unsigned*)(UINT_PTR)v;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                v = ~g_ready_value;
            }
        }
        g_ready_count = v == g_ready_value ? g_ready_count + 1 : 0;
        if (g_ready_count >= g_ready_frames) {
            fprintf(g_out, "ready %u ms\n", now - g_ready_started);
            g_state = 2;
        } else if (now - g_ready_started > 180000) {
            Finish(host, "ready-timeout");
        }
        return 1;
    }
    {
        static W8DCase c;
        char* line;
        PatchAsserts(1);
        while (GetTickCount() - now < 150) {
            char* body;
            const char* cmd;
            if (g_pending) {
                line = g_pending;
                g_pending = 0;
            } else {
                line = NextLine();
            }
            if (!line) {
                PatchAsserts(0);
                fflush(g_out);
                Finish(host, "ok");
                return 0;
            }
            body = line;
            cmd = Word(&body);
            if (strcmp(cmd, "case") != 0) {
                continue;
            }
            if (!ParseCase(body, &c)) {
                break;
            }
            if (g_skip) {
                --g_skip;
                continue;
            }
            /* Start marker: if the call never returns, run.ps1 sees which
               case hung, kills the game and restarts after it. */
            fprintf(g_out, "S %s\n", c.id);
            fflush(g_out);
            RunCase(host, &c);
            ++g_case_count;
        }
        PatchAsserts(0);
        fflush(g_out);
    }
    return 1;
}
