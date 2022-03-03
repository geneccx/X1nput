/*
	Thanks to r57zone for his Xinput emulation library
	https://github.com/r57zone/XInput
*/

#include "stdafx.h"

#define XINPUT_GAMEPAD_DPAD_UP          0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN        0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT        0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT       0x0008
#define XINPUT_GAMEPAD_START            0x0010
#define XINPUT_GAMEPAD_BACK             0x0020
#define XINPUT_GAMEPAD_LEFT_THUMB       0x0040
#define XINPUT_GAMEPAD_RIGHT_THUMB      0x0080
#define XINPUT_GAMEPAD_LEFT_SHOULDER    0x0100
#define XINPUT_GAMEPAD_RIGHT_SHOULDER   0x0200
#define XINPUT_GAMEPAD_A                0x1000
#define XINPUT_GAMEPAD_B                0x2000
#define XINPUT_GAMEPAD_X                0x4000
#define XINPUT_GAMEPAD_Y				0x8000

#define XINPUT_CAPS_FFB_SUPPORTED       0x0001
#define XINPUT_CAPS_WIRELESS            0x0002
#define XINPUT_CAPS_PMD_SUPPORTED       0x0008
#define XINPUT_CAPS_NO_NAVIGATION       0x0010

//
// Flags for battery status level
//
#define BATTERY_TYPE_DISCONNECTED       0x00    // This device is not connected
#define BATTERY_TYPE_WIRED              0x01    // Wired device, no battery
#define BATTERY_TYPE_ALKALINE           0x02    // Alkaline battery source
#define BATTERY_TYPE_NIMH               0x03    // Nickel Metal Hydride battery source
#define BATTERY_TYPE_UNKNOWN            0xFF    // Cannot determine the battery type

// These are only valid for wireless, connected devices, with known battery types
// The amount of use time remaining depends on the type of device.
#define BATTERY_LEVEL_EMPTY             0x00
#define BATTERY_LEVEL_LOW               0x01
#define BATTERY_LEVEL_MEDIUM            0x02
#define BATTERY_LEVEL_FULL              0x03


#define XINPUT_DEVTYPE_GAMEPAD          0x01
#define XINPUT_DEVSUBTYPE_WHEEL			0x02

#define BATTERY_TYPE_DISCONNECTED		0x00

#define XUSER_MAX_COUNT                 4
#define MAX_PLAYER_COUNT				8
#define XUSER_INDEX_ANY					0x000000FF

#define CONFIG_PATH						_T(".\\X1nput.ini")


using namespace ABI::Windows::Foundation::Collections;
using namespace ABI::Windows::Gaming::Input;
using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;

const float c_XboxOneThumbDeadZone = .24f;  // Recommended Xbox One controller deadzone

ComPtr<IRacingWheelStatics> racingWheelStatics;
ComPtr<IRacingWheel> racingWheels[MAX_PLAYER_COUNT];
EventRegistrationToken mUserChangeToken[MAX_PLAYER_COUNT];

EventRegistrationToken gAddedToken;
EventRegistrationToken gRemovedToken;

int mMostRecentWheel = 0;

float RTriggerStrength = 0.25f;
float LTriggerStrength = 0.25f;
float RMotorStrength = 1.0f;
float LMotorStrength = 1.0f;

bool TriggerSwap = false;
bool MotorSwap = false;

// Config related methods, thanks to xiaohe521, https://www.codeproject.com/Articles/10809/A-Small-Class-to-Read-INI-File
#pragma region Config loading
float GetConfigFloat(LPCTSTR AppName, LPCTSTR KeyName, LPCTSTR Default) {
	TCHAR result[256];
	GetPrivateProfileString(AppName, KeyName, Default, result, 256, CONFIG_PATH);
	return _tstof(result);
}

bool GetConfigBool(LPCTSTR AppName, LPCTSTR KeyName, LPCTSTR Default) {
	TCHAR result[256];
	GetPrivateProfileString(AppName, KeyName, Default, result, 256, CONFIG_PATH);
	// Thanks to CookiePLMonster for recommending _tcsicmp to me
	return _tcsicmp(result, _T("true")) == 0 ? true : false;
}

void GetConfig() {
	LTriggerStrength = GetConfigFloat(_T("Triggers"), _T("LeftStrength"), _T("0.25"));
	RTriggerStrength = GetConfigFloat(_T("Triggers"), _T("RightStrength"), _T("0.25"));
	TriggerSwap = GetConfigBool(_T("Triggers"), _T("SwapSides"), _T("False"));

	LMotorStrength = GetConfigFloat(_T("Motors"), _T("LeftStrength"), _T("1.0"));
	RMotorStrength = GetConfigFloat(_T("Motors"), _T("RightStrength"), _T("1.0"));
	MotorSwap = GetConfigBool(_T("Motors"), _T("SwapSides"), _T("False"));
}
#pragma endregion

bool ReconnectIO(bool OpenNewConsole)
{
	bool MadeConsole;

	MadeConsole = false;
	if (!AttachConsole(ATTACH_PARENT_PROCESS))
	{
		if (!OpenNewConsole)
			return false;

		MadeConsole = true;
		if (!AllocConsole())
			return false;   // Could throw here
	}

	FILE* fDummy;
	freopen_s(&fDummy, "CONIN$", "r", stdin);
	freopen_s(&fDummy, "CONOUT$", "w", stderr);
	freopen_s(&fDummy, "CONOUT$", "w", stdout);

	// C++ streams to console
	std::ios_base::sync_with_stdio();

	_flushall();

	return MadeConsole;
}

// Gamepad scanning and racingWheel related methods
#pragma region Stuff from GamePad.cpp

// DeadZone enum
enum DeadZone
{
	DEAD_ZONE_INDEPENDENT_AXES = 0,
	DEAD_ZONE_CIRCULAR,
	DEAD_ZONE_NONE,
};

float ApplyLinearDeadZone(float value, float maxValue, float deadZoneSize)
{
	if (value < -deadZoneSize)
	{
		// Increase negative values to remove the deadzone discontinuity.
		value += deadZoneSize;
	}
	else if (value > deadZoneSize)
	{
		// Decrease positive values to remove the deadzone discontinuity.
		value -= deadZoneSize;
	}
	else
	{
		// Values inside the deadzone come out zero.
		return 0;
	}

	// Scale into 0-1 range.
	float scaledValue = value / (maxValue - deadZoneSize);
	return std::max(-1.f, std::min(scaledValue, 1.f));
}

// Applies DeadZone to thumbstick positions
void ApplyStickDeadZone(float x, float y, DeadZone deadZoneMode, float maxValue, float deadZoneSize, _Out_ float& resultX, _Out_ float& resultY)
{
	switch (deadZoneMode)
	{
	case DEAD_ZONE_INDEPENDENT_AXES:
		resultX = ApplyLinearDeadZone(x, maxValue, deadZoneSize);
		resultY = ApplyLinearDeadZone(y, maxValue, deadZoneSize);
		break;

	case DEAD_ZONE_CIRCULAR:
	{
		float dist = sqrtf(x*x + y * y);
		float wanted = ApplyLinearDeadZone(dist, maxValue, deadZoneSize);

		float scale = (wanted > 0.f) ? (wanted / dist) : 0.f;

		resultX = std::max(-1.f, std::min(x * scale, 1.f));
		resultY = std::max(-1.f, std::min(y * scale, 1.f));
	}
	break;

	default: // GamePad::DEAD_ZONE_NONE
		resultX = ApplyLinearDeadZone(x, maxValue, 0);
		resultY = ApplyLinearDeadZone(y, maxValue, 0);
		break;
	}
}

// UserChanged Event
static HRESULT UserChanged(ABI::Windows::Gaming::Input::IGameController*, ABI::Windows::System::IUserChangedEventArgs*)
{
	return S_OK;
}

// Scans for racingWheels (adds/removes racingWheels from racingWheels array)
void ScanRacingWheels()
{
	std::cout << "ScanRacingWheels" << std::endl;

	ComPtr<IVectorView<RacingWheel*>> wheels;
	HRESULT hr = racingWheelStatics->get_RacingWheels(&wheels);
	assert(SUCCEEDED(hr));

	unsigned int count = 0;
	hr = wheels->get_Size(&count);
	assert(SUCCEEDED(hr));

	std::cout << "Found " << count << std::endl;

	// Check for removed racingWheels
	for (size_t j = 0; j < MAX_PLAYER_COUNT; ++j)
	{
		if (racingWheels[j])
		{
			unsigned int k = 0;
			for (; k < count; ++k)
			{
				ComPtr<IRacingWheel> wheel;
				hr = wheels->GetAt(k, wheel.GetAddressOf());
				if (SUCCEEDED(hr) && (wheel == racingWheels[j]))
				{
					break;
				}
			}

			if (k >= count)
			{
				ComPtr<IGameController> ctrl;
				hr = racingWheels[j].As(&ctrl);
				if (SUCCEEDED(hr) && ctrl)
				{
					(void)ctrl->remove_UserChanged(mUserChangeToken[j]);
					mUserChangeToken[j].value = 0;
				}

				racingWheels[j].Reset();
			}
		}
	}

	// Check for added racingWheels
	for (unsigned int j = 0; j < count; ++j)
	{
		ComPtr<IRacingWheel> wheel;
		hr = wheels->GetAt(j, wheel.GetAddressOf());
		if (SUCCEEDED(hr))
		{
			size_t empty = MAX_PLAYER_COUNT;
			size_t k = 0;
			for (; k < MAX_PLAYER_COUNT; ++k)
			{
				if (racingWheels[k] == wheel)
				{
					if (j == (count - 1))
						mMostRecentWheel = static_cast<int>(k);
					break;
				}
				else if (!racingWheels[k])
				{
					if (empty >= MAX_PLAYER_COUNT)
						empty = k;
				}
			}

			if (k >= MAX_PLAYER_COUNT)
			{
				// Silently ignore "extra" racingWheels as there's no hard limit
				if (empty < MAX_PLAYER_COUNT)
				{
					racingWheels[empty] = wheel;
					if (j == (count - 1))
						mMostRecentWheel = static_cast<int>(empty);

					ComPtr<IGameController> ctrl;
					hr = wheel.As(&ctrl);
					if (SUCCEEDED(hr) && ctrl)
					{
						typedef __FITypedEventHandler_2_Windows__CGaming__CInput__CIGameController_Windows__CSystem__CUserChangedEventArgs UserHandler;
						hr = ctrl->add_UserChanged(Callback<UserHandler>(UserChanged).Get(), &mUserChangeToken[empty]);
						assert(SUCCEEDED(hr));
					}
				}
			}
		}
	}
}

// GamepadAdded Event
static HRESULT RacingWheelAdded(IInspectable *, ABI::Windows::Gaming::Input::IRacingWheel*)
{
	std::cout << "RacingWheelAdded" << std::endl;

	ScanRacingWheels();
	return S_OK;
}

// GamepadRemoved Event
static HRESULT RacingWheelRemoved(IInspectable *, ABI::Windows::Gaming::Input::IRacingWheel*)
{
	std::cout << "RacingWheelRemoved" << std::endl;

	ScanRacingWheels();
	return S_OK;
}
#pragma endregion

/*
	Thanks to CookiePLMonster for suggesting this.
	I definitely should have asked how to implement it, but oh well, there's still a lot of time for fixing.
	Oddly enough, this seemed to have fixed HITMAN 2 once again. That game is really cursed. Before, only debug version of the DLL worked.
*/
#pragma region InitOnceExecuteOnce

// Global variable for one-time initialization structure
INIT_ONCE g_InitOnce = INIT_ONCE_STATIC_INIT; // Static initialization

// Initialization callback function 
BOOL CALLBACK InitHandleFunction(
	PINIT_ONCE InitOnce,
	PVOID Parameter,
	PVOID *lpContext);

bool InitializeRacingWheel()
{
	// Execute the initialization callback function 
	BOOL bStatus = InitOnceExecuteOnce(&g_InitOnce,          // One-time initialization structure
		InitHandleFunction,   // Pointer to initialization callback function
		NULL,                 // Optional parameter to callback function (not used)
		NULL);          // Receives pointer to event object stored in g_InitOnce

// InitOnceExecuteOnce function succeeded.
	return bStatus != FALSE;
}

// Initialization callback function that creates the event object 
BOOL CALLBACK InitHandleFunction(
	PINIT_ONCE InitOnce,        // Pointer to one-time initialization structure        
	PVOID Parameter,            // Optional parameter passed by InitOnceExecuteOnce            
	PVOID *lpContext)           // Receives pointer to event object           
{
	ReconnectIO(true);

	HRESULT hr = RoInitialize(RO_INIT_SINGLETHREADED);
	assert(SUCCEEDED(hr));
	std::cout << "RoInitialize(st): " << hr << std::endl;

	hr = RoGetActivationFactory(HStringReference(L"Windows.Gaming.Input.RacingWheel").Get(), __uuidof(IRacingWheelStatics), &racingWheelStatics);
	assert(SUCCEEDED(hr));
	std::cout << "RoGetActivationFactory: " << hr << std::endl;
	std::cout << "racingWheelStatics: " << racingWheelStatics << std::endl;

	typedef __FIEventHandler_1_Windows__CGaming__CInput__CRacingWheel AddedHandler;
	hr = racingWheelStatics->add_RacingWheelAdded(Callback<AddedHandler>(RacingWheelAdded).Get(), &gAddedToken);
	assert(SUCCEEDED(hr));
	std::cout << "add_RacingWheelAdded: " << hr
		<< ", token=" << gAddedToken.value
		<< std::endl;

	typedef __FIEventHandler_1_Windows__CGaming__CInput__CRacingWheel RemovedHandler;
	hr = racingWheelStatics->add_RacingWheelRemoved(Callback<RemovedHandler>(RacingWheelRemoved).Get(), &gRemovedToken);
	assert(SUCCEEDED(hr));
	std::cout << "add_RacingWheelRemoved: " << hr
		<< ", token=" << gRemovedToken.value
		<< std::endl;

	// GetConfig();

	ScanRacingWheels();

	return TRUE;
}

#pragma endregion

//
// Structures used by XInput APIs
//
typedef struct _XINPUT_GAMEPAD
{
	WORD                                wButtons;
	BYTE                                bLeftTrigger;
	BYTE                                bRightTrigger;
	SHORT                               sThumbLX;
	SHORT                               sThumbLY;
	SHORT                               sThumbRX;
	SHORT                               sThumbRY;
} XINPUT_GAMEPAD, *PXINPUT_GAMEPAD;

typedef struct _XINPUT_STATE
{
	DWORD                               dwPacketNumber;
	XINPUT_GAMEPAD                      Gamepad;
} XINPUT_STATE, *PXINPUT_STATE;

typedef struct _XINPUT_VIBRATION
{
	WORD                                wLeftMotorSpeed;
	WORD                                wRightMotorSpeed;
} XINPUT_VIBRATION, *PXINPUT_VIBRATION;

typedef struct _XINPUT_CAPABILITIES
{
	BYTE                                Type;
	BYTE                                SubType;
	WORD                                Flags;
	XINPUT_GAMEPAD                      Gamepad;
	XINPUT_VIBRATION                    Vibration;
} XINPUT_CAPABILITIES, *PXINPUT_CAPABILITIES;

typedef struct _XINPUT_BATTERY_INFORMATION
{
	BYTE BatteryType;
	BYTE BatteryLevel;
} XINPUT_BATTERY_INFORMATION, *PXINPUT_BATTERY_INFORMATION;

typedef struct _XINPUT_KEYSTROKE
{
	WORD    VirtualKey;
	WCHAR   Unicode;
	WORD    Flags;
	BYTE    UserIndex;
	BYTE    HidCode;
} XINPUT_KEYSTROKE, *PXINPUT_KEYSTROKE;

#define DLLEXPORT extern "C" __declspec(dllexport)

/*
  Racing wheel controller.

  Left Stick X reports the wheel rotation, 
  Right Trigger is the acceleration pedal,
  and Left Trigger is the brake pedal.

  Includes Directional Pad and most standard buttons (A, B, X, Y, START, BACK, LB, RB). LSB and RSB are optional.
  https://docs.microsoft.com/en-us/windows/win32/xinput/xinput-and-controller-subtypes
 */
DLLEXPORT DWORD WINAPI XInputGetState(_In_ DWORD dwUserIndex, _Out_ XINPUT_STATE *pState)
{
	InitializeRacingWheel();
	//std::cout << "XInputGetState" << std::endl;

	if (racingWheels[dwUserIndex] == NULL) {
		return ERROR_DEVICE_NOT_CONNECTED;
	}

	auto racingWheel = racingWheels[dwUserIndex];

	RacingWheelReading state;
	HRESULT hr = racingWheel->GetCurrentReading(&state);

	if (SUCCEEDED(hr)) {

		DWORD keys = 0;

		//float Wheel = ApplyLinearDeadZone(state.Wheel, 1.f, c_XboxOneThumbDeadZone);

		pState->Gamepad.bRightTrigger = state.Throttle * 255;
		pState->Gamepad.bLeftTrigger = state.Brake * 255;

		pState->Gamepad.sThumbLX = (state.Wheel >= 0) ? state.Wheel * 32767 : state.Wheel * 32768;
		pState->Gamepad.sThumbLY = 0;

		pState->Gamepad.sThumbRX = 0;
		pState->Gamepad.sThumbRY = 0;

		if ((state.Buttons & RacingWheelButtons::RacingWheelButtons_Button3) != 0) keys += XINPUT_GAMEPAD_A;
		if ((state.Buttons & RacingWheelButtons::RacingWheelButtons_Button4) != 0) keys += XINPUT_GAMEPAD_B;
		if ((state.Buttons & RacingWheelButtons::RacingWheelButtons_Button5) != 0) keys += XINPUT_GAMEPAD_X;
		if ((state.Buttons & RacingWheelButtons::RacingWheelButtons_Button6) != 0) keys += XINPUT_GAMEPAD_Y;

		/*
		if ((state.Buttons & RacingWheelButtons::GamepadButtons_RightThumbstick) != 0) keys += XINPUT_GAMEPAD_RIGHT_THUMB;
		if ((state.Buttons & RacingWheelButtons::GamepadButtons_LeftThumbstick) != 0) keys += XINPUT_GAMEPAD_LEFT_THUMB;
		if ((state.Buttons & RacingWheelButtons::GamepadButtons_RightShoulder) != 0) keys += XINPUT_GAMEPAD_RIGHT_SHOULDER;
		if ((state.Buttons & RacingWheelButtons::GamepadButtons_LeftShoulder) != 0) keys += XINPUT_GAMEPAD_LEFT_SHOULDER;
		*/
		if ((state.Buttons & RacingWheelButtons::RacingWheelButtons_Button2) != 0) keys += XINPUT_GAMEPAD_BACK;
		if ((state.Buttons & RacingWheelButtons::RacingWheelButtons_Button1) != 0) keys += XINPUT_GAMEPAD_START;

		if ((state.Buttons & RacingWheelButtons::RacingWheelButtons_DPadUp) != 0) keys += XINPUT_GAMEPAD_DPAD_UP;
		if ((state.Buttons & RacingWheelButtons::RacingWheelButtons_DPadDown) != 0) keys += XINPUT_GAMEPAD_DPAD_DOWN;
		if ((state.Buttons & RacingWheelButtons::RacingWheelButtons_DPadLeft) != 0) keys += XINPUT_GAMEPAD_DPAD_LEFT;
		if ((state.Buttons & RacingWheelButtons::RacingWheelButtons_DPadRight) != 0) keys += XINPUT_GAMEPAD_DPAD_RIGHT;

		// Press both shoulder buttons and the start button to reload configuration.
		/*
		if ((state.Buttons & RacingWheelButtons::GamepadButtons_RightShoulder) != 0 &&
			(state.Buttons & RacingWheelButtons::GamepadButtons_LeftShoulder) != 0 &&
			(state.Buttons & RacingWheelButtons::GamepadButtons_Menu) != 0) {
			GetConfig();
		}*/


		pState->dwPacketNumber = state.Timestamp;
		pState->Gamepad.wButtons = keys;

		return ERROR_SUCCESS;
	}
	else
	{
		return ERROR_DEVICE_NOT_CONNECTED;
	}

}

DLLEXPORT DWORD WINAPI XInputSetState(_In_ DWORD dwUserIndex, _In_ XINPUT_VIBRATION *pVibration)
{
	InitializeRacingWheel();
	std::cout << "XInputSetState" << std::endl;

	if (racingWheels[dwUserIndex] == NULL) {
		return ERROR_DEVICE_NOT_CONNECTED;
	}

	auto racingWheel = racingWheels[dwUserIndex];

	RacingWheelReading state;
	HRESULT hr = racingWheel->GetCurrentReading(&state);

	if (SUCCEEDED(hr)) {

		/*
			TODO: support wheel motor FFB:
			https://docs.microsoft.com/en-us/windows/uwp/gaming/racing-wheel-and-force-feedback
			GamepadVibration vibration;

			float LSpeed = pVibration->wLeftMotorSpeed / 65535.0f;
			float RSpeed = pVibration->wRightMotorSpeed / 65535.0f;

			vibration.LeftMotor = MotorSwap ? RSpeed * LMotorStrength : LSpeed * LMotorStrength;
			vibration.RightMotor = MotorSwap ? LSpeed * RMotorStrength : RSpeed * RMotorStrength;

			vibration.LeftTrigger = TriggerSwap ? RSpeed * LTriggerStrength : LSpeed * LTriggerStrength;
			vibration.RightTrigger = TriggerSwap ? LSpeed * RTriggerStrength : RSpeed * RTriggerStrength;

			racingWheel->put_Vibration(vibration);
		*/

		return ERROR_SUCCESS;
	}

	else
	{
		return ERROR_DEVICE_NOT_CONNECTED;
	}
}


DLLEXPORT DWORD WINAPI XInputGetCapabilities(_In_ DWORD dwUserIndex, _In_ DWORD dwFlags, _Out_ XINPUT_CAPABILITIES *pCapabilities)
{
	InitializeRacingWheel();
	std::cout << "XInputGetCapabilities" << std::endl;

	if (racingWheels[dwUserIndex] == NULL) {
		return ERROR_DEVICE_NOT_CONNECTED;
	}

	auto racingWheel = racingWheels[dwUserIndex];

	RacingWheelReading state;
	HRESULT hr = racingWheel->GetCurrentReading(&state);

	if (SUCCEEDED(hr)) {

		ComPtr<IGameController> racingWheelInfo;
		racingWheels[dwUserIndex].As(&racingWheelInfo);

		boolean wireless;
		racingWheelInfo->get_IsWireless(&wireless);

		ABI::Windows::Gaming::Input::ForceFeedback::IForceFeedbackMotor* wheelMotor;
		racingWheel->get_WheelMotor(&wheelMotor);

		pCapabilities->Type = XINPUT_DEVTYPE_GAMEPAD;

		pCapabilities->SubType = XINPUT_DEVSUBTYPE_WHEEL;

		if (wheelMotor) pCapabilities->Flags += XINPUT_CAPS_FFB_SUPPORTED;
		if (wireless) pCapabilities->Flags += XINPUT_CAPS_WIRELESS;

		return ERROR_SUCCESS;
	}
	else
	{
		return ERROR_DEVICE_NOT_CONNECTED;
	}
}

DLLEXPORT void WINAPI XInputEnable(_In_ BOOL enable)
{
	InitializeRacingWheel();

	std::cout << "XInputEnable" << std::endl;
	ScanRacingWheels();
}

DLLEXPORT DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD dwUserIndex, GUID* pDSoundRenderGuid, GUID* pDSoundCaptureGuid)
{
	InitializeRacingWheel();
	std::cout << "XInputGetDSoundAudioDeviceGuids" << std::endl;

	if (racingWheels[dwUserIndex] == NULL) {
		return ERROR_DEVICE_NOT_CONNECTED;
	}

	auto racingWheel = racingWheels[dwUserIndex];

	RacingWheelReading state;
	HRESULT hr = racingWheel->GetCurrentReading(&state);

	if (SUCCEEDED(hr)) {
		return ERROR_SUCCESS;
	}
	else
	{
		return ERROR_DEVICE_NOT_CONNECTED;
	}
}

DLLEXPORT DWORD WINAPI XInputGetBatteryInformation(_In_ DWORD dwUserIndex, _In_ BYTE devType, _Out_ XINPUT_BATTERY_INFORMATION *pBatteryInformation)
{
	InitializeRacingWheel();
	std::cout << "XInputGetBatteryInformation" << std::endl;

	if (racingWheels[dwUserIndex] == NULL) {
		return ERROR_DEVICE_NOT_CONNECTED;
	}

	auto racingWheel = racingWheels[dwUserIndex];

	RacingWheelReading state;
	HRESULT hr = racingWheel->GetCurrentReading(&state);

	if (SUCCEEDED(hr)) {
		return ERROR_SUCCESS;
	}
	else
	{
		return ERROR_DEVICE_NOT_CONNECTED;
	}
	/*

	ComPtr<IGameControllerBatteryInfo> battInf;
	racingWheels[dwUserIndex].As(&battInf);

	ComPtr<IGameController> test;

	ComPtr<ABI::Windows::Devices::Power::IBatteryReport> battReport;
	battInf->TryGetBatteryReport(&battReport);

	//Can't find any information on IReference
	int Charge;
	battReport->get_RemainingCapacityInMilliwattHours(&Charge);

	*/
	/*
	InitializeGamepad();
	auto state = m_gamePad->GetCapabilities(dwUserIndex);
	if (state.connected) {
		return ERROR_SUCCESS;
	}
	else
	{
		return ERROR_DEVICE_NOT_CONNECTED;
	}
	*/
}

DLLEXPORT DWORD WINAPI XInputGetKeystroke(DWORD dwUserIndex, DWORD dwReserved, PXINPUT_KEYSTROKE pKeystroke)
{
	InitializeRacingWheel();
	std::cout << "XInputGetKeystroke" << std::endl;

	if (racingWheels[dwUserIndex] == NULL) {
		return ERROR_DEVICE_NOT_CONNECTED;
	}

	RacingWheelReading state;
	HRESULT hr = racingWheels[dwUserIndex]->GetCurrentReading(&state);

	if (SUCCEEDED(hr)) {
		return ERROR_SUCCESS;
	}
	else
	{
		return ERROR_DEVICE_NOT_CONNECTED;
	}
}

DLLEXPORT DWORD WINAPI XInputGetStateEx(_In_ DWORD dwUserIndex, _Out_ XINPUT_STATE *pState)
{
	return XInputGetState(dwUserIndex, pState);
}

DLLEXPORT DWORD WINAPI XInputWaitForGuideButton(_In_ DWORD dwUserIndex, _In_ DWORD dwFlag, _In_ LPVOID pVoid)
{
	InitializeRacingWheel();
	std::cout << "XInputWaitForGuideButton" << std::endl;

	if (racingWheels[dwUserIndex] == NULL) {
		return ERROR_DEVICE_NOT_CONNECTED;
	}

	auto racingWheel = racingWheels[dwUserIndex];

	RacingWheelReading state;
	HRESULT hr = racingWheel->GetCurrentReading(&state);

	if (SUCCEEDED(hr)) {
		return ERROR_SUCCESS;
	}
	else
	{
		return ERROR_DEVICE_NOT_CONNECTED;
	}
}

DLLEXPORT DWORD XInputCancelGuideButtonWait(_In_ DWORD dwUserIndex)
{
	InitializeRacingWheel();
	std::cout << "XInputCancelGuideButtonWait" << std::endl;

	if (racingWheels[dwUserIndex] == NULL) {
		return ERROR_DEVICE_NOT_CONNECTED;
	}

	auto racingWheel = racingWheels[dwUserIndex];

	RacingWheelReading state;
	HRESULT hr = racingWheel->GetCurrentReading(&state);

	if (SUCCEEDED(hr)) {
		return ERROR_SUCCESS;
	}
	else
	{
		return ERROR_DEVICE_NOT_CONNECTED;
	}
}

DLLEXPORT DWORD XInputPowerOffController(_In_ DWORD dwUserIndex)
{
	InitializeRacingWheel();
	std::cout << "XInputPowerOffController" << std::endl;

	if (racingWheels[dwUserIndex] == NULL) {
		return ERROR_DEVICE_NOT_CONNECTED;
	}

	auto racingWheel = racingWheels[dwUserIndex];

	RacingWheelReading state;
	HRESULT hr = racingWheel->GetCurrentReading(&state);

	if (SUCCEEDED(hr)) {
		return ERROR_SUCCESS;
	}
	else
	{
		return ERROR_DEVICE_NOT_CONNECTED;
	}
}
