/* Copyright 2023 Dual Tachyon
 * https://github.com/DualTachyon
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

#include <string.h>

#include "app/app.h"
#ifdef ENABLE_FMRADIO
	#include "app/fm.h"
#endif
#include "app/generic.h"
#include "app/menu.h"
#include "app/scanner.h"
#include "audio.h"
#include "driver/keyboard.h"
#include "dtmf.h"
#include "external/printf/printf.h"
#include "functions.h"
#include "misc.h"
#include "settings.h"
#include "ui/inputbox.h"
#include "ui/ui.h"

void GENERIC_Key_F(bool bKeyPressed, bool bKeyHeld)
{
	if (gInputBoxIndex > 0)
	{
		if (!bKeyHeld && bKeyPressed)
			gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
		return;
	}

	if (bKeyHeld || !bKeyPressed)
	{
		if (bKeyHeld || bKeyPressed)
		{
			if (!bKeyHeld)
				return;

			if (!bKeyPressed)
				return;

			if (gScreenToDisplay != DISPLAY_MENU &&
			    gScreenToDisplay != DISPLAY_FM &&
			    #ifdef ENABLE_FMRADIO
					!gFmRadioMode &&
			    #endif
			    gCurrentFunction != FUNCTION_TRANSMIT)
			{	// toggle the keyboad lock

				#ifdef ENABLE_VOICE
					g_another_voice_id = g_eeprom.key_lock ? VOICE_ID_UNLOCK : VOICE_ID_LOCK;
				#endif

				g_eeprom.key_lock = !g_eeprom.key_lock;

				gRequestSaveSettings = true;
			}
		}
		else
		{
			#ifdef ENABLE_FMRADIO
				if ((gFmRadioMode || gScreenToDisplay != DISPLAY_MAIN) && gScreenToDisplay != DISPLAY_FM)
					return;
			#else
				if (gScreenToDisplay != DISPLAY_MAIN)
					return;
			#endif

			g_was_f_key_pressed = !g_was_f_key_pressed;

			if (g_was_f_key_pressed)
				gKeyInputCountdown = key_input_timeout_500ms;

			#ifdef ENABLE_VOICE
				if (!g_was_f_key_pressed)
					g_another_voice_id = VOICE_ID_CANCEL;
			#endif

			gUpdateStatus = true;
		}
	}
	else
	{
		if (gScreenToDisplay != DISPLAY_FM)
		{
			gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
			return;
		}

		#ifdef ENABLE_FMRADIO
			if (gFM_ScanState == FM_SCAN_OFF)
			{
				gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
				return;
			}
		#endif

		gBeepToPlay     = BEEP_440HZ_500MS;

		gPttWasReleased = true;
	}
}

void GENERIC_Key_PTT(bool bKeyPressed)
{
	gInputBoxIndex = 0;

	if (!bKeyPressed || gSerialConfigCountDown_500ms > 0)
	{	// PTT released

		if (gCurrentFunction == FUNCTION_TRANSMIT)
		{	// we are transmitting .. stop

			if (gFlagEndTransmission)
			{
				FUNCTION_Select(FUNCTION_FOREGROUND);
			}
			else
			{
				APP_EndTransmission();

				if (g_eeprom.repeater_tail_tone_elimination == 0)
					FUNCTION_Select(FUNCTION_FOREGROUND);
				else
					gRTTECountdown = g_eeprom.repeater_tail_tone_elimination * 10;
			}

			gFlagEndTransmission = false;

			#ifdef ENABLE_VOX
				gVOX_NoiseDetected = false;
			#endif

			RADIO_SetVfoState(VFO_STATE_NORMAL);

			if (gScreenToDisplay != DISPLAY_MENU)     // 1of11 .. don't close the menu
				gRequestDisplayScreen = DISPLAY_MAIN;
		}

		return;
	}

	// PTT pressed

	if (gScanStateDir != SCAN_OFF ||             // frequency/channel scanning
	    gScreenToDisplay == DISPLAY_SCANNER ||   // CTCSS/CDCSS scanning
	    gCssScanMode != CSS_SCAN_MODE_OFF)       //   "     "
	{	// we're scanning .. stop

		if (gScreenToDisplay == DISPLAY_SCANNER)
		{	// CTCSS/CDCSS scanning .. stop
			g_eeprom.cross_vfo_rx_tx = gBackup_cross_vfo_rx_tx;
			gFlagStopScan            = true;
			gVfoConfigureMode        = VFO_CONFIGURE_RELOAD;
			gFlagResetVfos           = true;
		}
		else
		if (gScanStateDir != SCAN_OFF)
		{	// frequency/channel scanning . .stop
			SCANNER_Stop();
		}
		else
		if (gCssScanMode != CSS_SCAN_MODE_OFF)
		{	// CTCSS/CDCSS scanning .. stop
			MENU_StopCssScan();

			#ifdef ENABLE_VOICE
				g_another_voice_id = VOICE_ID_SCANNING_STOP;
			#endif
		}

		goto cancel_tx;
	}

	#ifdef ENABLE_FMRADIO
		if (gFM_ScanState != FM_SCAN_OFF)
		{	// FM radio is scanning .. stop
			FM_PlayAndUpdate();
			#ifdef ENABLE_VOICE
				g_another_voice_id = VOICE_ID_SCANNING_STOP;
			#endif
			gRequestDisplayScreen = DISPLAY_FM;
			goto cancel_tx;
		}
	#endif

	#ifdef ENABLE_FMRADIO
		if (gScreenToDisplay == DISPLAY_FM)
			goto start_tx;	// listening to the FM radio .. start TX'ing
	#endif

	if (gCurrentFunction == FUNCTION_TRANSMIT && gRTTECountdown == 0)
	{	// already transmitting
		gInputBoxIndex = 0;
		return;
	}

	if (gScreenToDisplay != DISPLAY_MENU)     // 1of11 .. don't close the menu
		gRequestDisplayScreen = DISPLAY_MAIN;

	if (!gDTMF_InputMode && gDTMF_InputBox_Index == 0)
		goto start_tx;	// wasn't entering a DTMF code .. start TX'ing (maybe)

	// was entering a DTMF string

	if (gDTMF_InputBox_Index > 0 || gDTMF_PreviousIndex > 0)
	{	// going to transmit a DTMF string

		if (gDTMF_InputBox_Index == 0 && gDTMF_PreviousIndex > 0)
			gDTMF_InputBox_Index = gDTMF_PreviousIndex;           // use the previous DTMF string

		if (gDTMF_InputBox_Index < sizeof(gDTMF_InputBox))
			gDTMF_InputBox[gDTMF_InputBox_Index] = 0;             // NULL term the string

		#if 0
			// append our DTMF ID to the inputted DTMF code -
			//  IF the user inputted code is exactly 3 digits long
			if (gDTMF_InputBox_Index == 3)
				gDTMF_CallMode = DTMF_CheckGroupCall(gDTMF_InputBox, 3);
			else
				gDTMF_CallMode = DTMF_CALL_MODE_DTMF;
		#else
			// append our DTMF ID to the inputted DTMF code -
			//  IF the user inputted code is exactly 3 digits long and D-DCD is enabled
			if (gDTMF_InputBox_Index == 3 && gTxVfo->DTMF_decoding_enable > 0)
				gDTMF_CallMode = DTMF_CheckGroupCall(gDTMF_InputBox, 3);
			else
				gDTMF_CallMode = DTMF_CALL_MODE_DTMF;
		#endif

		// remember the DTMF string
		gDTMF_PreviousIndex = gDTMF_InputBox_Index;
		strcpy(gDTMF_String, gDTMF_InputBox);

		gDTMF_ReplyState = DTMF_REPLY_ANI;
		gDTMF_State      = DTMF_STATE_0;
	}

	DTMF_clear_input_box();

start_tx:
	// request start TX
	gFlagPrepareTX = true;
	goto done;
	
cancel_tx:
	if (gPttIsPressed)
	{
		gPttIsPressed  = false;
		gPttWasPressed = true;
	}

done:	
	gPttDebounceCounter = 0;
	if (gScreenToDisplay != DISPLAY_MENU && gRequestDisplayScreen != DISPLAY_FM)     // 1of11 .. don't close the menu
		gRequestDisplayScreen = DISPLAY_MAIN;
	gUpdateStatus  = true;
	gUpdateDisplay = true;
}
