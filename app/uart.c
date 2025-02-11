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

#if !defined(ENABLE_OVERLAY)
	#include "ARMCM0.h"
#endif
#ifdef ENABLE_FMRADIO
	#include "app/fm.h"
#endif
#include "app/uart.h"
#include "board.h"
#include "bsp/dp32g030/dma.h"
#include "bsp/dp32g030/gpio.h"
#include "driver/aes.h"
#include "driver/bk4819.h"
#include "driver/crc.h"
#include "driver/eeprom.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "functions.h"
#include "misc.h"
#include "settings.h"
#if defined(ENABLE_OVERLAY)
	#include "sram-overlay.h"
#endif
#include "version.h"
#include "ui/ui.h"

#define DMA_INDEX(x, y) (((x) + (y)) % sizeof(UART_DMA_Buffer))

typedef struct {
	uint16_t ID;
	uint16_t Size;
} __attribute__((packed)) Header_t;

typedef struct {
	uint8_t  Padding[2];
	uint16_t ID;
} __attribute__((packed)) Footer_t;

typedef struct {
	Header_t Header;
	uint32_t Timestamp;
} __attribute__((packed)) CMD_0514_t;

typedef struct {
	Header_t Header;
	struct {
		char     Version[16];
		bool     g_has_custom_aes_key;
		bool     g_is_in_lock_screen;
		uint8_t  Padding[2];
		uint32_t Challenge[4];
	} __attribute__((packed)) Data;
} __attribute__((packed)) REPLY_0514_t;

typedef struct {
	Header_t Header;
	uint16_t Offset;
	uint8_t  Size;
	uint8_t  Padding;
	uint32_t Timestamp;
} __attribute__((packed)) CMD_051B_t;

typedef struct {
	Header_t Header;
	struct {
		uint16_t Offset;
		uint8_t  Size;
		uint8_t  Padding;
		uint8_t  Data[128];
	} __attribute__((packed)) Data;
} __attribute__((packed)) REPLY_051B_t;

typedef struct {
	Header_t Header;
	uint16_t Offset;
	uint8_t  Size;
	bool     bAllowPassword;
	uint32_t Timestamp;
//	uint8_t  Data[0];
} __attribute__((packed)) CMD_051D_t;

typedef struct {
	Header_t Header;
	struct {
		uint16_t Offset;
	} __attribute__((packed)) Data;
} __attribute__((packed)) REPLY_051D_t;

typedef struct {
	Header_t Header;
	struct {
		uint16_t RSSI;
		uint8_t  ExNoiseIndicator;
		uint8_t  GlitchIndicator;
	} __attribute__((packed)) Data;
} __attribute__((packed)) REPLY_0527_t;

typedef struct {
	Header_t Header;
	struct {
		uint16_t Voltage;
		uint16_t Current;
	} __attribute__((packed)) Data;
} __attribute__((packed)) REPLY_0529_t;

typedef struct {
	Header_t Header;
	uint32_t Response[4];
} __attribute__((packed)) CMD_052D_t;

typedef struct {
	Header_t Header;
	struct {
		bool bIsLocked;
		uint8_t Padding[3];
	} __attribute__((packed)) Data;
} __attribute__((packed)) REPLY_052D_t;

typedef struct {
	Header_t Header;
	uint32_t Timestamp;
} __attribute__((packed)) CMD_052F_t;

static const uint8_t Obfuscation[16] =
{
	0x16, 0x6C, 0x14, 0xE6, 0x2E, 0x91, 0x0D, 0x40, 0x21, 0x35, 0xD5, 0x40, 0x13, 0x03, 0xE9, 0x80
};

static union
{
	uint8_t Buffer[256];
	struct
	{
		Header_t Header;
		uint8_t Data[252];
	} __attribute__((packed));
} __attribute__((packed)) UART_Command;

static uint32_t Timestamp;
static uint16_t g_uart_write_index;
static bool     bIsEncrypted = true;

static void SendReply(void *pReply, uint16_t Size)
{
	Header_t Header;
	Footer_t Footer;

	if (bIsEncrypted)
	{
		uint8_t     *pBytes = (uint8_t *)pReply;
		unsigned int i;
		for (i = 0; i < Size; i++)
			pBytes[i] ^= Obfuscation[i % 16];
	}

	Header.ID = 0xCDAB;
	Header.Size = Size;
	UART_Send(&Header, sizeof(Header));
	UART_Send(pReply, Size);

	if (bIsEncrypted)
	{
		Footer.Padding[0] = Obfuscation[(Size + 0) % 16] ^ 0xFF;
		Footer.Padding[1] = Obfuscation[(Size + 1) % 16] ^ 0xFF;
	}
	else
	{
		Footer.Padding[0] = 0xFF;
		Footer.Padding[1] = 0xFF;
	}
	Footer.ID = 0xBADC;

	UART_Send(&Footer, sizeof(Footer));
}

static void SendVersion(void)
{
	REPLY_0514_t Reply;

	Reply.Header.ID = 0x0515;
	Reply.Header.Size = sizeof(Reply.Data);
	strcpy(Reply.Data.Version, Version);
	Reply.Data.g_has_custom_aes_key = g_has_custom_aes_key;
	Reply.Data.g_is_in_lock_screen = g_is_in_lock_screen;
	Reply.Data.Challenge[0] = g_challenge[0];
	Reply.Data.Challenge[1] = g_challenge[1];
	Reply.Data.Challenge[2] = g_challenge[2];
	Reply.Data.Challenge[3] = g_challenge[3];

	SendReply(&Reply, sizeof(Reply));
}

static bool IsBadChallenge(const uint32_t *pKey, const uint32_t *pIn, const uint32_t *pResponse)
{
	unsigned int i;
	uint32_t     IV[4];

	IV[0] = 0;
	IV[1] = 0;
	IV[2] = 0;
	IV[3] = 0;

	AES_Encrypt(pKey, IV, pIn, IV, true);

	for (i = 0; i < 4; i++)
		if (IV[i] != pResponse[i])
			return true;

	return false;
}

static void CMD_0514(const uint8_t *pBuffer)
{
	const CMD_0514_t *pCmd = (const CMD_0514_t *)pBuffer;

	Timestamp = pCmd->Timestamp;

	#ifdef ENABLE_FMRADIO
		g_fm_radio_count_down_500ms = fm_radio_countdown_500ms;
	#endif

	g_serial_config_count_down_500ms = serial_config_count_down_500ms;
	
	// turn the LCD backlight off
//	GPIO_ClearBit(&GPIOB->DATA, GPIOB_PIN_BACKLIGHT);

	// show message
	g_request_display_screen = DISPLAY_MAIN;
	g_update_display = true;

	SendVersion();
}

static void CMD_051B(const uint8_t *pBuffer)
{
	const CMD_051B_t *pCmd = (const CMD_051B_t *)pBuffer;
	REPLY_051B_t      Reply;
	bool              bLocked = false;

	if (pCmd->Timestamp != Timestamp)
		return;

	g_serial_config_count_down_500ms = serial_config_count_down_500ms;

	#ifdef ENABLE_FMRADIO
		g_fm_radio_count_down_500ms = fm_radio_countdown_500ms;
	#endif

	memset(&Reply, 0, sizeof(Reply));
	Reply.Header.ID   = 0x051C;
	Reply.Header.Size = pCmd->Size + 4;
	Reply.Data.Offset = pCmd->Offset;
	Reply.Data.Size   = pCmd->Size;

	if (g_has_custom_aes_key)
		bLocked = g_is_locked;

	if (!bLocked)
		EEPROM_ReadBuffer(pCmd->Offset, Reply.Data.Data, pCmd->Size);

	SendReply(&Reply, pCmd->Size + 8);
}

static void CMD_051D(const uint8_t *pBuffer)
{
	const CMD_051D_t *pCmd = (const CMD_051D_t *)pBuffer;
	REPLY_051D_t      Reply;
	bool              bReloadEeprom;
	bool              bIsLocked;

	if (pCmd->Timestamp != Timestamp)
		return;

	g_serial_config_count_down_500ms = serial_config_count_down_500ms;
	
	bReloadEeprom = false;

	#ifdef ENABLE_FMRADIO
		g_fm_radio_count_down_500ms = fm_radio_countdown_500ms;
	#endif

	Reply.Header.ID   = 0x051E;
	Reply.Header.Size = sizeof(Reply.Data);
	Reply.Data.Offset = pCmd->Offset;

	bIsLocked = g_has_custom_aes_key ? g_is_locked : g_has_custom_aes_key;

	if (!bIsLocked)
	{
		unsigned int i;
		for (i = 0; i < (pCmd->Size / 8); i++)
		{
			const uint16_t Offset = pCmd->Offset + (i * 8U);

			if (Offset >= 0x0F30 && Offset < 0x0F40)
				if (!g_is_locked)
					bReloadEeprom = true;

			if ((Offset < 0x0E98 || Offset >= 0x0EA0) || !g_is_in_lock_screen || pCmd->bAllowPassword)
				EEPROM_WriteBuffer(Offset, (uint8_t *)&pCmd + (i * 8));  // 1of11
		}

		if (bReloadEeprom)
			BOARD_EEPROM_Init();
	}

	SendReply(&Reply, sizeof(Reply));
}

static void CMD_0527(void)
{
	REPLY_0527_t Reply;

	Reply.Header.ID             = 0x0528;
	Reply.Header.Size           = sizeof(Reply.Data);
	Reply.Data.RSSI             = BK4819_ReadRegister(BK4819_REG_67) & 0x01FF;
	Reply.Data.ExNoiseIndicator = BK4819_ReadRegister(BK4819_REG_65) & 0x007F;
	Reply.Data.GlitchIndicator  = BK4819_ReadRegister(BK4819_REG_63);

	SendReply(&Reply, sizeof(Reply));
}

static void CMD_0529(void)
{
	uint16_t     voltage;
	uint16_t     current;
	REPLY_0529_t Reply;

	Reply.Header.ID   = 0x52A;
	Reply.Header.Size = sizeof(Reply.Data);

	// Original doesn't actually send current!
	BOARD_ADC_GetBatteryInfo(&voltage, &current);
	Reply.Data.Voltage = voltage;
	Reply.Data.Current = current;

	SendReply(&Reply, sizeof(Reply));
}

static void CMD_052D(const uint8_t *pBuffer)
{
	CMD_052D_t  *pCmd = (CMD_052D_t *)pBuffer;
	REPLY_052D_t Reply;
	uint32_t     response[4];
	bool         bIsLocked;

	#ifdef ENABLE_FMRADIO
		g_fm_radio_count_down_500ms = fm_radio_countdown_500ms;
	#endif
	Reply.Header.ID   = 0x052E;
	Reply.Header.Size = sizeof(Reply.Data);

	bIsLocked = g_has_custom_aes_key;

	if (!bIsLocked)
	{
		bIsLocked = IsBadChallenge(g_custom_aes_key, g_challenge, response);
		memmove(pCmd->Response, response, sizeof(pCmd->Response));
	}
	
	if (!bIsLocked)
	{
		bIsLocked = IsBadChallenge(g_default_aes_key, g_challenge, response);
		memmove(pCmd->Response, response, sizeof(pCmd->Response));
		if (bIsLocked)
			g_try_count++;
	}

	if (g_try_count < 3)
	{
		if (!bIsLocked)
			g_try_count = 0;
	}
	else
	{
		g_try_count = 3;
		bIsLocked = true;
	}
	
	g_is_locked          = bIsLocked;
	Reply.Data.bIsLocked = bIsLocked;

	SendReply(&Reply, sizeof(Reply));
}

static void CMD_052F(const uint8_t *pBuffer)
{
	const CMD_052F_t *pCmd = (const CMD_052F_t *)pBuffer;

	g_eeprom.dual_watch                       = DUAL_WATCH_OFF;
	g_eeprom.cross_vfo_rx_tx                  = CROSS_BAND_OFF;
	g_eeprom.rx_vfo                           = 0;
	g_eeprom.dtmf_side_tone                   = false;
	g_eeprom.vfo_info[0].frequency_reverse    = false;
	g_eeprom.vfo_info[0].pRX                  = &g_eeprom.vfo_info[0].freq_config_rx;
	g_eeprom.vfo_info[0].pTX                  = &g_eeprom.vfo_info[0].freq_config_tx;
	g_eeprom.vfo_info[0].tx_offset_freq_dir   = TX_OFFSET_FREQ_DIR_OFF;
	g_eeprom.vfo_info[0].dtmf_ptt_id_tx_mode  = PTT_ID_OFF;
	g_eeprom.vfo_info[0].dtmf_decoding_enable = false;

	#ifdef ENABLE_NOAA
		g_is_noaa_mode = false;
	#endif

	if (g_current_function == FUNCTION_POWER_SAVE)
		FUNCTION_Select(FUNCTION_FOREGROUND);

	g_serial_config_count_down_500ms = serial_config_count_down_500ms;

	Timestamp = pCmd->Timestamp;

	// turn the LCD backlight off
//	GPIO_ClearBit(&GPIOB->DATA, GPIOB_PIN_BACKLIGHT);

	// show message
	g_request_display_screen = DISPLAY_MAIN;
	g_update_display = true;

	SendVersion();
}

bool UART_IsCommandAvailable(void)
{
	uint16_t Index;
	uint16_t TailIndex;
	uint16_t Size;
	uint16_t CRC;
	uint16_t CommandLength;
	uint16_t DmaLength = DMA_CH0->ST & 0xFFFU;

	while (1)
	{
		if (g_uart_write_index == DmaLength)
			return false;

		while (g_uart_write_index != DmaLength && UART_DMA_Buffer[g_uart_write_index] != 0xABU)
			g_uart_write_index = DMA_INDEX(g_uart_write_index, 1);

		if (g_uart_write_index == DmaLength)
			return false;

		if (g_uart_write_index < DmaLength)
			CommandLength = DmaLength - g_uart_write_index;
		else
			CommandLength = (DmaLength + sizeof(UART_DMA_Buffer)) - g_uart_write_index;

		if (CommandLength < 8)
			return 0;

		if (UART_DMA_Buffer[DMA_INDEX(g_uart_write_index, 1)] == 0xCD)
			break;

		g_uart_write_index = DMA_INDEX(g_uart_write_index, 1);
	}

	Index = DMA_INDEX(g_uart_write_index, 2);
	Size  = (UART_DMA_Buffer[DMA_INDEX(Index, 1)] << 8) | UART_DMA_Buffer[Index];

	if ((Size + 8u) > sizeof(UART_DMA_Buffer))
	{
		g_uart_write_index = DmaLength;
		return false;
	}

	if (CommandLength < (Size + 8))
		return false;

	Index     = DMA_INDEX(Index, 2);
	TailIndex = DMA_INDEX(Index, Size + 2);

	if (UART_DMA_Buffer[TailIndex] != 0xDC || UART_DMA_Buffer[DMA_INDEX(TailIndex, 1)] != 0xBA)
	{
		g_uart_write_index = DmaLength;
		return false;
	}

	if (TailIndex < Index)
	{
		const uint16_t ChunkSize = sizeof(UART_DMA_Buffer) - Index;
		memmove(UART_Command.Buffer, UART_DMA_Buffer + Index, ChunkSize);
		memmove(UART_Command.Buffer + ChunkSize, UART_DMA_Buffer, TailIndex);
	}
	else
		memmove(UART_Command.Buffer, UART_DMA_Buffer + Index, TailIndex - Index);

	TailIndex = DMA_INDEX(TailIndex, 2);
	if (TailIndex < g_uart_write_index)
	{
		memset(UART_DMA_Buffer + g_uart_write_index, 0, sizeof(UART_DMA_Buffer) - g_uart_write_index);
		memset(UART_DMA_Buffer, 0, TailIndex);
	}
	else
		memset(UART_DMA_Buffer + g_uart_write_index, 0, TailIndex - g_uart_write_index);

	g_uart_write_index = TailIndex;

	if (UART_Command.Header.ID == 0x0514)
		bIsEncrypted = false;

	if (UART_Command.Header.ID == 0x6902)
		bIsEncrypted = true;

	if (bIsEncrypted)
	{
		unsigned int i;
		for (i = 0; i < (Size + 2u); i++)
			UART_Command.Buffer[i] ^= Obfuscation[i % 16];
	}
	
	CRC = UART_Command.Buffer[Size] | (UART_Command.Buffer[Size + 1] << 8);

	return (CRC_Calculate(UART_Command.Buffer, Size) != CRC) ? false : true;
}

void UART_HandleCommand(void)
{
	switch (UART_Command.Header.ID)
	{
		case 0x0514:
			CMD_0514(UART_Command.Buffer);
			break;
	
		case 0x051B:
			CMD_051B(UART_Command.Buffer);
			break;
	
		case 0x051D:
			CMD_051D(UART_Command.Buffer);
			break;
	
		case 0x051F:	// Not implementing non-authentic command
			break;
	
		case 0x0521:	// Not implementing non-authentic command
			break;
	
		case 0x0527:
			CMD_0527();
			break;
	
		case 0x0529:
			CMD_0529();
			break;
	
		case 0x052D:
			CMD_052D(UART_Command.Buffer);
			break;
	
		case 0x052F:
			CMD_052F(UART_Command.Buffer);
			break;
	
		case 0x05DD:
			#if defined(ENABLE_OVERLAY)
				overlay_FLASH_RebootToBootloader();
			#else
				NVIC_SystemReset();
			#endif
			break;
	}
}
