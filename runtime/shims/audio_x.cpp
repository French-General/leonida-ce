// HACK (Leonida CE boot): the console's audio decoder (xapu.dll) and audio DSP (chad.dll), answering their protocols
// with silence. Ported from Marvisak/VIctory (GPLv3), crates/victory-hle/src/modules/apu.rs and dsp.rs: the command
// and result layouts, the status block, and "commands complete when queued". VIctory decodes Opus; this writes zeros,
// so the title's audio engine runs (and stops waiting) with no sound.
// magic code, don't wanna deal with hours of plumbing with what these conjured devils of the souls of gpt 6 astra did.. jeez
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include "shims.h"

namespace {

// ---- xapu.dll -------------------------------------------------------------------------------------------------
constexpr HRESULT kQueueFull = static_cast<HRESULT>(0x8AC81000u);
constexpr HRESULT kPendingResults = static_cast<HRESULT>(0x8AC81001u);
constexpr HRESULT kInvalidCommand = static_cast<HRESULT>(0x8AC81002u);
constexpr HRESULT kInvalidStream = static_cast<HRESULT>(0x8AC81003u);
constexpr std::uint32_t kActivate = 1, kDeactivate = 2, kProcess = 3, kNotify = 4, kReset = 5;
constexpr std::uint32_t kSignal = 1;
constexpr std::uint32_t kResultEvent = 8;

#pragma pack(push, 1)
struct ConnectInput { std::uint32_t process, streams, commands_per_stream, memory, frames_per_output, options; };
struct CommandId { std::uint32_t kind, stream, sequence, options; std::uint64_t context; };
struct Activate { CommandId id; std::uint32_t channels, init_length; const void* init; float start_pitch;
                  std::uint32_t processing_length; void* processing; };
struct Process { CommandId id; float pitch_ramp, target_pitch; std::uint32_t first_frame, frame_count, input_length;
                 const std::uint8_t* input; std::uint32_t output_length; std::uint8_t* output; };
struct ApuResult { CommandId id; std::uint32_t state; std::uint64_t state_data, usec; HRESULT error;
                   std::uint32_t frames, output_length; std::uint8_t* output; float pitch; std::uint32_t input_length;
                   const std::uint8_t* input; };
#pragma pack(pop)
struct ConnectOutput { void* memory; HANDLE result_event; };

struct Connection {
    std::uint8_t* memory = nullptr;
    HANDLE event = nullptr;
    std::uint32_t options = 0, per_stream = 0;
    std::mutex mu;
    std::vector<std::uint32_t> channels;  // 0: not active
    std::vector<std::uint32_t> queued;
    std::deque<ApuResult> results;
    std::uint32_t commands = 0, dequeued = 0;
};

HRESULT WINAPI XApuConnect(const ConnectInput* input, ConnectOutput* output, Connection** handle) {
    if (!input || !output || !handle) return E_INVALIDARG;
    if (input->streams < 1 || input->streams > 500 || input->commands_per_stream < 1 ||
        input->commands_per_stream > 16 || input->memory == 0)
        return E_INVALIDARG;
    auto* c = new Connection;
    c->memory = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, input->memory, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!c->memory) { delete c; return E_OUTOFMEMORY; }
    if (input->options & kResultEvent) c->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    c->options = input->options;
    c->per_stream = input->commands_per_stream;
    c->channels.assign(input->streams, 0);
    c->queued.assign(input->streams, 0);
    output->memory = c->memory;
    output->result_event = c->event;
    *handle = c;
    std::fprintf(stderr, "[hack] xapu: connected (process %u, %u streams, %u bytes, options 0x%x): silent\n",
                 input->process, input->streams, input->memory, input->options);
    std::fflush(stderr);
    return S_OK;
}

HRESULT WINAPI XApuDisconnect(Connection* c) {
    if (!c) return E_INVALIDARG;
    if (c->event) CloseHandle(c->event);
    VirtualFree(c->memory, 0, MEM_RELEASE);
    delete c;
    return S_OK;
}

HRESULT WINAPI XApuEnqueueCommand(Connection* c, const CommandId* command, std::uint32_t* total) {
    if (!c || !command) return E_INVALIDARG;
    CommandId id;
    std::memcpy(&id, command, sizeof id);
    std::lock_guard lock(c->mu);
    if (id.stream >= c->channels.size()) return kInvalidStream;
    if (c->queued[id.stream] >= c->per_stream) return kQueueFull;
    ApuResult r{};
    r.id = id;
    r.pitch = 1.0f;
    switch (id.kind) {
    case kActivate: {
        Activate a;
        std::memcpy(&a, command, sizeof a);
        c->channels[id.stream] = (a.channels >= 1 && a.channels <= 2) ? a.channels : 2;
        break;
    }
    case kDeactivate: c->channels[id.stream] = 0; break;
    case kReset: case kNotify: break;
    case kProcess: {
        Process p;
        std::memcpy(&p, command, sizeof p);
        r.input = p.input;
        r.input_length = p.input_length;
        r.output = p.output;
        const std::uint32_t ch = c->channels[id.stream] ? c->channels[id.stream] : 2;
        if (!p.output || !p.input || !p.input_length) { r.error = E_INVALIDARG; break; }
        // One 20 ms Opus frame at 48 kHz is 960 frames; the title asks for at most frame_count.
        std::uint32_t frames = p.frame_count ? p.frame_count : 960;
        if (frames > p.output_length / (4 * ch)) frames = p.output_length / (4 * ch);
        std::memset(p.output, 0, std::size_t(frames) * ch * 4);
        r.frames = frames;
        r.output_length = frames * ch * 4;
        break;
    }
    default: return kInvalidCommand;
    }
    c->queued[id.stream] += 1;
    c->commands += 1;
    c->results.push_back(r);
    if (total) *total = c->commands;
    if ((id.options & kSignal) && c->event) SetEvent(c->event);
    return S_OK;
}

HRESULT WINAPI XApuDequeueResult(Connection* c, ApuResult* out) {
    if (!c || !out) return E_INVALIDARG;
    std::lock_guard lock(c->mu);
    if (c->results.empty()) return kPendingResults;
    const ApuResult r = c->results.front();
    c->results.pop_front();
    if (r.id.stream < c->queued.size() && c->queued[r.id.stream]) c->queued[r.id.stream] -= 1;
    c->dequeued += 1;
    std::memcpy(out, &r, sizeof r);
    return S_OK;
}

std::uint32_t WINAPI XApuGetResultCount(Connection* c, std::uint32_t* total) {
    if (!c) return 0;
    std::lock_guard lock(c->mu);
    if (total) *total = c->dequeued;
    return static_cast<std::uint32_t>(c->results.size());
}

// ---- chad.dll -------------------------------------------------------------------------------------------------
#pragma pack(push, 1)
struct StreamDesc { std::uint32_t kind, options, frames, channels, ir_floats; const float* ir; };
#pragma pack(pop)
struct Status { std::uint32_t submitted, completed, reserved; HRESULT result; };
struct DspCommand { float gains[2]; const float* input; float* output; std::uint64_t reserved; };
struct Dsp { std::uint32_t max_streams = 0, active = 0; std::mutex mu; };
struct DspStream { Dsp* dsp; std::uint32_t frames, channels; Status status{}; };

HRESULT WINAPI XDspConnectWithMaximumStreamLimit(void* base, std::uint32_t base_length, std::uint32_t ir_seconds,
                                                 std::uint32_t max_streams, std::uint32_t max_channels, Dsp** out) {
    if (!base || base_length < 0x10000 || base_length > 0x1F400000 || ir_seconds > 0x80 || max_streams > 0x100 ||
        max_channels > 2)
        return E_INVALIDARG;
    if (!out) return E_POINTER;
    auto* d = new Dsp;
    d->max_streams = max_streams;
    *out = d;
    std::fprintf(stderr, "[hack] chad: DSP connected (%u streams): outputs are silence\n", max_streams);
    std::fflush(stderr);
    return S_OK;
}

HRESULT WINAPI XDspDisconnect(Dsp* d) {
    if (!d) return E_INVALIDARG;
    delete d;
    return S_OK;
}

HRESULT WINAPI XDspActivate(Dsp* d, const StreamDesc* desc, const Status** status, DspStream** stream) {
    if (!status || !stream) return E_POINTER;
    if (!d || !desc) return E_INVALIDARG;
    StreamDesc sd;
    std::memcpy(&sd, desc, sizeof sd);
    {
        std::lock_guard lock(d->mu);
        if (d->active >= d->max_streams) return E_OUTOFMEMORY;
        d->active += 1;
    }
    auto* s = new DspStream{d, sd.frames, sd.channels ? sd.channels : 1};
    *status = &s->status;
    *stream = s;
    return S_OK;
}

HRESULT WINAPI XDspDeactivate(DspStream* s) {
    if (!s) return E_INVALIDARG;
    {
        std::lock_guard lock(s->dsp->mu);
        if (s->dsp->active) s->dsp->active -= 1;
    }
    delete s;
    return S_OK;
}

HRESULT WINAPI XDspSubmitCommand(DspStream* s, const DspCommand* command, std::uint32_t* ticket) {
    if (!s || !command) return E_INVALIDARG;
    if (command->output) std::memset(command->output, 0, std::size_t(s->frames) * s->channels * sizeof(float));
    const std::uint32_t next = s->status.submitted + 1;
    *reinterpret_cast<volatile std::uint32_t*>(&s->status.submitted) = next;
    *reinterpret_cast<volatile HRESULT*>(&s->status.result) = S_OK;
    *reinterpret_cast<volatile std::uint32_t*>(&s->status.completed) = next;
    if (ticket) *ticket = next;
    return S_OK;
}

}  // namespace

SHIM_TABLE(audio_x,
    {"xapu.dll", "XApuConnect", reinterpret_cast<void*>(&XApuConnect)},
    {"xapu.dll", "XApuDisconnect", reinterpret_cast<void*>(&XApuDisconnect)},
    {"xapu.dll", "XApuEnqueueCommand", reinterpret_cast<void*>(&XApuEnqueueCommand)},
    {"xapu.dll", "XApuDequeueResult", reinterpret_cast<void*>(&XApuDequeueResult)},
    {"xapu.dll", "XApuGetResultCount", reinterpret_cast<void*>(&XApuGetResultCount)},
    {"chad.dll", "XDspConnectWithMaximumStreamLimit", reinterpret_cast<void*>(&XDspConnectWithMaximumStreamLimit)},
    {"chad.dll", "XDspDisconnect", reinterpret_cast<void*>(&XDspDisconnect)},
    {"chad.dll", "XDspActivate", reinterpret_cast<void*>(&XDspActivate)},
    {"chad.dll", "XDspDeactivate", reinterpret_cast<void*>(&XDspDeactivate)},
    {"chad.dll", "XDspSubmitCommand", reinterpret_cast<void*>(&XDspSubmitCommand)},
);
