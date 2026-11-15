# Audio core and audio services

**Purpose.** `audio_core` emulates the Switch audio DSP: a command-list renderer (`audren:u`), simple PCM output and input (`audout:u`, `audin:u`), hardware Opus decoding (`hwopus`) and the host sinks (cubeb or SDL3 on Windows). The HLE services in `core/hle/service/audio` expose these to the guest. The fork adds Switch 2 entry points: `aud:u`, `auddmg:u` and `audsmx:u` (sysmix), plus an Opus "hwacod" client under `hwopus` cmd 100. Everything is paced by the host sink and a 5 ms CoreTiming tick; there is no separate audio clock.

## Files

| File | Role | Read depth |
|---|---|---|
| `audio_core/sink/sink_stream.cpp/.h` | queue, sample ring, volume, downmix, `WaitFreeSpace`, `ProcessAudioOutAndRender` | full |
| `audio_core/sink/cubeb_sink.cpp`, `sdl3_sink.cpp`, `sink_details.cpp`, `null_sink.h`, `sink.h` | host backends and Auto selection | full |
| `common/ring_buffer.h`, `common/bounded_threadsafe_queue.h` | sample ring and buffer queue | full |
| `audio_core/device/device_session.cpp/.h`, `audio_buffers.h` | per-session sink stream, 5 ms tick, buffer ring | full |
| `audio_core/out/*`, `audio_out_manager.cpp`, `audio_in_manager.cpp`, `audio_manager.cpp`, `audio_event.*`, `audio_core.cpp` | audout sessions, manager thread | full |
| `audio_core/adsp/adsp.cpp`, `mailbox.h`, `apps/audio_renderer/*` | ADSP thread, command list processor | full |
| `audio_core/renderer/system_manager.cpp`, `audio_renderer.cpp`, `system.cpp` | host render thread, `Update`, command generation entry | full / full / partial (lines 400-831) |
| `audio_core/renderer/command/sink/device.cpp` | final write into the sink | full |
| `audio_core/opus/hardware_opus.cpp`, `adsp/apps/opus/opus_decoder.cpp` | Opus mailbox and thread | full |
| `core/hle/service/audio/audio.cpp`, `audio_out.cpp`, `audio_renderer.cpp`, `audio_renderer_manager.cpp`, `audio_controller.cpp`, `hardware_opus_decoder_manager.cpp` | HLE front ends | full / partial |
| `nx2_audio_device_manager.h` (653 lines), `nx2_audio_user_client.h`, `nx2_hwacod.h` | fork code: `auddmg:u`, `audsmx:u`, `aud:u`, hwacod | full |
| `renderer/command/*` (except `sink/device.cpp`), `command_processing_time_estimator.cpp` (3620 lines), `resample.cpp`, `effect/*`, `voice/*`, `mix/*`, `splitter/*`, `behavior/*`, `performance/*`, `in/audio_in*`, `opus/decoder*` | DSP maths, parsing, estimators | skimmed or not opened |

## Main types and ownership

- `AudioCore` owns two sinks (`output_sink`, `input_sink`, both from `Settings::values.sink_id`), the `AudioManager` and the `ADSP`. `CreateSinks` runs before the ADSP is built.
- `ADSP::AudioRenderer` (one per process) owns the mailbox, two command-buffer slots (`MaxRendererSessions = 2`), two sink streams (`SetRingSize(4)`) and the ADSP thread.
- `Renderer::Manager` owns a `SystemManager`, which owns the host render thread and a raw-pointer list of `Renderer::System`. Each `IAudioRenderer` owns a `Renderer::Renderer` that owns one `System`. `System` owns its work buffer carved from guest transfer memory.
- `AudioOut::Manager` owns up to 12 `Out` sessions. `Out` owns an `AudioBuffers<32>` ring and a `DeviceSession`. `DeviceSession` owns a sink stream handle and a looping CoreTiming event.
- `SinkStream` owns an `SPSCQueue<SinkBuffer, 0x10000>`, a mutex-based `RingBuffer<s16, 0x10000>`, `playing_buffer` and `last_frame`.
- Sysmix: a function-local static `Nx2SysmixStreams` (mutex + `map<u32, shared_ptr<Stream>>`), shared by every `audsmx:u` session. Each stream owns a sink stream acquired from the output sink.

## Threads and locking

- **Service/HLE thread**: all IPC. Takes the manager's `std::recursive_mutex`, then `AudioBuffers::lock` (recursive).
- **AudioRenderSystemManager** (High priority): holds `SystemManager::mutex1` while it calls `System::SendCommandToDsp` (takes `System::lock`), then `Signal()` and `Wait()` on the ADSP mailbox with no timeout.
- **DSP_AudioRenderer_Main** (High priority, performance cores): receives `Render`, calls `SinkStream::WaitFreeSpace` for the first session with a command buffer (session 0 only before `be1fd16`), runs `CommandListProcessor::Process`, appends to the sink stream.
- **Host audio callback** (cubeb or SDL thread): `SinkStream::ProcessAudioOutAndRender` pops the queue under `release_mutex`, pops samples under the ring's own mutex, and `notify_one`s `release_cv`.
- **AudioManager** thread: waits up to 2 s on an event, runs the out/in `BufferReleaseAndRegister` callbacks while holding the event lock. `ClearManager` takes `{manager lock, event lock}`.
- **CoreTiming thread**: `DeviceSession::ThreadFunc` every 5 ms (sets `played_sample_count`, sets the AudioManager event) and the sysmix `Drain` every 5 ms (takes the stream-list mutex, reads and writes guest memory).
- **Opus ADSP thread**: `HardwareOpus::mutex` serialises host requests; reply is awaited with no timeout.
- Lock order seen in code: event lock, then manager mutex, then `AudioBuffers::lock`; `System::lock` and `mutex1` are not nested with those. `playing_buffer`/`last_frame` are touched by the callback outside `release_mutex` (only `ClearQueue` takes it).

## Flow

1. Guest opens `audren:u` (`IAudioRendererManager::OpenAudioRenderer`), creating `Renderer::Renderer` -> `Manager::AddSystem` -> `SystemManager::Add` -> first system triggers `InitializeUnsafe` -> `AudioRenderer::Start` (`CreateSinkStreams`, mailbox init, ADSP thread).
2. Guest `RequestUpdate` -> `System::Update` parses behaviour, memory pools, voices, mixes, effects, sinks; `GenerateCommand` fills the command workbuffer.
3. Host thread loop: `SendCommandToDsp` publishes buffer pointer and size when the previous list finished, else re-sends the old one; `Signal()`; `Wait()`; `PostDSPClearCommandBuffer`.
4. ADSP `Main` handles `Render`: per session `WaitFreeSpace` (first session with commands: before `Process` for session 0, after it for another) -> `Process` -> sets `remaining_command_count`, `render_time_taken_us` -> reply `RenderResponse`.
5. `DeviceSinkCommand::Process` builds a `SinkBuffer` and calls `SinkStream::AppendBuffer`: volume = `system_volume * device_volume * Settings::Volume()`, channel conversion, `queue.EmplaceWait`, `samples_buffer.Push`, `++queued_buffers`; `Start()` if paused.
6. Host callback `ProcessAudioOutAndRender`: pop buffer, copy samples, on empty queue repeat `last_frame`, `--queued_buffers`, notify `release_cv`.
7. audout: `Out::AppendBuffer` -> `AudioBuffers` -> `ReleaseAndRegisterBuffers` (manager thread) -> `DeviceSession::AppendBuffers` copies guest PCM and calls `AppendBuffer`; released when `played_sample_count >= end_timestamp`, where the count is `min(real elapsed*48000, frames written) + 1200`.
8. Sysmix: guest writes PCM into its own ring; `Drain` takes `5 ms * rate` frames from each engaged ring, zero-pads on underrun, appends to the sink, advances the read index in guest memory, and signals the data event when free frames reach the threshold. If the sink queue holds 8 or more buffers the frames owed for that tick are dropped and the ring is left alone.
9. hwacod `Execute` decodes Opus with libopus on the service thread, writing PCM and completion records straight into guest memory.

## Connections

In: guest IPC (`audout:u`, `audin:u`, `audren:u`, `audctl`, `hwopus`, `aud:u`, `auddmg:u`, `audsmx:u`); `Core::System` startup/shutdown (`IsShuttingDown`, `IsPaused`); the main emulation shutdown path.
Out: `CoreTiming` (`ScheduleLoopingEvent`, `GetGlobalTimeNs/Us`); kernel events (`KEvent::Signal`); guest memory (`CpuGuestMemory`, `ApplicationMemory().ReadBlock/Write32`); `Settings::values`; `Set::ISystemSettingsServer` (audctl); cubeb and SDL3 libraries; libopus.

## Settings and switches

`Settings::values.sink_id`, `audio_output_device_id`, `audio_input_device_id`, `volume` (0-200, default 100), `audio_muted`, `dump_audio_commands`. `Settings::Volume()` returns `volume/100`, 0 if muted, and above 1.0 becomes `0.6 + 20*log10(v)`. Env var `EDEN_NX2_AUDIO_LEVEL` enables peak logging every 2 s of output. `sink_id = Auto` picks cubeb if its minimum latency is at most 720 frames, else SDL3.

## Invariants

- Frame quantum is 240 samples at 48 kHz (5 ms); enforced by constants in `common/common.h`, assumed in the sink and in the +1200-sample leeway.
- Sample ring and buffer queue stay in step (one ring slice per queue entry); only assumed, see the findings.
- Sink stream `queued_buffers` equals queue length; enforced by increments and decrements, but the increment is outside `release_mutex`.
- Session ids 0/1 are unique per `Renderer::Manager`; assumed that only one manager exists (not true for `aud:u`).
- `ADSP::AudioRenderer::Start` is called once per running period; only assumed.
- Guest ring and IO bounds in sysmix and hwacod are checked at open/execute time (`OpenStream`, `Execute` layout checks); the guest memory is not re-validated on each `Drain`.

## Hazards and improvement candidates

See `findings/services-audio.json`. Summary:

- Audio-out restart: `played_sample_count` regresses while `end_timestamp` keeps growing (buffers stop being released).
- `aud:u` creates a new audren and audout manager per call (hang in `AudioRenderer::Start`, lost release callbacks).
- Render manager thread had no pacing when no system is active, session 0 is gone, or the Null sink is selected. `fd1acac` added a 5 ms sleep per pass for the first case. `be1fd16` covers all three: the ADSP waits on the first session with commands, and the manager waits to a 5 ms steady-clock deadline after any pass that no started stream held back (`AudioRenderer::LastRenderWasPaced`). Still unpaced: a started stream whose device has stalled (`SinkStream::stalled`).
- `AppendBuffer` enqueues before it pushes samples, and the callback ignores the pop count.
- `WaitFreeSpace` has an unbounded wait; shutdown deadlocks if the sink stops.
- `Renderer::Initialize` swallows `System::Initialize` errors; `SystemManager::Add` races with the render thread.
- Volume is applied through system, device and UI paths at once.
- `SetTargetMute` muted the output whichever target it named. Since `fd1acac` only the active target does. Nothing assigns `m_active_target`, so that is always the speaker.
- The null sink handed one shared stream, with one sample counter, to every caller. Since `fd1acac` each call gets its own.
- Above 100 % the volume curve began at 0.6, so 101 % was quieter than 100 %. Since `fd1acac` it begins at 1.0.
- Not filed (low value or unconfirmed): the DSP time limit is never enforced (`max_process_time` is stored but unused, and `render_times_taken` underflows); an invalid command magic leaves `remaining_command_count` non-zero forever and the same list is re-sent; `AppendBuffer` for audio-in returns without queuing (stale microphone data up to 1.4 s in the ring); `HardwareOpus::GetWorkBufferSizeForMultiStream` skips the `IsRunning` check; ADSP Opus code uses `ASSERT` on guest-supplied channel counts; sysmix `Drain` and the SDL callback allocate vectors every tick; `TaskSize` in hwacod uses a 16-byte stride while `Execute` uses 64-byte completions (protocol guess).
