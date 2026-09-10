#include "physical_input.h"
#include "session.h"
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <stddef.h>
#include <wchar.h>

typedef enum { DEVICE_UNKNOWN, DEVICE_PHYSICAL, DEVICE_VIRTUAL } InputDeviceKind;

static volatile LONG physicalInputConfirmed;
static HANDLE inputWakeEvent;
static BOOL inputRegistered;
// Device handles are stable until WM_INPUT_DEVICE_CHANGE. Avoid walking the
// device tree on every event from a high-polling-rate mouse.
static struct { HANDLE handle; InputDeviceKind kind; } inputDevices[32];
static size_t nextInputDevice;

static BOOL StartsWith(const WCHAR *value, const WCHAR *prefix)
{
    return _wcsnicmp(value, prefix, wcslen(prefix)) == 0;
}

static InputDeviceKind ClassifyInputDevice(HANDLE device)
{
    WCHAR path[1024] = {0};
    UINT length = _countof(path);
    // On success pcbSize may still contain the supplied capacity. Use the
    // return value for the copied character count, not that in/out argument.
    const UINT copied = GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, path, &length);
    if (copied == (UINT)-1 || copied == 0 || copied >= _countof(path)) return DEVICE_UNKNOWN;

    WCHAR instance[MAX_DEVICE_ID_LEN] = {0};
    ULONG bytes = sizeof(instance);
    DEVPROPTYPE type = 0;
    if (CM_Get_Device_Interface_PropertyW(path, &DEVPKEY_Device_InstanceId, &type,
            (BYTE *)instance, &bytes, 0) != CR_SUCCESS || type != DEVPROP_TYPE_STRING ||
        bytes < sizeof(WCHAR) || bytes > sizeof(instance) || bytes % sizeof(WCHAR) != 0 ||
        instance[bytes / sizeof(WCHAR) - 1] != L'\0') return DEVICE_UNKNOWN;

    DEVINST node;
    if (CM_Locate_DevNodeW(&node, instance, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
        return DEVICE_UNKNOWN;

    BOOL hardware = FALSE;
    for (unsigned depth = 0; depth < 64; ++depth)
    {
        WCHAR id[MAX_DEVICE_ID_LEN] = {0};
        if (CM_Get_Device_IDW(node, id, _countof(id), 0) != CR_SUCCESS)
            return DEVICE_UNKNOWN;

        // Sunlogin and other remote tools can inject through virtual HID/USB
        // drivers instead of SendInput. Follow the complete ancestry: a HID or
        // USB-looking child of a software/root-enumerated device is not proof
        // that somebody is using the physical PC.
        // Real PCI/USB devices also traverse Windows' ACPI HAL root. It is the
        // system bus root, unlike a root-enumerated virtual keyboard/controller.
        if ((StartsWith(id, L"ROOT\\") && !StartsWith(id, L"ROOT\\ACPI_HAL\\")) ||
            StartsWith(id, L"SWD\\")) return DEVICE_VIRTUAL;
        if (StartsWith(id, L"USB\\") || StartsWith(id, L"ACPI\\") ||
            StartsWith(id, L"BTHENUM\\") || StartsWith(id, L"BTHLEDEVICE\\")) hardware = TRUE;
        if (StartsWith(id, L"HTREE\\ROOT\\")) return hardware ? DEVICE_PHYSICAL : DEVICE_UNKNOWN;

        DEVINST parent;
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS || parent == node)
            return DEVICE_UNKNOWN;
        node = parent;
    }
    return DEVICE_UNKNOWN;
}

static InputDeviceKind GetInputDeviceKind(HANDLE device)
{
    for (size_t i = 0; i < _countof(inputDevices); ++i)
        if (inputDevices[i].handle == device) return inputDevices[i].kind;

    const InputDeviceKind kind = ClassifyInputDevice(device);
    inputDevices[nextInputDevice].handle = device;
    inputDevices[nextInputDevice].kind = kind;
    nextInputDevice = (nextInputDevice + 1) % _countof(inputDevices);
    return kind;
}

void PhysicalInputForgetDevices(void)
{
    ZeroMemory(inputDevices, sizeof(inputDevices));
    nextInputDevice = 0;
}

BOOL PhysicalInputInitialize(HWND window, HANDLE wakeEvent)
{
    inputWakeEvent = wakeEvent;
    InterlockedExchange(&physicalInputConfirmed, FALSE);
    PhysicalInputForgetDevices();
    RAWINPUTDEVICE devices[] = {
        {1, 2, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window}, // Mouse
        {1, 6, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window}, // Keyboard
    };
    inputRegistered = RegisterRawInputDevices(devices, _countof(devices), sizeof(devices[0]));
    return inputRegistered;
}

void PhysicalInputShutdown(void)
{
    if (inputRegistered)
    {
        RAWINPUTDEVICE devices[] = {{1, 2, RIDEV_REMOVE, NULL}, {1, 6, RIDEV_REMOVE, NULL}};
        RegisterRawInputDevices(devices, _countof(devices), sizeof(devices[0]));
    }
    inputRegistered = FALSE;
    inputWakeEvent = NULL;
    InterlockedExchange(&physicalInputConfirmed, FALSE);
    PhysicalInputForgetDevices();
}

BOOL PhysicalInputIsConfirmed(void)
{
    return InterlockedCompareExchange(&physicalInputConfirmed, FALSE, FALSE) != FALSE;
}

void PhysicalInputRequireConfirmation(void)
{
    if (InterlockedExchange(&physicalInputConfirmed, FALSE) && inputWakeEvent)
        SetEvent(inputWakeEvent);
}

void PhysicalInputHandle(HRAWINPUT handle)
{
    if (!inputRegistered) return;
    RAWINPUT input = {0};
    UINT size = sizeof(input);
    const UINT read = GetRawInputData(handle, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER));
    if (read == (UINT)-1 || read < sizeof(RAWINPUTHEADER) || input.header.dwSize != read ||
        input.header.hDevice == NULL) return;

    if (input.header.dwType == RIM_TYPEKEYBOARD)
    {
        if (read < offsetof(RAWINPUT, data) + sizeof(RAWKEYBOARD) ||
            input.data.keyboard.VKey == 255 || (input.data.keyboard.Flags & RI_KEY_BREAK)) return;
    }
    else if (input.header.dwType == RIM_TYPEMOUSE)
    {
        if (read < offsetof(RAWINPUT, data) + sizeof(RAWMOUSE) ||
            (!input.data.mouse.lLastX && !input.data.mouse.lLastY && !input.data.mouse.usButtonFlags)) return;
    }
    else return;

    const InputDeviceKind kind = GetInputDeviceKind(input.header.hDevice);
    if (kind == DEVICE_VIRTUAL)
    {
        PhysicalInputRequireConfirmation();
        return;
    }

    // SendInput (including our replay shortcut) provides no hardware raw input.
    // Unknown devices never grant confirmation. RDP input cannot grant it even
    // if Windows exposes a device that looks like local hardware.
    if (kind == DEVICE_PHYSICAL && !PhysicalInputIsConfirmed() && IsLocalInteractiveSession())
    {
        InterlockedExchange(&physicalInputConfirmed, TRUE);
        if (inputWakeEvent) SetEvent(inputWakeEvent);
    }
}
