// Exercise the real input monitor with fake raw-input and device-tree APIs.
// No hooks, real input registration, keyboard input or device changes are made.
#include <windows.h>
#include <cfgmgr32.h>
#include <assert.h>
#include <stdio.h>
#include <wchar.h>

#define TEST_WINDOW ((HWND)(UINT_PTR)11)
#define TEST_WAKE ((HANDLE)(UINT_PTR)12)
#define TEST_INPUT ((HRAWINPUT)(UINT_PTR)13)
#define TEST_DEVICE ((HANDLE)(UINT_PTR)14)

static struct
{
    RAWINPUT input;
    const WCHAR *ids[8];
    unsigned nodes;
    BOOL local, registerFails, rawFails, infoFails, interfaceFails, badProperty;
    BOOL locateFails, idFails, parentFails, parentCycle;
    UINT readSize;
    unsigned registrations, removals, reads, deviceReads, localQueries, wakes;
} env;

static BOOL WINAPI FakeRegisterRawInputDevices(const RAWINPUTDEVICE *devices, UINT count, UINT size)
{
    assert(count == 2 && size == sizeof(*devices));
    assert(devices[0].usUsagePage == 1 && devices[0].usUsage == 2);
    assert(devices[1].usUsagePage == 1 && devices[1].usUsage == 6);
    if (devices[0].dwFlags == RIDEV_REMOVE)
    {
        assert(devices[1].dwFlags == RIDEV_REMOVE && !devices[0].hwndTarget && !devices[1].hwndTarget);
        ++env.removals;
    }
    else
    {
        assert(devices[0].dwFlags == (RIDEV_INPUTSINK | RIDEV_DEVNOTIFY));
        assert(devices[1].dwFlags == devices[0].dwFlags);
        assert(devices[0].hwndTarget == TEST_WINDOW && devices[1].hwndTarget == TEST_WINDOW);
        ++env.registrations;
    }
    return !env.registerFails;
}

static UINT WINAPI FakeGetRawInputData(HRAWINPUT input, UINT command, void *data, UINT *size, UINT headerSize)
{
    assert(input == TEST_INPUT && command == RID_INPUT && headerSize == sizeof(RAWINPUTHEADER));
    assert(*size >= sizeof(RAWINPUT));
    ++env.reads;
    if (env.rawFails) return (UINT)-1;
    memcpy(data, &env.input, env.readSize);
    *size = env.readSize;
    return env.readSize;
}

static UINT WINAPI FakeGetRawInputDeviceInfoW(HANDLE device, UINT command, void *data, UINT *length)
{
    assert(device && command == RIDI_DEVICENAME);
    ++env.deviceReads;
    if (env.infoFails) return (UINT)-1;
    const WCHAR path[] = L"\\\\?\\HID#test-device";
    assert(*length >= _countof(path));
    memcpy(data, path, sizeof(path));
    // Windows leaves the provided capacity unchanged on a successful call.
    return _countof(path);
}

static CONFIGRET WINAPI FakeCMGetInterfaceProperty(LPCWSTR path, const DEVPROPKEY *key,
    DEVPROPTYPE *type, BYTE *buffer, ULONG *bytes, ULONG flags)
{
    assert(path && key->pid == 256 && !flags);
    if (env.interfaceFails) return CR_FAILURE;
    *type = env.badProperty ? DEVPROP_TYPE_UINT32 : DEVPROP_TYPE_STRING;
    const ULONG length = (ULONG)((wcslen(env.ids[0]) + 1) * sizeof(WCHAR));
    assert(*bytes >= length);
    memcpy(buffer, env.ids[0], length);
    *bytes = length;
    return CR_SUCCESS;
}

static CONFIGRET WINAPI FakeCMLocate(PDEVINST node, DEVINSTID_W id, ULONG flags)
{
    assert(wcscmp(id, env.ids[0]) == 0 && flags == CM_LOCATE_DEVNODE_NORMAL);
    *node = 0;
    return env.locateFails ? CR_FAILURE : CR_SUCCESS;
}

static CONFIGRET WINAPI FakeCMGetId(DEVINST node, PWSTR id, ULONG length, ULONG flags)
{
    assert(node < env.nodes && !flags);
    if (env.idFails) return CR_FAILURE;
    assert(length > wcslen(env.ids[node]));
    wcscpy(id, env.ids[node]);
    return CR_SUCCESS;
}

static CONFIGRET WINAPI FakeCMGetParent(PDEVINST parent, DEVINST node, ULONG flags)
{
    assert(node < env.nodes && !flags);
    if (env.parentFails || node + 1 >= env.nodes) return CR_NO_SUCH_DEVNODE;
    *parent = env.parentCycle ? node : node + 1;
    return CR_SUCCESS;
}

static BOOL FakeIsLocalInteractiveSession(void)
{
    ++env.localQueries;
    return env.local;
}

static BOOL WINAPI FakeSetEvent(HANDLE event)
{
    assert(event == TEST_WAKE);
    ++env.wakes;
    return TRUE;
}

#define RegisterRawInputDevices FakeRegisterRawInputDevices
#define GetRawInputData FakeGetRawInputData
#define GetRawInputDeviceInfoW FakeGetRawInputDeviceInfoW
#define CM_Get_Device_Interface_PropertyW FakeCMGetInterfaceProperty
#define CM_Locate_DevNodeW FakeCMLocate
#define CM_Get_Device_IDW FakeCMGetId
#define CM_Get_Parent FakeCMGetParent
#define IsLocalInteractiveSession FakeIsLocalInteractiveSession
#define SetEvent FakeSetEvent
#include "../src/physical_input.c"

static void ResetEnvironment(void)
{
    PhysicalInputShutdown();
    ZeroMemory(&env, sizeof(env));
    env.local = TRUE;
    env.input.header.dwSize = env.readSize = sizeof(RAWINPUT);
    env.input.header.dwType = RIM_TYPEKEYBOARD;
    env.input.header.hDevice = TEST_DEVICE;
    env.input.data.keyboard.VKey = 'A';
    env.ids[0] = L"HID\\VID_TEST";
    env.ids[1] = L"USB\\VID_TEST";
    env.ids[2] = L"PCI\\VEN_TEST";
    env.ids[3] = L"ACPI\\PNP0A08\\0";
    env.ids[4] = L"ACPI_HAL\\PNP0C08\\0";
    env.ids[5] = L"ROOT\\ACPI_HAL\\0000";
    env.ids[6] = L"HTREE\\ROOT\\0";
    env.nodes = 7;
    assert(PhysicalInputInitialize(TEST_WINDOW, TEST_WAKE));
    assert(!PhysicalInputIsConfirmed());
}

static void TestPhysicalConfirmationAndLifecycle(void)
{
    ResetEnvironment();
    env.input.header.dwSize = env.readSize = offsetof(RAWINPUT, data) + sizeof(RAWKEYBOARD);
    for (unsigned i = 0; i < 100; ++i) PhysicalInputHandle(TEST_INPUT);
    assert(PhysicalInputIsConfirmed());
    assert(env.wakes == 1 && env.deviceReads == 1 && env.localQueries == 1);
    PhysicalInputRequireConfirmation();
    assert(!PhysicalInputIsConfirmed() && env.wakes == 2);
    PhysicalInputRequireConfirmation();
    assert(env.wakes == 2);
    PhysicalInputHandle(TEST_INPUT);
    assert(PhysicalInputIsConfirmed() && env.wakes == 3);
    PhysicalInputShutdown();
    assert(env.removals == 1 && !PhysicalInputIsConfirmed());
    PhysicalInputHandle(TEST_INPUT);
    assert(env.reads == 101);
}

static void TestRemoteAndVirtualInput(void)
{
    ResetEnvironment();
    env.local = FALSE; // Even hardware-looking input cannot authorize an RDP session.
    PhysicalInputHandle(TEST_INPUT);
    assert(!PhysicalInputIsConfirmed());
    env.local = TRUE;
    assert(!PhysicalInputIsConfirmed()); // Disconnect/unlock alone is insufficient.
    PhysicalInputHandle(TEST_INPUT);
    assert(PhysicalInputIsConfirmed());

    env.input.header.hDevice = (HANDLE)(UINT_PTR)99;
    env.ids[1] = L"ROOT\\OrayVirtualHID";
    PhysicalInputHandle(TEST_INPUT);
    assert(!PhysicalInputIsConfirmed()); // Virtual remote input revokes a local grant.
    env.input.header.hDevice = TEST_DEVICE;
    PhysicalInputHandle(TEST_INPUT);
    assert(PhysicalInputIsConfirmed()); // The remote service need not exit.

    ResetEnvironment();
    env.ids[0] = L"USB\\VID_TEST";
    env.ids[1] = L"root\\VirtualUsbController";
    PhysicalInputHandle(TEST_INPUT);
    assert(!PhysicalInputIsConfirmed());

    ResetEnvironment();
    env.ids[1] = L"SWD\\RemoteKeyboard";
    PhysicalInputHandle(TEST_INPUT);
    assert(!PhysicalInputIsConfirmed());

    ResetEnvironment();
    env.input.header.hDevice = NULL; // Synthetic input has no hardware evidence.
    PhysicalInputHandle(TEST_INPUT);
    assert(!PhysicalInputIsConfirmed() && env.deviceReads == 0);
}

static void TestPhysicalDeviceFamiliesAndMouse(void)
{
    const WCHAR *buses[] = {L"USB\\VID_TEST", L"BTHENUM\\Keyboard", L"BTHLEDEVICE\\Keyboard", L"ACPI\\PNP0303"};
    for (unsigned i = 0; i < _countof(buses); ++i)
    {
        ResetEnvironment();
        env.ids[1] = buses[i];
        PhysicalInputHandle(TEST_INPUT);
        assert(PhysicalInputIsConfirmed());
    }

    ResetEnvironment();
    env.input.header.dwType = RIM_TYPEMOUSE;
    ZeroMemory(&env.input.data, sizeof(env.input.data));
    PhysicalInputHandle(TEST_INPUT);
    assert(!PhysicalInputIsConfirmed()); // Empty packets are not user activity.
    env.input.data.mouse.lLastX = 1;
    PhysicalInputHandle(TEST_INPUT);
    assert(PhysicalInputIsConfirmed());
    PhysicalInputRequireConfirmation();
    env.input.data.mouse.lLastX = 0;
    env.input.data.mouse.usButtonFlags = RI_MOUSE_LEFT_BUTTON_DOWN;
    PhysicalInputHandle(TEST_INPUT);
    assert(PhysicalInputIsConfirmed());
}

static void TestDeviceCacheInvalidation(void)
{
    ResetEnvironment();
    env.ids[1] = L"ROOT\\VirtualKeyboard";
    PhysicalInputHandle(TEST_INPUT);
    assert(!PhysicalInputIsConfirmed());
    env.ids[1] = L"USB\\VID_TEST";
    PhysicalInputHandle(TEST_INPUT);
    assert(!PhysicalInputIsConfirmed() && env.deviceReads == 1);
    PhysicalInputForgetDevices(); // WM_INPUT_DEVICE_CHANGE: handle may be reused.
    PhysicalInputHandle(TEST_INPUT);
    assert(PhysicalInputIsConfirmed() && env.deviceReads == 2);
}

static void TestIncompleteEvidence(void)
{
    for (unsigned failure = 0; failure < 9; ++failure)
    {
        ResetEnvironment();
        switch (failure)
        {
            case 0: env.rawFails = TRUE; break;
            case 1: env.infoFails = TRUE; break;
            case 2: env.interfaceFails = TRUE; break;
            case 3: env.badProperty = TRUE; break;
            case 4: env.locateFails = TRUE; break;
            case 5: env.idFails = TRUE; break;
            case 6: env.parentFails = TRUE; break;
            case 7: env.parentCycle = TRUE; break;
            case 8: env.nodes = 3; break; // Physical ancestry was not fully verified.
        }
        PhysicalInputHandle(TEST_INPUT);
        assert(!PhysicalInputIsConfirmed());
    }
    ResetEnvironment();
    env.input.header.dwSize = env.readSize = sizeof(RAWINPUTHEADER);
    PhysicalInputHandle(TEST_INPUT);
    assert(!PhysicalInputIsConfirmed());
    ResetEnvironment();
    env.input.data.keyboard.Flags = RI_KEY_BREAK;
    PhysicalInputHandle(TEST_INPUT);
    assert(!PhysicalInputIsConfirmed());
    ResetEnvironment();
    env.ids[1] = L"HID\\UnknownBus";
    env.ids[2] = L"HTREE\\ROOT\\0";
    env.nodes = 3;
    PhysicalInputHandle(TEST_INPUT);
    assert(!PhysicalInputIsConfirmed());
    ResetEnvironment();
    PhysicalInputShutdown();
    env.registerFails = TRUE;
    assert(!PhysicalInputInitialize(TEST_WINDOW, TEST_WAKE));
    PhysicalInputHandle(TEST_INPUT);
    assert(!PhysicalInputIsConfirmed() && env.reads == 0);
}

int main(void)
{
    TestPhysicalConfirmationAndLifecycle();
    TestRemoteAndVirtualInput();
    TestPhysicalDeviceFamiliesAndMouse();
    TestDeviceCacheInvalidation();
    TestIncompleteEvidence();
    PhysicalInputShutdown();
    puts("Physical input tests passed (hardware, remote/virtual input, lifecycle and failure guards).");
    return 0;
}
