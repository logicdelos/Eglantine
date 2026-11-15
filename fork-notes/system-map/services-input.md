# Input and HID services

**Purpose.** This slice turns host input (SDL gamepads, keyboard, mouse, Joy-Con over HID, GameCube adapter, Cemuhook UDP, TAS, virtual amiibo) into the guest-visible HID shared memory, and serves the `hid`, `hid:sys`, `irs`, `hidbus`, `nfc`/`nfp`, `mii` and Bluetooth (`btdrv`/`btm`) services. Host drivers (`input_common`) feed an `InputEngine`; `hid_core` maps engine values to `EmulatedController` state; `ResourceManager` copies that state into per-applet shared memory on core-timing ticks; the HLE services in `core/hle/service/hid` hand out handles and settings to the guest.

## Files

| file | role | read depth |
|---|---|---|
| `src/hid_core/resource_manager.cpp`, `hid_core.cpp` | owns the HID resources and schedules the 5 update events; owns `EmulatedController` objects | full / partial |
| `src/hid_core/resources/applet_resource.cpp` | aruid slots, shared memory per aruid | full |
| `src/hid_core/resources/npad/npad.cpp`, `npad_resource.cpp` | npad state machine, LIFO writes, connect/disconnect | mostly full |
| `src/hid_core/resources/abstracted_pad/*`, `npad_vibration.cpp` | per-pad handlers (vibration, battery, sixaxis, nfc) | partial |
| `src/hid_core/frontend/emulated_controller.cpp/.h` | button/stick/motion mapping, callbacks, injector | mostly full |
| `src/input_common/input_engine.cpp`, `input_poller.cpp`, `main.cpp` | engine base class, poller devices, subsystem | full / partial / partial |
| `src/input_common/drivers/sdl_driver.cpp` | SDL3 gamepads | most |
| `src/input_common/drivers/joycon.cpp`, `helpers/joycon_driver.cpp`, `joycon_protocol/poller.cpp` | native Joy-Con/Pro driver, one thread per device | most / most / partial |
| `drivers/gc_adapter.cpp` | GameCube adapter via libusb (`ENABLE_LIBUSB`) | partial |
| `drivers/udp_client.cpp` | Cemuhook UDP client | mostly |
| `drivers/mouse.cpp`, `keyboard.cpp`, `tas_input.cpp`, `virtual_amiibo.cpp`, `camera.cpp`, `stick_from_buttons.cpp` | other engines | skimmed / skimmed / not opened |
| `drivers/android.cpp` | Android only | not opened (platform note) |
| `src/core/hle/service/hid/hid_server.cpp` | `hid` IPC | partial (applet resource, vibration) |
| `hid_system_server.cpp`, `irs.cpp`, `hidbus.cpp`, `nfc/`, `nfp/`, `mii/`, `btdrv/`, `btm/` | other services | not opened |
| `hid_core/resources/touch_screen`, `six_axis`, `irsensor`, `hidbus/ringcon.cpp`, `emulated_devices.cpp`, `emulated_console.cpp` | | not opened |

NX2 note: no `nx2` symbol exists in this slice. The fork additions seen are `EmulatedController::SetInputInjector` / `Injected()` (debug-server input, `docs/nx2/debug-mcp.md`) and a few `LOG_INFO` probes in `hid_server.cpp` (command 213, `SendVibrationValuesInMode`).

## Main types and ownership

- `ResourceManager` owns `AppletResource`, `NPad`, `DebugPad`, `Mouse`, `Keyboard`, `TouchResource`, six-axis resources, `input_event` (`KEvent`) and the core-timing events. `IHidServer` and `IAppletResource` hold `shared_ptr<ResourceManager>`.
- `HIDCore` owns the `EmulatedController` array (one per npad id), the emulated console and devices.
- `EmulatedController` owns `InputDevice` objects created by the poller factories (`button_devices`, `stick_devices`, `motion_devices`, `output_devices[]`) and a callback list for `NPad`.
- `InputSubsystem` owns one engine per driver. Each `InputEngine` owns `controller_list` (pad state) and `callback_list`; poller devices register in it and deregister in their destructor.
- `AppletResource` holds `data[AruidIndexMax]`, `registration_list` and one shared-memory block per slot. `NPad` keeps raw `shared_memory` pointers into those blocks.
- `SDLDriver` owns `joystick_map` (guid to vector of `SDLJoystick`); `Joycons` owns `JoyconDriver`s, each with an `std::jthread`.

## Threads and locking

- **Qt UI thread:** `update_input_timer` fires every 1 ms (`main_window.cpp:344,527`) and calls `InputSubsystem::PumpEvents`, which pumps SDL. SDL event callbacks therefore run on the UI thread, and so do `SetButton`, `SetStick`, `SetMotion`, `TriggerOnChange` and `NPad::ControllerUpdate`.
- **Core-timing thread:** `NPad::OnUpdate` (1 ms; the code comment says real hardware is 4 ms), default controllers (4 ms), mouse/keyboard (8 ms), motion (5 ms), touch.
- **Driver threads:** Joy-Con `ScanThread` (5 s) and one `InputThread` per device (`SDL_hid_read_timeout` 3 ms + `yield`); GC adapter scan (2 s) and input (16 ms) threads; one `cemuhookWorker` per UDP client (boost asio `io_context`).
- **Locks:** `ResourceManager::shared_mutex` (recursive) guards shared-memory writes. `NPad::mutex` is taken before `shared_mutex` on the paths read. `InputEngine::mutex` guards `controller_list`; `mutex_callback` guards callbacks and is held while poller callbacks run. `EmulatedController` has only `callback_mutex` and `injector_mutex`; state is plain structs plus a few atomics. `SDLDriver::joystick_map_mutex` is held in `InitJoystick` while engine callbacks fire.
- No lock-order inversion was found between `NPad::mutex` and `shared_mutex`. `NPad::ControllerUpdate` takes neither (finding 2).

## Flow

1. SDL event: `SDLDriver::HandleGameControllerEvent` -> `InputEngine::SetButton/SetAxis` -> `TriggerOnButtonChange` (takes `mutex_callback`) -> poller `OnChange` -> `EmulatedController::SetButton` (updates `controller.button_values`, calls `Connect()` if the player is connected, `TriggerOnChange`) -> `NPad::ControllerUpdate`.
2. Every 1 ms on the core thread: `NPad::OnUpdate` takes `shared_mutex`, for each active aruid and connected pad runs `RequestPadStateUpdate` (reads `GetNpadButtons`, `GetSticks`, applies the injector), and writes the LIFOs in shared memory. Motion events run `StatusUpdate`, which calls `ForceUpdate` on devices that need refresh.
3. Guest side: `IHidServer::CreateAppletResource` -> `ResourceManager::CreateAppletResource` -> `AppletResource::CreateAppletResource`. Guest reads shared memory directly; HLE calls (`SetSupportedNpadStyleSet`, `SendVibrationValue`, ...) go to `NPad` / the abstracted pads.
4. Vibration back path: `NpadVibrationDevice` -> `EmulatedController::SetVibration` -> `output_devices[i]` -> SDL rumble or Joy-Con `vibration_queue`.

## Connections

- In: Qt frontend (`UpdateInputDrivers`, `ReloadInputDevices`, config dialogs via `InputSubsystem::GetInputDevices` and mapping callbacks), debug server (`SetInputInjector`), applet/am code (`RegisterAppletResourceUserId`), guest via HIPC.
- Out: `Core::Timing` (looping events), `Kernel::KernelCore` (`KEvent`, shared memory, `KTransferMemory`), `Service::Set` (vibration settings), `Settings::values`, SDL3, libusb (GC adapter), boost asio (UDP), nfc/amiibo files.

## Settings and switches

`Settings::values` keys: `enable_joycon_driver` (default true), `enable_procon_driver` (default false), `enable_udp_controller`, `udp_input_servers` (default `127.0.0.1:26760`), `touch_device`, `players_N`, `motion_enabled`, `vibration_enabled`, `enable_raw_input`, `tas_*`, `mouse_enabled`, `keyboard_enabled`. No environment variables are read in `input_common` or `hid_core` (`getenv` grep is empty). Build switch: `ENABLE_LIBUSB` for the GC adapter.

## Invariants

- Every pad id used in `InputEngine::Set*` must have been added by `PreSetController`; enforced only by `controller_list.at()` throwing. Drivers pre-set on discovery and never remove entries.
- Writers of shared memory hold `shared_mutex`: enforced in `OnUpdate` and most HLE setters, not enforced in `ControllerUpdate` or `UnregisterAppletResourceUserId` paths from the UI thread.
- `NPad::controller_data[aruid][npad].shared_memory` is non-null only while the aruid is registered; code checks for null in `InitNewlyAddedController` and `ControllerUpdate`, but not in `GetSixaxisProperties`.
- Unknown aruids fall back to slot 0 in `NPad::GetControllerFromNpadIdType`, `Unregister…`: assumed to be harmless (it is not in `Unregister`).
- Joy-Con callbacks must be set before the input thread produces data: assumed, not enforced.

## Hazards and improvement candidates

Details are in `findings/services-input.json`.
- `NPad::ControllerUpdate` runs on the UI thread without locks and races `OnUpdate`; it also uses `return` where `continue` is meant.
- `JoyconDriver::InitializeDevice` starts the input thread before `Joycons::Setup` calls `SetCallbacks`; an early report calls an empty `std::function`.
- `NPad::UnregisterAppletResourceUserId` acts on slot 0 for an unknown aruid.
- SDL gamepad handle leaked when the Joy-Con/Pro-Controller driver is preferred.
- `GetActualVibrationGcErmCommand` does not return after the inactive-aruid case.
- `EmulatedController` motion/colour/battery state is read and written from several threads without a lock.
- `ResetButtonState` / `ResetAnalogState` iterate `controller_list` unlocked; `SDLDriver::GetInputDevices` iterates `joystick_map` unlocked.
- `IsVibrationEnabled` sleeps 15 ms per joystick on the calling thread.
- Not reviewed: hid_system_server, irs, hidbus, nfc, nfp, mii, btdrv, btm, TAS and keyboard drivers.
