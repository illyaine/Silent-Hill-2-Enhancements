/**
* Copyright (C) 2020 Adrian Zdanowicz
*
* This software is  provided 'as-is', without any express  or implied  warranty. In no event will the
* authors be held liable for any damages arising from the use of this software.
* Permission  is granted  to anyone  to use  this software  for  any  purpose,  including  commercial
* applications, and to alter it and redistribute it freely, subject to the following restrictions:
*
*   1. The origin of this software must not be misrepresented; you must not claim that you  wrote the
*      original  software. If you use this  software  in a product, an  acknowledgment in the product
*      documentation would be appreciated but is not required.
*   2. Altered source versions must  be plainly  marked as such, and  must not be  misrepresented  as
*      being the original software.
*   3. This notice may not be removed or altered from any source distribution.
*/

#define WIN32_LEAN_AND_MEAN
#define _USE_MATH_DEFINES // For cmath
#include <Windows.h>
#include "Common\Utils.h"
#include "Common\Settings.h"
#include "Logging\Logging.h"


#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

struct AnalogState
{
	short X, Y;
	float angle, length;
};

struct GamePadState // Very incomplete
{
	AnalogState m_leftStick;
	AnalogState m_rightStick;
};

static_assert( sizeof(AnalogState) == 0xC, "Wrong size: AnalogState" );

DIJOYSTATE2* dinputJoyState;

static void (*orgProcessDInputData)(GamePadState*);
void ProcessDInputData_Hook(GamePadState* state)
{
	DIJOYSTATE2& joystickState = *dinputJoyState;

	if (DPadMovementFix)
	{
		const DWORD povAngle = joystickState.rgdwPOV[0];
		const bool centered = LOWORD(povAngle) == 0xFFFF;
		if ( !centered )
		{
			// Override analog values with DPad values
			const double angleRadians = static_cast<double>(povAngle) * M_PI / (180.0 * 100.0);

			joystickState.lX = static_cast<LONG>(std::sin(angleRadians) * 32767.0);
			joystickState.lY = static_cast<LONG>(std::cos(angleRadians) * -32767.0);
		}
	}

	// Populate right stick with data
	switch (RestoreSearchCamMovement)
	{
	case 1: // XRot/YRot
		state->m_rightStick.X = static_cast<short>(joystickState.lRx);
		state->m_rightStick.Y = static_cast<short>(joystickState.lRy);
		break;
	case 2: // Z axis/ZRot
		state->m_rightStick.X = static_cast<short>(joystickState.lZ);
		state->m_rightStick.Y = static_cast<short>(joystickState.lRz);
		break;
	case 3: // ZRot/Z axis
		state->m_rightStick.X = static_cast<short>(joystickState.lRz);
		state->m_rightStick.Y = static_cast<short>(joystickState.lZ);
		break;
	default:
		break;
	}

	orgProcessDInputData(state);
}

void __stdcall GetDeviceState_Hook(IDirectInputDevice8A* device)
{
	DIJOYSTATE2& joystickState = *dinputJoyState;
	bool success = false;
	if (SUCCEEDED(device->Acquire()))
	{
		if(SUCCEEDED(device->GetDeviceState(sizeof(joystickState), &joystickState)))
		{
			success = true;
		}
	}

	if (!success)
	{
		memset(&joystickState, 0, sizeof(joystickState));
		std::fill(std::begin(joystickState.rgdwPOV), std::end(joystickState.rgdwPOV), DWORD(-1));
	}
}

bool* usingGamepad = *(bool**)(0x52EA69+2);
int* moveDirection2 = *(int**)(0x51F1E2+1);

static BOOL (*orgUsingSearchCamera)();
BOOL UsingSearchCamera_SeparateAnalogs()
{
	return *usingGamepad ? FALSE : orgUsingSearchCamera();
}

// FALSE - allow search camera
// TRUE - do not allow
BOOL StartSearchCamera_Hook()
{
	if ( *usingGamepad ) return FALSE;

	return *moveDirection2 != 0 && *moveDirection2 != 3;
}

void UpdateControllerTweaks()
{
	constexpr BYTE PollDInputDevicesSearchBytes[] { 0x33, 0xDB, 0x3B, 0xC3, 0x74, 0x33, 0x8B, 0x08 };
	const DWORD PollDInputDevicesAddr = SearchAndGetAddresses(0x0045893B, 0x00458B9B, 0x00458B9B, PollDInputDevicesSearchBytes, sizeof(PollDInputDevicesSearchBytes), -0xB);
	if (PollDInputDevicesAddr == 0)
	{
		Logging::Log() << __FUNCTION__ " Error: failed to find memory address!";
		return;
	}

	dinputJoyState = *(DIJOYSTATE2**)(PollDInputDevicesAddr + 0x5A + 1);

	const DWORD HandleDInputAddr = PollDInputDevicesAddr + 0x159;

	// Shared
	int32_t jmpAddress = 0;
	memcpy( &jmpAddress, (void*)(HandleDInputAddr + 1), sizeof(jmpAddress) );

	orgProcessDInputData = decltype(orgProcessDInputData)(jmpAddress + HandleDInputAddr + 5);
	WriteCalltoMemory((BYTE*)HandleDInputAddr, ProcessDInputData_Hook);

	// Default to centered POV hats, as the game will never write to joy state if controller is not there - but it will read from it
	std::fill(std::begin(dinputJoyState->rgdwPOV), std::end(dinputJoyState->rgdwPOV), DWORD(-1));

	if (DPadMovementFix)
	{
		// If controller is unplugged while playing, clear gamepad input
		// push eax
		// call GetDeviceState_Hook
		// jmp loc_4589CA
		DWORD GetDeviceState_Addr = PollDInputDevicesAddr + 0x7F;

		BYTE pushEax[] = { 0x50 };
		UpdateMemoryAddress((void*)GetDeviceState_Addr, pushEax, sizeof(pushEax)); GetDeviceState_Addr += 1;
		WriteCalltoMemory((BYTE*)GetDeviceState_Addr, GetDeviceState_Hook); GetDeviceState_Addr += 5;

		BYTE jmpLoc[] = { 0xEB, 0x13 };
		UpdateMemoryAddress((void*)GetDeviceState_Addr, jmpLoc, sizeof(jmpLoc));
	}

	if (RestoreSearchCamMovement != 0)
	{
		// Map right stick to search camera
		float* rightStickXFloat = *(float**)(0x52E93B+2);
		float* rightStickYFloat = *(float**)(0x52E9DD+2);

		UpdateMemoryAddress((void*)(0x535CCF + 2), &rightStickXFloat, sizeof(rightStickXFloat) );
		UpdateMemoryAddress((void*)(0x535D51 + 2), &rightStickXFloat, sizeof(rightStickXFloat) );

		UpdateMemoryAddress((void*)(0x535CE2 + 2), &rightStickYFloat, sizeof(rightStickYFloat) );
		UpdateMemoryAddress((void*)(0x535D69 + 2), &rightStickYFloat, sizeof(rightStickYFloat) );

		// Allow for simultaneous walking and moving camera
		memcpy( &jmpAddress, (void*)(0x54E4B2 + 1), sizeof(jmpAddress) );
		orgUsingSearchCamera = decltype(orgUsingSearchCamera)(jmpAddress + 0x54E4B2 + 5);

		// Rotational walk
		WriteCalltoMemory((BYTE*)0x54E4B2, UsingSearchCamera_SeparateAnalogs);
		WriteCalltoMemory((BYTE*)0x54F13F, UsingSearchCamera_SeparateAnalogs);
		WriteCalltoMemory((BYTE*)0x54F209, UsingSearchCamera_SeparateAnalogs);

		// Directional walk
		WriteCalltoMemory((BYTE*)0x547FBB, UsingSearchCamera_SeparateAnalogs);
		WriteCalltoMemory((BYTE*)0x547FFA, UsingSearchCamera_SeparateAnalogs);
		WriteCalltoMemory((BYTE*)0x54807A, UsingSearchCamera_SeparateAnalogs);
		WriteCalltoMemory((BYTE*)0x548D70, UsingSearchCamera_SeparateAnalogs);
		WriteCalltoMemory((BYTE*)0x548DB2, UsingSearchCamera_SeparateAnalogs);
		WriteCalltoMemory((BYTE*)0x548DF7, UsingSearchCamera_SeparateAnalogs);
		WriteCalltoMemory((BYTE*)0x548EC6, UsingSearchCamera_SeparateAnalogs);

		const BYTE nops[] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
		WriteCalltoMemory((BYTE*)0x535CBD, StartSearchCamera_Hook);
		UpdateMemoryAddress((BYTE*)(0x535CBD + 5 + 2), nops, sizeof(nops) );
	}
}