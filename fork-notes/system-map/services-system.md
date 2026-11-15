# HLE service framework and system services (src/core/hle/service, internal_network, network)

**Purpose.** This slice holds the machinery that turns a guest IPC request into a call on a C++ service object, plus the roughly 50 service groups that sit on it. It also holds the host networking layer used by the `bsd` socket services, and the multiplayer room code in `src/network`. The core is `ServerManager` (waits on ports and sessions and dispatches requests), `ServiceFrameworkBase` (command table, per-object lock, fork auto-stub), `SessionRequestManager` (per-session state, domains) and `sm`. The service groups themselves are mostly thin: they answer queries, stub out commands, and in a few places (bsd, nifm, ldn, am) do real host work.

## Files

| file | role | read depth |
|---|---|---|
| `server_manager.cpp/.h` | wait loop, port accept, session dispatch, deferral, extra threads | full |
| `service.cpp/.h` | `ServiceFrameworkBase::HandleSyncRequest`, control requests, fork auto-stub and IPC trace | full |
| `hle_ipc.cpp/.h` | `HLERequestContext`, `SessionRequestManager` (domains), command buffer parse/write | full |
| `ipc_helpers.h` | `ResponseBuilder` / `RequestParser`, `PushIpcInterface` | partial (builder only) |
| `services.cpp` | `Services::Services` starts every service thread | full |
| `sm/sm.cpp` | service manager, named ports, Switch 2 stub sessions | full |
| `sockets/sockets.cpp` | bsd/nsd/sfdnsres registration, extra bsd threads | full |
| `sockets/bsd.cpp` (2944 lines) | BSD sockets, select/poll notifier, eventfd, proxy sockets | partial (about 70 %: handlers, select, eventfd, deferred paths) |
| `sockets/sfdnsres.cpp` | DNS (getaddrinfo, gethostbyname) | partial |
| `internal_network/network.cpp` | host socket wrapper (WSAPoll, interrupt socket) | partial (725-1200) |
| `nifm/nifm.cpp` | network interface manager; scan worker | partial |
| `am/applet_manager.cpp`, `am/am.cpp`, `am/event_observer.cpp` | applet startup, window system | partial |
| `glue/time/worker.cpp`, `nim/nim.cpp`, `ldn/lan_discovery.cpp` | worker threads, sleeps | partial |
| `acc/*`, `set/*`, `ns/*`, `ssl/*`, `friend/*`, `psc/*`, `pctl/*`, `bcat/*`, `mii/*`, `ldn/*` (rest), `nfc/*`, `nfp/*`, `am/frontend/*`, `am/service/*`, about 300 small files | per-service handlers | skimmed or not opened |
| `src/network/room*.cpp`, `room_member.cpp`, `announce_*`, `web_service/*` | multiplayer room (host and client), thread per room | not opened except thread names by grep |
| `nx2_*` service headers (for example `nx2_gsv.h`) | fork Switch 2 stubs | not opened (belongs to the GPU/service-gfx slices) |

I did not trace individual handlers in acc, set, ns, ssl, friend, psc or the applet frontends. Statements about those groups are from names, registration code and grep.

## Main types and ownership

- `Services` (owned by `System`) only starts threads; it keeps no list of them.
- `ServerManager`: one per service module. It owns its `Port`s, `Session`s (a session wraps a `KServerSession` and a `SessionRequestManager`), a `MultiWait` and a wakeup event. It is created in the module's `LoopProcess`, then `ServerManager::RunServer` hands it to `KernelCore::RunServer`, which stores it in `server_managers` and runs the loop on the calling thread.
- `ServiceFrameworkBase`/`ServiceFramework<T>`: one object per interface. Handlers are kept in a command-id table. The object is held by `shared_ptr` in a `SessionRequestManager` (plain session) or in `domain_handlers` (domain). `AddMoveInterface` / `PushIpcInterface` create a new KSession and a new `SessionRequestManager` for each sub-interface.
- `HLERequestContext`: created per request in `KServerSession::ReceiveRequestHLE`, shared by `shared_ptr` while a deferral holds it, reset after the reply.
- `ServiceManager` (`sm`): `service_ports` map (name to `KClientPort`), `registered_services` and the factory functions. Its destructor closes the remaining ports.
- bsd: `BSD_USA` singletons for `bsd:u`, `bsd:s`, `bsd:a` share one fd table (`file_descriptors`), one `SelectNotifier` and one eventfd state.

## Threads and locking

There are two kinds of service thread, and the difference matters.

1. **Host threads** (`kernel.RunOnHostCoreProcess(name, f).detach()` in `services.cpp`): `audio`, `FS`, `ldn`, `nvservices`, `bsdsocket`, `vi`. Each is a real host `jthread` that never goes through the emulated scheduler. A blocking call there stalls only that service.
2. **Guest-core kernel threads** (`KernelCore::RunOnGuestCoreProcess`): about 50 services (sm, account, am, apm, settings, hid, nifm, nim, ns, ssl, friends, psc, glue, pctl, and so on). Each gets a KProcess and a KThread (`InitializeServiceThread`, priority 16, core 3). They are scheduled by the emulated scheduler on guest core 3, which is one host thread. **Any host-blocking call (sleep, mutex wait, condition variable, socket call, thread join) in one of these handlers stops all guest threads on core 3**, including the other services.

Per `ServerManager`: one thread runs `LoopProcess`. `StartAdditionalHostThreads(name, n)` adds n `jthread`s running the same loop; only bsd uses it (`sockets.cpp:92`, n = 2, so 3 server threads). They share one `MultiWait`; `WaitSignaled` serialises waiting under `m_selection_mutex`, so one thread waits and the others handle requests. `bsdsocket:select` is a further host thread (`SelectNotifier`).

Locks:
- `m_deferred_list_mutex` guards the list of holders waiting to be linked into the MultiWait.
- `m_deferral_epoch` (atomic) and `m_deferral_holder` handle the retry-versus-new-defer race.
- `ServiceFrameworkBase::LockService()` returns a per-object `std::mutex` lock held across the whole `HandleSyncRequest`, so one interface object is never entered by two threads. `BSD_USA::LockService` returns an empty lock; bsd handlers are concurrent and rely on `file_descriptors_mutex`.
- bsd lock order, as documented in the code: `SelectNotifier::mutex`, then `file_descriptors_mutex`, then an eventfd's or room socket's own mutex.
- The room thread calls `OnProxyPacketReceived` into bsd; it takes `file_descriptors_mutex` and `proxy_binding_mutex`.
- Static mutexes in nifm (`g_scan_mtx`, `g_profile_mtx`) guard scan results and the pending profile.

## Flow

Guest `svcSendSyncRequest` reaches the service thread like this:

1. `KServerSession::OnRequest` signals the session; the `MultiWait` in `ServerManager::WaitSignaled` wakes. `LinkDeferred` has first moved newly deferred/relinked holders in.
2. `ServerManager::Process` switches on `UserDataTag` (Port, Session, DeferEvent).
3. Port: `OnPortEvent` calls `AcceptSession`, builds a `SessionRequestManager` from the port's handler factory, `RegisterSession`, and relinks the port.
4. Session: `OnSessionEvent` calls `KServerSession::ReceiveRequestHLE` (builds `HLERequestContext` from the TLS command buffer), then `CompleteSyncRequest`.
5. `SessionRequestManager::CompleteSyncRequest` picks `HandleDomainSyncRequest` (object id selects the handler in `domain_handlers`) or the plain session handler.
6. `ServiceFrameworkBase::HandleSyncRequest` takes `LockService()` and switches on `ctx.GetCommandType()`: Close, Control (`InvokeControlRequest`: ConvertToDomain, QueryPointerBufferSize, CloneCurrentObject), Request (`InvokeRequest` finds the `FunctionInfo` and calls the handler) or TIPC. Unknown commands go to `ReportUnimplementedFunction`, which can auto-stub (fork).
7. The handler writes into the command buffer (`ResponseBuilder`); `WriteToOutgoingCommandBuffer` converts moved objects into handles or domain ids.
8. `KServerSession::SendReplyHLE` (fork: `HoldHleReply` may spin up to `EDEN_NX2_IPC_MIN_US` microseconds before waking the client) wakes the guest client thread. The session is then relinked, or destroyed if the client closed.

Deferral: a handler calls `ctx.SetIsDeferred()`. The session is parked in `m_deferred_sessions` and not waited on. A deferral event (signalled by the service's own helper thread, or by a client-close event) makes `OnDeferralEvent` retry each parked request. `RetireDeferredRequest` cancels via `m_deferral_cancel_handler`. Used by `sm` (GetService before registration) and by bsd (select, poll, blocking eventfd, recvmmsg/sendmmsg). `ManageDeferral` may be called once per manager.

Service lookup: `sm:GetServiceImpl` returns a session from `service_ports`; if the name is not registered it defers, except for Switch 2 callers (`IsSwitch2()`), which get a stub session (`INx2StreamPlayService` for `stpl:u`, `INx2StubInterface` for `nx2:<name>`).

bsd: `Recv`, `Send`, `Accept`, `Connect` call `ExecuteWork` which runs the host socket call inline on a bsd server thread. Select/Poll build a `SelectRequest`, defer, and `SelectNotifier::Run` polls (WSAPoll) over all pending sockets plus a wake and an interrupt socket, then signals the deferral event.

## Connections

- In: every service module calls `ServerManager::RegisterNamedService` / `RunServer`; the kernel calls `Services::Services` from `System::Initialize`; `AppletManager` is called from `System`/frontends (`CreateAndInsertByFrontendAppletParameters`, `OperationModeChanged`).
- Out to kernel: `KernelCore::RunOnHostCoreProcess`, `RunOnGuestCoreProcess`, `RunServer`, `KServerSession::ReceiveRequestHLE/SendReplyHLE`, `KSession::Create`, `KEvent`, `KHandleTable::Add`, resource-limit `Reserve` for SessionCountMax (system limit is 1133).
- Out to other subsystems: `Network::` host sockets (WSAPoll, getaddrinfo), `Network::GetRoomMember` (room proxy packets in bsd), `Network::ScanWifiNetworks` (nifm), applets to frontends via `Frontend::*`, `Core::Trace::g_ipc_hook`.

## Settings and switches

- `Settings::values.use_auto_stub`: stub unknown commands with zeroed replies.
- `Settings::values.airplane_mode`: bsd refuses connection-based sockets.
- `Settings::values.use_debug_asserts`: decides whether `ASSERT` (see below) breaks into the debugger.
- `Settings::values.enable_overlay`: AM launches the overlay applet before the application.
- Env `EDEN_NX2_AUTO_STUB` (force auto-stub), `EDEN_NX2_IPC_TRACE` (log each dispatched command and non-zero result), `EDEN_NX2_IPC_SUMMARY` (5 s summary), `EDEN_NX2_IPC_MIN_US` (reply spin, default 50, read in `k_server_session.cpp`).
- Service names starting with `Nx2StubPrefix` and any process where `IsSwitch2()` is true also auto-stub.

## Invariants

- `ASSERT` is soft: `AssertFailSoftImpl` logs and continues unless `use_debug_asserts` is on. Code that says `ASSERT(...)` or `ASSERT(false)` on a guest-controlled condition (SFCI magic, object id range, SessionCountMax reserve) therefore continues with bad state in a normal run. Enforced in `common/assert.h`.
- One thread at a time runs inside a given interface object: enforced by `LockService()` except for bsd, which opts out.
- `ServerManager::ManageDeferral` once per manager and `SetDeferralCancelHandler` before extra threads start: enforced by assert only.
- Lock order in bsd (above): only documented in comments.
- A deferred request's `HLERequestContext` stays valid until retried: enforced by `shared_ptr` and the epoch counter.
- Domain object ids are 1-based indexes into `domain_handlers`: range check misses id 0 (finding).
- `UnregisterService` and the destructor both own `KClientPort` closing: only the destructor does it.

## Hazards and improvement candidates

Details are in `findings/services-system.json`.
- bsd has 3 server threads and the plain recv/accept/connect paths block on the host, while select/poll/mmsg are deferred.
- bsd Recv/Send handlers pass guest flags through unchecked to host sockets that only support MSG_DONTWAIT; MSG_PEEK is silently dropped.
- nifm `SetNetworkProfile` copies a guest-controlled SSID length into a 33-byte stack buffer; `IScanRequest` closes events before joining its worker.
- Domain object id 0 throws on the service thread; domain handler slots are never reused.
- Blocking calls on core 3 service threads: AM's `WindowSystem` constructor waits on a condition variable inside `AppletManager::SetWindowSystem` until the frontend inserts the process (upstream behaviour, but it parks core 3 until then); `TimeWorker` destructor sleeps 16 ms; `ldn` `Scan` sleeps 1 s on its own host thread; `FS` `IStorage` read sleeps as a rate limiter on the FS host thread.
- Auto-stub accounting (`RecordUnimplementedCommand`) formats and scans under a mutex on every unimplemented call; only hot for polled Switch 2 stubs (nx2:camera ~47 calls/s).
- Service threads started with `RunOnHostCoreProcess` are detached and never joined; shutdown relies on `is_shutting_down` and the kernel closing sessions.
