/* Experimental (2026-10-23, the patch author's request), part of [Experimental] FrameRateFix:
   the rest of the original's frame-rate-dependent behaviour (FixedTick.cpp does the party's
   movement). Where the code states no duration, the reference is the original at 30 frames
   per second, the rate of the movement step: at 30 fps it behaves as the original does at
   30 fps, and at any other frame rate the same as that. Where it states one (the sound fades),
   the designed duration is kept. Audits: build\tmp\fps-indep\ (not in git).

   1. Monster group AI (UpdateMonsterGroups 0x00530150, called once per frame at 0x005603ee):
      runs 30 times per real second (0..4 calls per frame; also while camping or resting,
      when the game clock runs faster), so the look-about roll (Random(20) per
      evaluation while a monster pauses) and the cooldowns counted in evaluations (0x1e after
      facing a noise, 0x14 after a patrol failure) run at the 30 fps rate.
   2. Per-frame random rolls: the monster idle fidget (TryStartMonsterCycle2 0x004e67a0, the
      Random call at 0x004e68a3) rolls once per logic tick; a flickering light
      (stLight::Update 0x0049c960, the block at 0x0049cf23) takes one on/off step per tick, so
      a flash starts with the same chance per second and lasts one tick (33 ms), as at 30 fps.
   3. Floating damage numbers (W8Monster::UpdateAttachedObjects 0x004c3f70, 0x004c4507): each
      frame the original lifts them by 0.2 * (ms since the monster's last animation frame), so
      they rise faster the more frames there are (3.1 s on screen at 30 fps, 0.25 s at
      1000 fps). The lift is now scaled to the frame time and the elapsed time is taken as at
      30 fps: identical at 30 fps, the same speed and life at any rate.
   4. Sound fades. Here the code states the length, so the designed length is the reference,
      not 30 fps (the owner's decision: "the experience the developers intended"). An SGP fade
      (SoundSetFadeVolume: a length in ms, e.g. 500, split into one period per volume step;
      SoundServiceStreams 0x004095b0, the check at 0x00409719) makes at most one step per call
      and restarts its period at the call; an ambient zone fade (period 1.5 s / target volume;
      W8AmbientSound::UpdateFade 0x0047a310, the GetProgress call at 0x0047a333) at most one
      step per frame. A full 0..127 fade took 4.2 s at 30 fps and ~2 s at 64+ fps. Both now
      step every period on the fade's own timeline and take all the steps due in a frame (an
      ambient fade at most what the frame's time can owe: its start leaves the timer far
      behind): the designed length at any frame rate.
   5. Automap arrow-key panning: AutomapScreenFrame (transplanted) scales its per-frame step by
      FrameRateAutomapPanScale (frame time / (1/30 s)).
   6. Mouselook smoothing (ApplyPendingMouselook 0x00568c40, the fraction at 0x00568c65): the
      original drains 0.2 of the pending turn per frame under 20 ms of GetTickCount time, then
      0.2 per whole 10 ms (integer division), 1.0 above 50 ms. At 30 fps that is 0.6 per frame;
      now the fraction is 1 - 0.4^(dt * 30) from the real frame time (QueryPerformanceCounter):
      0.6 at 30 fps, the same settling time at any rate. Smoothing off is untouched.
   7. Camera paths (PathAITick 0x004aa1f0) and the texture scroll clock (UpdateRenderElapsedTime
      0x00482140) read GetTickCount (15.6 ms steps: stop-go judder above 64 fps); they now read a
      millisecond clock from QueryPerformanceCounter that never runs behind GetTickCount.

   Each patch goes over the exact original bytes only, and not into a function the hook list
   replaced; each checks the switch every time it runs and does the original's work when off. */
#include <windows.h>
#include <string.h>
#include "wiz8/experimental.h"
#include "patch_util.h"
#include "wiz8/engine_code/GameTimeAccumulator.h"

#include <stdio.h>

namespace {

/* Addresses used by the C++ below are named here; the naked stubs write theirs as literals with
   a comment (inline asm reads a C++ constant as a memory operand, not as a number). */

/* The reference rate and the logic tick. */
const int kTickRate = 30;
const int kClockRate = 10000;                       /* W8GameTimer units per second */
const int kMaxTicks = 4;                            /* at most this many logic ticks in a frame */
const int kMaxFrameUnits = 2000;                    /* the accumulator's own frame cap (0.2 s) */
const float kReferenceFrameMs = 1000.0f / kTickRate;

/* W8GameTimer / W8GameTimeAccumulator fields (game_timer.h, GameTimeAccumulator.h). */
const unsigned kTimerFlags = 0x08;                  /* bit 0x8: paused */
const unsigned kTimerDuration = 0x18;
const unsigned kFrameDelta = 0x28;

/* 1. Monster group AI. */
const unsigned kMainGameScreenFrame = 0x0055fb30;
const unsigned kMonsterGroupsCall = 0x005603ee;     /* push 1 before; call rel32 */
const unsigned kUpdateMonsterGroups = 0x00530150;   /* cdecl void (bool staggered) */
/* 2. Rolls. */
const unsigned kTryStartMonsterCycle2 = 0x004e67a0;
const unsigned kFidgetRandomCall = 0x004e68a3;      /* call rel32 -> Random */
const unsigned kRandom = 0x0040efa0;                /* cdecl UINT32 Random(UINT32) */
const unsigned kLightUpdate = 0x0049c960;
const unsigned kFlickerBlock = 0x0049cf23;          /* mov ebx, [0x5ebb00] (6 bytes) */
const unsigned kFlickerRandCall = 0x0049cf4d;       /* call [kRandImport] */
const unsigned kNodeTestFlag = 0x005ebb00;          /* import slots: thiscall (int flag) */
const unsigned kNodeSetFlag = 0x005eb5fc;
const unsigned kNodeClearFlag = 0x005eb600;
const unsigned kRandImport = 0x005eb1d0;            /* the game's rand() */
const unsigned kFlickerRandScale = 0x005ec1e4;      /* 1/32767 */
const unsigned kFlickerChance = 0x0c;               /* stParametricLightDefinition::flicker_chance */
const unsigned kSetAnimatedTextureFrame = 0x004b9b00; /* cdecl (instance, frame) */
/* 3. Damage numbers. */
const unsigned kUpdateAttachedObjects = 0x004c3f70;
const unsigned kPosterRise = 0x004c4507;            /* elapsed * 0.2 -> [esp+0x40], 35 bytes */
const unsigned kPosterRiseEnd = 0x004c452a;
const unsigned kPosterRate = 0x005ed298;            /* 0.2 */
/* 4. Sound fades. */
const unsigned kSoundServiceStreams = 0x004095b0;
const unsigned kSoundFadeCheck = 0x00409719;        /* fade time + rate vs now, 16 bytes */
const unsigned kSoundFadeStore = 0x004097bf;        /* mov [edi+0x24], ebp: the time of the step */
const unsigned kAmbientUpdateFade = 0x0047a310;
const unsigned kAmbientProgressCall = 0x0047a333;   /* call rel32 -> GetProgress */
const unsigned kTimerGetProgress = 0x0043a190;      /* thiscall float W8GameTimer::GetProgress() */
const unsigned kAmbientTimer = 0x108;               /* W8AmbientSound::fade_timer */
const unsigned kAmbientTarget = 0x9c;
const unsigned kAmbientCurrent = 0xa0;
/* 6. Mouselook. */
const unsigned kApplyPendingMouselook = 0x00568c40;
const unsigned kMouselookClock = 0x00568c4e;        /* mov esi, [GetTickCount] */
const unsigned kMouselookScale = 0x00568c65;        /* call esi (GetTickCount); mov edx, [last] */
const unsigned kMouselookStore = 0x00568c74;        /* mov [last], eax; pop esi */
/* 7. Clocks. */
const unsigned kPathAITick = 0x004aa1f0;
const unsigned kPathAITickClock = 0x004aa219;
const unsigned kUpdateRenderElapsedTime = 0x00482140;
const unsigned kRenderElapsedClock = 0x00482143;

/* Plain statics (the hybrid has no C runtime start-up). */
int s_frame_valid;      /* the fields below describe a frame */
unsigned s_frame_key;   /* s_frame_count of that frame */
unsigned s_frame_count; /* rendered frames (FrameRateCountFrame), never reset */
int s_have_start;
int s_tick_acc;         /* clock units * kTickRate not yet ticked */
int s_frame_ticks;      /* logic ticks of the current frame */
int s_frame_units;      /* clock units of the current frame (0 while paused) */

unsigned s_fade_time;              /* SoundFadeDue -> the stub: the fade time to store */
unsigned s_fade_volume;            /* SoundFadeDue -> the stub: the volume before the last step */

double s_ms_per_count;             /* QueryPerformanceCounter period, ms */
LONGLONG s_pan_last;               /* automap panning: the last frame's counter */
LONGLONG s_look_last;              /* mouselook: the last frame's counter */
int s_clock_based;                 /* the millisecond clock: base set */
DWORD s_clock_base_tick;
LONGLONG s_clock_base_count;
DWORD s_clock_last;                /* the last value it returned */
int s_clock_used;

char s_status[320];

/* [Experimental] FrameRateFix: -1 = not read yet, 0 = the original's behaviour, 1 = on. */
volatile int s_frame_rate_fix = -1;

bool On(void)
{
    return ExperimentalFrameRateFixEnabled();
}

int __cdecl OnFlag(void)
{
    return ExperimentalFrameRateFixEnabled() ? 1 : 0;
}


LONGLONG Counter(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (s_ms_per_count == 0.0) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        s_ms_per_count = frequency.QuadPart > 0 ? 1000.0 / static_cast<double>(frequency.QuadPart) : 1.0;
    }
    return now.QuadPart;
}

/* Seconds since *last (updated); a first call or a gap longer than `limit` gives -1. */
double SecondsSince(LONGLONG* last, double limit)
{
    LONGLONG now = Counter();
    double seconds = -1.0;
    if (*last != 0) {
        seconds = static_cast<double>(now - *last) * s_ms_per_count * 0.001;
        if (seconds < 0.0 || seconds > limit) {
            seconds = -1.0;
        }
    }
    *last = now;
    return seconds;
}

/* 0.4^x for x >= 0 without the C runtime: whole powers, then the series of e^(x ln 0.4). */
double PowPointFour(double x)
{
    double result = 1.0;
    while (x >= 1.0) {
        result *= 0.4;
        x -= 1.0;
    }
    const double y = x * -0.916290731874155; /* ln 0.4 */
    double term = 1.0;
    double sum = 1.0;
    for (int n = 1; n <= 9; ++n) {
        term *= y / n;
        sum += term;
    }
    return result * sum;
}

/* The logic ticks of this frame. A new frame starts when RenderFrame has counted one; the game
   frame clock's delta (W8GameTimeAccumulator, updated once per main-screen frame) in real time
   becomes ticks at kTickRate. While the game is paused the delta is 0: no ticks. */
void UpdateFrame(void)
{
    if (g_game_time_accumulator == 0) {
        s_frame_valid = 0;
        s_frame_ticks = 1;
        s_frame_units = kClockRate / kTickRate;
        return;
    }
    /* the frame's time is the clock's own frame delta (tenths of a second, capped, 0 while
       paused), not its start: pausing and resuming the shared timers inside a frame (asset
       loads) rewrite the start */
    const char* timer = reinterpret_cast<const char*>(g_game_time_accumulator);
    if (s_frame_valid && s_frame_key == s_frame_count) {
        return;
    }
    s_frame_valid = 1;
    s_frame_key = s_frame_count;
    s_frame_ticks = 0;
    s_frame_units = 0;
    if (!s_have_start) {
        s_have_start = 1;
        s_tick_acc = kClockRate / 2; /* ticks fall mid-way between 30 fps frames: jitter-proof */
    }
    const float tenths = *reinterpret_cast<const float*>(timer + kFrameDelta);
    if (!(tenths > 0.0f)) {
        return; /* paused or resuming: no time */
    }
    /* the delta is game time: clock units / m_duration, and m_duration is 1000 scaled by the
       level's gameplay time scale (SetDurationScale 0x0043a960, e.g. while camping or
       resting). The ticks count real time, as the original's frames do. */
    const int duration = *reinterpret_cast<const int*>(timer + kTimerDuration);
    unsigned units = static_cast<unsigned>(tenths * static_cast<float>(duration > 0 ? duration : kClockRate / 10) + 0.5f);
    if (units > static_cast<unsigned>(kMaxFrameUnits)) {
        units = kMaxFrameUnits;
    }
    s_frame_units = static_cast<int>(units);
    s_tick_acc += static_cast<int>(units) * kTickRate;
    s_frame_ticks = s_tick_acc / kClockRate;
    s_tick_acc %= kClockRate;
    if (s_frame_ticks > kMaxTicks) {
        s_frame_ticks = kMaxTicks;
    }
}

/* ---- 1. Monster group AI ---- */

/* Called instead of UpdateMonsterGroups from MainGameScreenFrame (cdecl, the caller pops). */
void __cdecl MonsterGroupsTick(int staggered)
{
    typedef void(__cdecl * UpdateMonsterGroupsFn)(int);
    UpdateMonsterGroupsFn update = reinterpret_cast<UpdateMonsterGroupsFn>(kUpdateMonsterGroups);
    if (!On()) {
        update(staggered);
        return;
    }
    for (int ticks = FrameRateLogicTicks(); ticks > 0; --ticks) {
        update(staggered);
    }
}

/* ---- 2. Rolls ---- */

/* Called instead of Random(range) in TryStartMonsterCycle2 (0 = start the fidget). */
unsigned __cdecl FidgetRandom(unsigned range)
{
    typedef unsigned(__cdecl * RandomFn)(unsigned);
    RandomFn random = reinterpret_cast<RandomFn>(kRandom);
    if (!On()) {
        return random(range);
    }
    for (int ticks = FrameRateLogicTicks(); ticks > 0; --ticks) {
        if (random(range) == 0) {
            return 0;
        }
    }
    return 1;
}

/* The flicker block of stLight::Update, once per logic tick (the original's code, once per
   frame): a lit light goes dark; a dark one lights up with flicker_chance per step. The
   light's prop shows its lit texture frame (0) or the dark one (1). */
void __cdecl FlickerTicks(void* light, const char* definition, void* instance)
{
    typedef int(__fastcall * FlagFn)(void*, void*, int);
    typedef void(__cdecl * FrameFn)(void*, int);
    typedef int(__cdecl * RandFn)(void);
    FlagFn test = *reinterpret_cast<FlagFn*>(kNodeTestFlag);
    FlagFn set = *reinterpret_cast<FlagFn*>(kNodeSetFlag);
    FlagFn clear = *reinterpret_cast<FlagFn*>(kNodeClearFlag);
    FrameFn frame = reinterpret_cast<FrameFn>(kSetAnimatedTextureFrame);
    RandFn rand_fn = *reinterpret_cast<RandFn*>(kRandImport);
    const double scale = *reinterpret_cast<const float*>(kFlickerRandScale);
    const double chance = *reinterpret_cast<const float*>(definition + kFlickerChance);
    for (int ticks = FrameRateLogicTicks(); ticks > 0; --ticks) {
        if (test(light, 0, 0) == 0) {
            set(light, 0, 0);
            if (instance != 0) {
                frame(instance, 1);
            }
        } else if (static_cast<double>(rand_fn()) * scale < chance) {
            /* both exact in a double, as the original's x87 product */
            if (test(light, 0, 0) == 0) {
                set(light, 0, 0);
                if (instance != 0) {
                    frame(instance, 1);
                }
            } else {
                clear(light, 0, 0);
                if (instance != 0) {
                    frame(instance, 0);
                }
            }
        }
    }
}

/* Reached by a jmp instead of `mov ebx, [testFlag]` at the start of the flicker block;
   ebp = the light, esi = its definition, edi = the prop's model instance or 0. */
__declspec(naked) void FlickerStub(void)
{
    __asm {
        call OnFlag
        test eax, eax
        jnz fixed
        mov ebx, dword ptr ds:[0x005ebb00] /* kNodeTestFlag */
        push 0x0049cf29 /* back after the replaced instruction */
        ret
    fixed:
        push edi
        push esi
        push ebp
        call FlickerTicks
        add esp, 12
        push 0x0049cf92 /* past the original's flicker block */
        ret
    }
}

/* ---- 3. Damage numbers ---- */

/* The lift of the damage numbers this frame from `elapsed` (ms since the monster's animation
   timer). Off: elapsed * 0.2 exactly as the original (x87, rounded to a float). On: the
   original's per-frame lift at 30 fps, scaled to this frame's time; the elapsed time is taken
   as at 30 fps (the timer had one 30 fps frame more, not this frame, to run). */
float __cdecl PosterRise(unsigned elapsed)
{
    float rise;
    if (!On()) {
        unsigned wide[2];
        wide[0] = elapsed;
        wide[1] = 0;
        __asm {
            lea eax, wide
            fild qword ptr [eax]
            mov eax, 0x005ed298 /* kPosterRate */
            fmul dword ptr [eax]
            fstp rise
        }
        return rise;
    }
    UpdateFrame();
    const float frame_ms = s_frame_units * (1000.0f / kClockRate);
    float base = static_cast<float>(elapsed) + kReferenceFrameMs - frame_ms;
    if (base < 0.0f) {
        base = 0.0f;
    }
    rise = *reinterpret_cast<const float*>(kPosterRate) * base * (frame_ms / kReferenceFrameMs);
    return rise;
}

/* ---- 4. Sound fades ---- */

/* SoundServiceStreams, one playing channel whose volume is not yet the fade's target: whether
   to take a step now. `entry` = edi (the channel's fields: +0x1c target volume, +0x20 step
   period in ms = the fade's length / the volume difference, +0x24 time of the last step),
   `volume` = esi (the current volume). s_fade_time = the time to store as the last step,
   s_fade_volume = the volume the original's single step then starts from. Off: the original
   (a step once the period has passed since the last step, at most one per call; the time
   stored is now). On: the designed fade: a step every period on the fade's own timeline (no
   drift), all the steps due in this call taken at once (the original's step does the last
   one), so the fade lasts the length its caller asked for at any frame rate. */
int __cdecl SoundFadeDue(const unsigned char* entry, unsigned now, unsigned volume)
{
    const unsigned target = *reinterpret_cast<const unsigned*>(entry + 0x1c);
    const unsigned rate = *reinterpret_cast<const unsigned*>(entry + 0x20);
    const unsigned last = *reinterpret_cast<const unsigned*>(entry + 0x24);
    s_fade_volume = volume;
    if (!On()) {
        s_fade_time = now;
        return now >= last + rate ? 1 : 0;
    }
    const unsigned elapsed = now - last;
    unsigned steps = 1;
    unsigned next = now;
    if (elapsed < 0x10000000) { /* else a time from long ago: one step from now */
        if (rate == 0) {
            steps = 128; /* no period: the whole fade at once */
        } else {
            steps = elapsed / rate;
            if (steps == 0) {
                return 0;
            }
            next = last + steps * rate;
        }
    }
    const unsigned distance = volume > target ? volume - target : target - volume;
    if (volume > 127 || steps > distance) { /* no volume read (-1): one step as the original */
        steps = volume > 127 ? 1 : distance;
    }

    /* the original's step code takes the last step (and stops a sound faded to zero) */
    if (volume < target) {
        s_fade_volume = volume + (steps - 1);
    } else {
        s_fade_volume = volume - (steps - 1);
    }
    s_fade_time = next;
    return 1;
}

/* Reached by a jmp at 0x00409719 instead of the original's period check; eax = the target
   volume (kept for the step code), esi = the current volume (moved by the steps beyond the
   original's one), ebp = now (becomes the time stored after the step). */
__declspec(naked) void SoundFadeStub(void)
{
    __asm {
        push eax
        push esi
        push ebp
        push edi
        call SoundFadeDue
        add esp, 12
        test eax, eax
        pop eax
        jz skip
        mov ebp, s_fade_time
        mov esi, s_fade_volume
        push 0x00409729 /* the original's volume step */
        ret
    skip:
        push 0x004097c2 /* past it: no step now */
        ret
    }
}

/* Called instead of fade_timer.GetProgress() in W8AmbientSound::UpdateFade (thiscall: the
   timer in ecx, no arguments - what __fastcall does with an unused edx). The original takes
   one volume step whenever at least one step period (1.5 s / the target volume) has passed,
   at most one per frame and the rest of the periods dropped. On: the steps due beyond the
   original's own are taken here, so the fade lasts the designed 1.5 s at any frame rate. */
float __fastcall AmbientFadeProgress(void* timer, void* /*edx*/)
{
    typedef float(__fastcall * GetProgressFn)(void*, void*);
    GetProgressFn get_progress = reinterpret_cast<GetProgressFn>(kTimerGetProgress);
    char* fields = static_cast<char*>(timer);
    const float progress = get_progress(timer, 0);
    if (!On() || (*reinterpret_cast<unsigned short*>(fields + kTimerFlags) & 0x8) != 0 || !(progress >= 2.0f)) {
        return progress;
    }
    /* every fade start (W8AmbientSound, Restart then m_start = now - m_start) leaves the timer's
       start near 0, so its first progress is the whole clock / the period: no time owed. Only
       what this frame can have owed is caught up (its periods + the one already begun). */
    UpdateFrame();
    const int period = *reinterpret_cast<const int*>(fields + kTimerDuration);
    const float owed = static_cast<float>(s_frame_units) / static_cast<float>(period > 0 ? period : 1) + 1.0f;
    if (progress <= owed) {
        char* sound = fields - kAmbientTimer;
        unsigned* target = reinterpret_cast<unsigned*>(sound + kAmbientTarget);
        unsigned* current = reinterpret_cast<unsigned*>(sound + kAmbientCurrent);
        int extra = progress >= 128.0f ? 127 : static_cast<int>(progress) - 1;
        for (; extra > 0 && *current != *target; --extra) {
            if (*target > *current) {
                ++*current;
            } else {
                --*current;
            }
        }
    }
    return progress;
}

/* ---- 6. Mouselook ---- */

/* The share of the pending mouselook turn applied this frame (smoothing on): 0.6 per 1/30 s,
   compounded over the real frame time. */
float __cdecl MouselookScaleOn(void)
{
    double seconds = SecondsSince(&s_look_last, 0.25);
    if (seconds < 0.0) {
        return 1.0f;
    }
    return static_cast<float>(1.0 - PowPointFour(seconds * kTickRate));
}

/* Reached by a jmp at 0x00568c65 instead of `call esi` (GetTickCount, esi still pushed). Off:
   that call and the next instruction, back to the original. On: the same bookkeeping of the
   last tick, then the fraction from MouselookScaleOn in st(0). */
__declspec(naked) void MouselookScaleStub(void)
{
    __asm {
        call OnFlag
        test eax, eax
        jnz fixed
        call esi
        mov edx, dword ptr ds:[0x0068edb0] /* the last tick */
        push 0x00568c6d /* back after the replaced instructions */
        ret
    fixed:
        call esi
        mov dword ptr ds:[0x0068edb0], eax /* the last tick */
        pop esi
        call MouselookScaleOn
        push 0x00568cb1 /* past the original's fraction (it leaves it in st(0)) */
        ret
    }
}

/* ---- 7. Clocks ---- */

/* Called instead of GetTickCount by PathAITick and UpdateRenderElapsedTime. On: milliseconds
   from QueryPerformanceCounter, kept 0..48 ms ahead of GetTickCount (its 15.6 ms steps; so a
   time stored from GetTickCount elsewhere is never ahead of it) and never going back. Off:
   GetTickCount, held at the last value given while on until GetTickCount passes it. */
DWORD WINAPI SmoothTickCount(void)
{
    const DWORD tick = GetTickCount();
    if (!On()) {
        if (s_clock_used && static_cast<int>(s_clock_last - tick) > 0) {
            return s_clock_last;
        }
        s_clock_used = 0;
        return tick;
    }
    const LONGLONG count = Counter();
    DWORD value = 0;
    if (s_clock_based) {
        value = s_clock_base_tick + static_cast<DWORD>(static_cast<double>(count - s_clock_base_count) * s_ms_per_count);
    }
    const int ahead = static_cast<int>(value - tick);
    if (!s_clock_based || ahead < 0 || ahead > 48) {
        s_clock_based = 1;
        s_clock_base_tick = tick + 16;
        s_clock_base_count = count;
        value = s_clock_base_tick;
    }
    if (s_clock_used && static_cast<int>(s_clock_last - value) > 0) {
        value = s_clock_last;
    }
    s_clock_last = value;
    s_clock_used = 1;
    return value;
}

} // namespace

void FrameRateCountFrame(void)
{
    ++s_frame_count;
}

unsigned FrameRateFrameCount(void)
{
    return s_frame_count;
}

int FrameRateLogicTicks(void)
{
    if (!On()) {
        return 1;
    }
    UpdateFrame();
    return s_frame_ticks;
}

float FrameRateAutomapPanScale(void)
{
    /* measured every frame, so the time is this frame's */
    double seconds = SecondsSince(&s_pan_last, 0.1);
    if (!On() || seconds < 0.0) {
        return 1.0f; /* switch off, or the first frame on the map */
    }
    return static_cast<float>(seconds * kTickRate);
}

bool ExperimentalFrameRateFixEnabled(void)
{
    if (s_frame_rate_fix < 0) {
        s_frame_rate_fix = ExperimentalReadSwitch("FrameRateFix") ? 1 : 0;
    }
    return s_frame_rate_fix == 1;
}

void ExperimentalSetFrameRateFix(bool enabled)
{
    s_frame_rate_fix = enabled ? 1 : 0;
    ExperimentalWriteSwitch("FrameRateFix", enabled);
}

const char* ExperimentalInstallFrameRateFix(void)
{
    static char s_line[480];
    const char* tick = FixedTickInstall();
    const char* rates = FrameRateFixesInstall();
    _snprintf(s_line, sizeof(s_line), "%s; fixed movement step %s, %s",
              ExperimentalFrameRateFixEnabled() ? "on" : "off (the original's behaviour)", tick, rates);
    s_line[sizeof(s_line) - 1] = '\0';
    return s_line;
}

const char* FrameRateFixesInstall(void)
{
    /* 1. */
    const unsigned char push_one[2] = {0x6a, 0x01};
    const bool groups_ok = PatchCallsTo(kMonsterGroupsCall, kUpdateMonsterGroups) &&
                           PatchBytes(kMonsterGroupsCall - 2, push_one, 2) && PatchOriginal(kMainGameScreenFrame) &&
                           PatchOriginal(kUpdateMonsterGroups);
    const bool groups = groups_ok && PatchBranch(kMonsterGroupsCall, 0xe8, reinterpret_cast<const void*>(&MonsterGroupsTick), 5);
    /* 2. */
    const bool fidget_ok = PatchCallsTo(kFidgetRandomCall, kRandom) && PatchOriginal(kTryStartMonsterCycle2);
    const bool fidget = fidget_ok && PatchBranch(kFidgetRandomCall, 0xe8, reinterpret_cast<const void*>(&FidgetRandom), 5);
    const unsigned char flicker_bytes[] = {0x8b, 0x1d, 0x00, 0xbb, 0x5e, 0x00, 0x6a, 0x00, 0x8b, 0xcd, 0xff, 0xd3,
                                           0x85, 0xc0, 0x75, 0x1a, 0x50, 0x8b, 0xcd, 0xff, 0x15, 0xfc, 0xb5, 0x5e,
                                           0x00};
    const unsigned char flicker_rand[] = {0xff, 0x15, 0xd0, 0xb1, 0x5e, 0x00};
    const bool flicker_ok = PatchBytes(kFlickerBlock, flicker_bytes, sizeof(flicker_bytes)) &&
                            PatchBytes(kFlickerRandCall, flicker_rand, sizeof(flicker_rand)) && PatchOriginal(kLightUpdate);
    const bool flicker = flicker_ok && PatchBranch(kFlickerBlock, 0xe9, reinterpret_cast<const void*>(&FlickerStub), 6);
    /* 3. */
    const unsigned char poster_bytes[] = {0x8b, 0x54, 0x24, 0x40, 0x89, 0x94, 0x24, 0x80, 0x00, 0x00, 0x00,
                                          0x89, 0xbc, 0x24, 0x84, 0x00, 0x00, 0x00, 0xdf, 0xac, 0x24, 0x80,
                                          0x00, 0x00, 0x00, 0xd8, 0x0d, 0x98, 0xd2, 0x5e, 0x00, 0xd9, 0x5c,
                                          0x24, 0x40};
    const bool poster_ok = sizeof(poster_bytes) == kPosterRiseEnd - kPosterRise &&
                           PatchBytes(kPosterRise, poster_bytes, sizeof(poster_bytes)) && PatchOriginal(kUpdateAttachedObjects);
    bool poster = false;
    if (poster_ok) {
        unsigned char code[sizeof(poster_bytes)];
        memset(code, 0x90, sizeof(code));
        const unsigned char head[4] = {0xff, 0x74, 0x24, 0x40}; /* push dword ptr [esp+0x40] */
        memcpy(code, head, 4);
        code[4] = 0xe8;                                       /* call PosterRise */
        int rel = static_cast<int>(reinterpret_cast<unsigned>(&PosterRise) - (kPosterRise + 9));
        memcpy(code + 5, &rel, 4);
        const unsigned char tail[9] = {0x83, 0xc4, 0x04,       /* add esp, 4 */
                                       0xd9, 0x5c, 0x24, 0x40, /* fstp dword ptr [esp+0x40] */
                                       0xeb, 0x00};            /* jmp short kPosterRiseEnd */
        memcpy(code + 9, tail, 9);
        code[17] = static_cast<unsigned char>(kPosterRiseEnd - (kPosterRise + 18));
        poster = PatchWrite(kPosterRise, code, sizeof(code));
    }
    /* 4. */
    const unsigned char fade_bytes[] = {0x8b, 0x4f, 0x20, 0x8b, 0x57, 0x24, 0x03, 0xca, 0x3b, 0xe9,
                                        0x0f, 0x82, 0x99, 0x00, 0x00, 0x00};
    const unsigned char fade_store[] = {0x89, 0x6f, 0x24}; /* mov [edi+0x24], ebp */
    const bool fade_ok = PatchBytes(kSoundFadeCheck, fade_bytes, sizeof(fade_bytes)) &&
                         PatchBytes(kSoundFadeStore, fade_store, sizeof(fade_store)) && PatchOriginal(kSoundServiceStreams);
    const bool fade = fade_ok && PatchBranch(kSoundFadeCheck, 0xe9, reinterpret_cast<const void*>(&SoundFadeStub),
                                             sizeof(fade_bytes));
    const bool ambient_ok = PatchCallsTo(kAmbientProgressCall, kTimerGetProgress) && PatchOriginal(kAmbientUpdateFade) &&
                            PatchOriginal(kTimerGetProgress);
    const bool ambient = ambient_ok && PatchBranch(kAmbientProgressCall, 0xe8,
                                                   reinterpret_cast<const void*>(&AmbientFadeProgress), 5);
    /* 6. */
    const unsigned char look_bytes[] = {0xff, 0xd6, 0x8b, 0x15, 0xb0, 0xed, 0x68, 0x00};
    const unsigned char look_store[] = {0xa3, 0xb0, 0xed, 0x68, 0x00, 0x5e};
    const unsigned char look_clock[] = {0x8b, 0x35, 0x2c, 0xb0, 0x5e, 0x00};
    const bool look_ok = PatchBytes(kMouselookScale, look_bytes, sizeof(look_bytes)) &&
                         PatchBytes(kMouselookStore, look_store, sizeof(look_store)) &&
                         PatchBytes(kMouselookClock, look_clock, sizeof(look_clock)) && PatchOriginal(kApplyPendingMouselook);
    const bool look = look_ok && PatchBranch(kMouselookScale, 0xe9, reinterpret_cast<const void*>(&MouselookScaleStub),
                                             sizeof(look_bytes));
    /* 7. */
    const unsigned char tick_call[6] = {0xff, 0x15, 0x2c, 0xb0, 0x5e, 0x00}; /* call [GetTickCount] */
    const bool path_ok = PatchBytes(kPathAITickClock, tick_call, 6) && PatchOriginal(kPathAITick);
    const bool path = path_ok && PatchBranch(kPathAITickClock, 0xe8, reinterpret_cast<const void*>(&SmoothTickCount), 6);
    const bool scroll_ok = PatchBytes(kRenderElapsedClock, tick_call, 6) && PatchOriginal(kUpdateRenderElapsedTime);
    const bool scroll = scroll_ok && PatchBranch(kRenderElapsedClock, 0xe8,
                                                 reinterpret_cast<const void*>(&SmoothTickCount), 6);

    _snprintf(s_status, sizeof(s_status),
              "monster AI rate %s, idle fidget %s, light flicker %s, damage numbers %s, sound fades %s, "
              "ambient fades %s, mouselook smoothing %s, camera path clock %s, texture scroll clock %s",
              PatchResult(groups_ok, groups), PatchResult(fidget_ok, fidget), PatchResult(flicker_ok, flicker),
              PatchResult(poster_ok, poster), PatchResult(fade_ok, fade), PatchResult(ambient_ok, ambient),
              PatchResult(look_ok, look), PatchResult(path_ok, path), PatchResult(scroll_ok, scroll));
    s_status[sizeof(s_status) - 1] = '\0';
    return s_status;
}
