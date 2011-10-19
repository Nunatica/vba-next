/******************************************************************************* 
 * emu-ps3.cpp - VBANext PS3
 *
 *  Created on: May 4, 2011
********************************************************************************/

#include <sys/timer.h>
#include <sys/return_code.h>
#include <sys/process.h>
#include <sys/spu_initialize.h>
#include <sysutil/sysutil_sysparam.h>
#include <sysutil/sysutil_msgdialog.h>

#include <cell/sysmodule.h>
#include <cell/cell_fs.h>
#include <string>
#include "sys/types.h"

#include "../../../src/vba/gba/GBA.h"
#include "../../../src/vba/gba/Sound.h"
#include "../../../src/vba/gba/RTC.h"
#ifdef USE_AGBPRINT
#include "../../../src/vba/gba/agbprint.h"
#endif
#include "../../../src/vba/gb/gb.h"
#include "../../../src/vba/gb/gbSound.h"
#include "../../../src/vba/gb/gbGlobals.h"
#include "../../../src/vba/gba/Globals.h"
#include "emu-ps3.hpp"

#include "menu.hpp"

#include "conf/config_file.h"
#include "conf/settings.h"

#include "../../../src/vba/common/memgzio.h"

#ifdef PS3_PROFILING
#include "cellframework/network-stdio/net_stdio.h"
#endif

#include <ppu_intrinsics.h>

SYS_PROCESS_PARAM(1001, 0x40000);

char contentInfoPath[MAX_PATH_LENGTH];
char usrDirPath[MAX_PATH_LENGTH];
char DEFAULT_PRESET_FILE[MAX_PATH_LENGTH];
char DEFAULT_BORDER_FILE[MAX_PATH_LENGTH];
char DEFAULT_MENU_BORDER_FILE[MAX_PATH_LENGTH];
char GAME_AWARE_SHADER_DIR_PATH[MAX_PATH_LENGTH];
char PRESETS_DIR_PATH[MAX_PATH_LENGTH];
char BORDERS_DIR_PATH[MAX_PATH_LENGTH];
char SHADERS_DIR_PATH[MAX_PATH_LENGTH];
char DEFAULT_SHADER_FILE[MAX_PATH_LENGTH];
char SYS_CONFIG_FILE[MAX_PATH_LENGTH];
char DEFAULT_MENU_SHADER_FILE[MAX_PATH_LENGTH];
char MULTIMAN_GAME_TO_BOOT[MAX_PATH_LENGTH];

PS3Graphics* Graphics = NULL;
cell_audio_handle_t audio_handle;
const struct cell_audio_driver *audio_driver = &cell_audio_audioport;
oskutil_params oskutil_handle;
PS3InputList PS3Input;

struct SSettings Settings;

int mode_switch = MODE_MENU;

static uint32_t is_running;			// is the ROM currently running in the emulator?
static bool is_ingame_menu_running;		// is the ingame menu currently running?
static bool return_to_MM = false;		// launch multiMAN on exit if ROM is passed
static bool need_load_rom = false;		// need to load the current ROM
static char * current_rom;			// filename of the current ROM being emulated
bool dialog_is_running;
static uint64_t ingame_menu_item = 0;

/* Emulator-specific variables */

int srcWidth, srcHeight;
static bool sgb_border_change = false;
uint32_t reinit_video = 0;
static int gbaRomSize = 0;

bool audio_net;
bool audio_blocking;

int RGB_LOW_BITS_MASK = 0;

uint16_t systemColorMap16[0x10000];
uint32_t systemColorMap32[0x10000];
uint16_t systemGbPalette[24];
int systemRedShift = 0;
int systemBlueShift = 0;
int systemGreenShift = 0;
#if defined(USE_GBA_FILTERS)
int systemColorDepth = 32;
#endif
int systemDebug = 0;
int systemVerbose = 0;
int systemFrameSkip = 0;
int systemSaveUpdateCounter = SYSTEM_SAVE_NOT_UPDATED;
int systemSpeed = 0;

void (*dbgOutput)(const char *s, uint32_t addr);
void (*dbgSignal)(int sig,int number);

extern void get_zipfilename(char * filename, char * outfilename);

//FIXME - Replace this with macro equivalent
static const char * MakeFName(int type)
{
	// strip out the filename from the path
	std::string fn = current_rom;
	fn = fn.substr(0, fn.find_last_of('.'));
	fn = fn.substr(fn.find_last_of('/')+1);

	char ss[MAX_PATH_LENGTH];
	switch (type)
	{
		case FILETYPE_STATE:
			snprintf(ss, sizeof(ss), "%s/%s.%d.sgm", Settings.PS3PathSaveStates, fn.c_str(), Settings.CurrentSaveStateSlot);
			break;
		case FILETYPE_BATTERY:
			snprintf(ss, sizeof(ss), "%s/%s.sav", Settings.PS3PathSRAM, fn.c_str());
			break;
		case FILETYPE_IMAGE_PREFS:
			snprintf(ss, sizeof(ss), "%s/vba-over.ini", usrDirPath);
			break;
	}

	return ss;
}

#define emulator_decrement_current_save_state_slot()  \
	if (Settings.CurrentSaveStateSlot > 0) \
	{ \
		Settings.CurrentSaveStateSlot -= 1; \
	} \
	snprintf(special_action_msg, sizeof(special_action_msg), "Save state slot changed to: #%d", Settings.CurrentSaveStateSlot); \
	special_action_msg_expired = Graphics->SetTextMessageSpeed();

#define emulator_increment_current_save_state_slot() \
	Settings.CurrentSaveStateSlot++; \
	snprintf(special_action_msg, sizeof(special_action_msg), "Save state slot changed to: #%d", Settings.CurrentSaveStateSlot); \
	special_action_msg_expired = Graphics->SetTextMessageSpeed();

#define emulator_load_current_save_state_slot(loadfunction) \
	int ret  = loadfunction(MakeFName(FILETYPE_STATE)); \
		if(ret) \
			snprintf(special_action_msg, sizeof(special_action_msg), "Loaded save state slot #%d", Settings.CurrentSaveStateSlot); \
		else \
			snprintf(special_action_msg, sizeof(special_action_msg), "Can't load from save state slot #%d", Settings.CurrentSaveStateSlot);

#define emulator_save_current_save_state_slot(savefunction) \
	int ret  = savefunction(MakeFName(FILETYPE_STATE)); \
		if(ret) \
			snprintf(special_action_msg, sizeof(special_action_msg), "Saved to save state slot #%d", Settings.CurrentSaveStateSlot); \
		else \
			snprintf(special_action_msg, sizeof(special_action_msg), "Can't save to save state slot #%d", Settings.CurrentSaveStateSlot);


extern void log(const char * fmt,...)
{
	va_list args;
	va_start(args,fmt);
	va_end(args);
}

void system_init()
{
	// Build GBPalette
	int i = 0;
	for( i = 0; i < 24; )
	{
		systemGbPalette[i++] = (0x1f) | (0x1f << 5) | (0x1f << 10);
		systemGbPalette[i++] = (0x15) | (0x15 << 5) | (0x15 << 10);
		systemGbPalette[i++] = (0x0c) | (0x0c << 5) | (0x0c << 10);
		systemGbPalette[i++] = 0;
	}

	//systemColorDepth = 32;

	// GL_RGBA
	//systemRedShift    = 27;
	//systemGreenShift  = 19;
	//systemBlueShift   = 11;

	// GL_ARGB
	systemRedShift    = 19;
	systemGreenShift  = 11;
	systemBlueShift   = 3;

	// FIXME: add systemColorDepth = 16 shifts one day

	// VBA - used by the cpu filters only, not needed really
	//RGB_LOW_BITS_MASK = 0x00010101;

	utilUpdateSystemColorMaps(Settings.EmulatedSystem == IMAGE_GBA && gbColorOption == 1);
}

void systemGbPrint(uint8_t *data,int pages,int feed,int palette, int contrast)
{
}

// updates the joystick data

static void ingame_menu_enable (int enable)
{
	is_running = false;
	is_ingame_menu_running = enable;
}


#define mainInputloop(pad, old_state) \
	__dcbt(&PS3Input); \
	\
	const uint64_t state = cell_pad_input_poll_device(pad); \
	const uint64_t button_was_pressed = old_state & (old_state ^ state); \
	const uint64_t button_was_not_held = ~(old_state & state); \
	const uint64_t button_was_not_pressed = ~(state); \
	\
	uint64_t J = 0; \
	\
	/* D-Pad Up + Left Stick Up */ \
	\
	int32_t ctrl_up_condition = ((CTRL_UP(state) || CTRL_LSTICK_UP(state)) | -(CTRL_UP(state) || CTRL_LSTICK_UP(state))) >> 31; \
	uint32_t  ctrl_up_result = (((J | PS3Input.DPad_Up[pad]) & ctrl_up_condition) | ((((J) & ~(ctrl_up_condition))))); \
	\
	J = ctrl_up_result; \
	\
	/* D-Pad Down + Left Stick Down */ \
	\
	int32_t ctrl_down_condition = ((CTRL_DOWN(state) || CTRL_LSTICK_DOWN(state)) | -(CTRL_DOWN(state) || CTRL_LSTICK_DOWN(state))) >> 31; \
	uint32_t  ctrl_down_result = (((J | PS3Input.DPad_Down[pad]) & ctrl_down_condition) | ((((J) & ~(ctrl_down_condition))))); \
	\
	J = ctrl_down_result; \
	\
	/* D-Pad Left + Left Stick Left */ \
	\
	int32_t ctrl_left_condition = ((CTRL_LEFT(state) || CTRL_LSTICK_LEFT(state)) | -(CTRL_LEFT(state) || CTRL_LSTICK_LEFT(state))) >> 31; \
	uint32_t  ctrl_left_result = (((J | PS3Input.DPad_Left[pad]) & ctrl_left_condition) | ((((J) & ~(ctrl_left_condition))))); \
	\
	J = ctrl_left_result; \
	\
	/* D-Pad Right + Left Stick Right */ \
	\
	int32_t ctrl_right_condition = ((CTRL_RIGHT(state) || CTRL_LSTICK_RIGHT(state)) | -(CTRL_RIGHT(state) || CTRL_LSTICK_RIGHT(state))) >> 31; \
	uint32_t  ctrl_right_result = ((((J | PS3Input.DPad_Right[pad]) & ctrl_right_condition) | ((((J) & ~(ctrl_right_condition)))))); \
	\
	J = ctrl_right_result; \
	\
	/* SQUARE button */ \
	\
	int32_t ctrl_square_condition = ((CTRL_SQUARE(state)) | -(CTRL_SQUARE(state))) >> 31; \
	uint32_t  ctrl_square_result = ((((J | PS3Input.ButtonSquare[pad]) & ctrl_square_condition) | ((((J) & ~(ctrl_square_condition)))))); \
	\
	J = ctrl_square_result; \
	\
	/* CIRCLE button */ \
	\
	int32_t ctrl_circle_condition = ((CTRL_CIRCLE(state)) | -(CTRL_CIRCLE(state))) >> 31; \
	uint32_t  ctrl_circle_result = ((((J | PS3Input.ButtonCircle[pad]) & ctrl_circle_condition) | ((((J) & ~(ctrl_circle_condition)))))); \
	\
	J = ctrl_circle_result; \
	\
	/* CROSS button */ \
	\
	int32_t ctrl_cross_condition = ((CTRL_CROSS(state)) | -(CTRL_CROSS(state))) >> 31; \
	uint32_t  ctrl_cross_result = ((((J | PS3Input.ButtonCross[pad]) & ctrl_cross_condition) | ((((J) & ~(ctrl_cross_condition)))))); \
	\
	J = ctrl_cross_result; \
	\
	/* TRIANGLE button */ \
	\
	int32_t ctrl_triangle_condition = ((CTRL_TRIANGLE(state)) | -(CTRL_TRIANGLE(state))) >> 31; \
	uint32_t  ctrl_triangle_result = ((((J | PS3Input.ButtonTriangle[pad]) & ctrl_triangle_condition) | ((((J) & ~(ctrl_triangle_condition)))))); \
	\
	J = ctrl_triangle_result; \
	\
	/* L1 button */ \
	\
	int32_t ctrl_l1_condition = ((CTRL_L1(state)) | -(CTRL_L1(state))) >> 31; \
	uint32_t  ctrl_l1_result = ((((J | PS3Input.ButtonL1[pad]) & ctrl_l1_condition) | ((((J) & ~(ctrl_l1_condition)))))); \
	\
	J = ctrl_l1_result; \
	\
	/* R1 button */ \
	\
	int32_t ctrl_r1_condition = ((CTRL_R1(state)) | -(CTRL_R1(state))) >> 31; \
	uint32_t  ctrl_r1_result = ((((J | PS3Input.ButtonR1[pad]) & ctrl_r1_condition) | ((((J) & ~(ctrl_r1_condition)))))); \
	\
	J = ctrl_r1_result; \
	\
	/* START button */ \
	\
	int32_t ctrl_start_condition = ((CTRL_START(state)) | -(CTRL_START(state))) >> 31; \
	uint32_t  ctrl_start_result = ((((J | PS3Input.ButtonStart[pad]) & ctrl_start_condition) | ((((J) & ~(ctrl_start_condition)))))); \
	\
	J = ctrl_start_result; \
	\
	/* SELECT button */ \
	\
	int32_t ctrl_select_condition = ((CTRL_SELECT(state)) | -(CTRL_SELECT(state))) >> 31; \
	uint32_t  ctrl_select_result = ((((J | PS3Input.ButtonSelect[pad]) & ctrl_select_condition) | ((((J) & ~(ctrl_select_condition)))))); \
	\
	J = ctrl_select_result; \
	\
	/* L2 + R3 */ \
	\
	int32_t ctrl_l2_r3_condition = ((CTRL_L2(state) && CTRL_R3(state)) | -(CTRL_L2(state) && CTRL_R3(state))) >> 31; \
	uint32_t  ctrl_l2_r3_result = ((((J | PS3Input.ButtonL2_ButtonR3[pad]) & ctrl_l2_r3_condition) | ((((J) & ~(ctrl_l2_r3_condition)))))); \
	\
	J = ctrl_l2_r3_result; \
	\
	/* L2 + R2 */ \
	\
	int32_t ctrl_l2_r2_condition = ((CTRL_L2(state) && CTRL_R2(state)) | -(CTRL_L2(state) && CTRL_R2(state))) >> 31; \
	uint32_t  ctrl_l2_r2_result = ((((J | PS3Input.ButtonL2_ButtonR2[pad]) & ctrl_l2_r2_condition) | ((((J) & ~(ctrl_l2_r2_condition)))))); \
	\
	J = ctrl_l2_r2_result; \
	\
	/* L2 + L3 */ \
	\
	int32_t ctrl_l2_l3_condition = ((CTRL_L2(state) && CTRL_L3(state)) | -(CTRL_L2(state) && CTRL_L3(state))) >> 31; \
	uint32_t  ctrl_l2_l3_result = ((((J | PS3Input.ButtonL2_ButtonL3[pad]) & ctrl_l2_l3_condition) | ((((J) & ~(ctrl_l2_l3_condition)))))); \
	\
	J = ctrl_l2_l3_result; \
	\
	/* L2 */ \
	\
	int32_t ctrl_l2_condition = ((CTRL_L2(state) && CTRL_R2(button_was_not_pressed)) | -(CTRL_L2(state) && CTRL_R2(button_was_not_pressed))) >> 31; \
	uint32_t  ctrl_l2_result = ((((J | PS3Input.ButtonL2[pad]) & ctrl_l2_condition) | ((((J) & ~(ctrl_l2_condition)))))); \
	\
	J = ctrl_l2_result; \
	\
	/* L3 + R3 */ \
	\
	int32_t ctrl_l3_r3_condition = ((CTRL_L3(state) && CTRL_R3(state)) | -(CTRL_L3(state) && CTRL_R3(state))) >> 31; \
	uint32_t  ctrl_l3_r3_result = ((((J | PS3Input.ButtonR3_ButtonL3[pad]) & ctrl_l3_r3_condition) | ((((J) & ~(ctrl_l3_r3_condition)))))); \
	\
	J = ctrl_l3_r3_result; \
	\
	/* L3 */ \
	int32_t ctrl_l3_condition = ((CTRL_L3(state) && CTRL_R3(button_was_not_pressed) && CTRL_L2(button_was_not_pressed) && CTRL_R2(button_was_not_pressed)) | -(CTRL_L3(state) && CTRL_R3(button_was_not_pressed) && CTRL_L2(button_was_not_pressed) && CTRL_R2(button_was_not_pressed))) >> 31; \
	uint32_t  ctrl_l3_result = ((((J | PS3Input.ButtonL3[pad]) & ctrl_l3_condition) | ((((J) & ~(ctrl_l3_condition)))))); \
	\
	J = ctrl_l3_result; \
	\
	/* R3 */ \
	\
	int32_t ctrl_r3_condition = ((CTRL_R3(state) && CTRL_L3(button_was_not_pressed) && CTRL_L2(button_was_not_pressed) && CTRL_R2(button_was_not_pressed)) | -(CTRL_R3(state) && CTRL_L3(button_was_not_pressed) && CTRL_L2(button_was_not_pressed) && CTRL_R2(button_was_not_pressed))) >> 31; \
	uint32_t  ctrl_r3_result = ((((J | PS3Input.ButtonR3[pad]) & ctrl_r3_condition) | ((((J) & ~(ctrl_r3_condition)))))); \
	\
	J = ctrl_r3_result; \
	\
	/* R2 */ \
	\
	int32_t ctrl_r2_condition = ((CTRL_R2(state) && CTRL_L2(button_was_not_held)) | -(CTRL_R2(state) && CTRL_L2(button_was_not_held))) >> 31; \
	uint32_t  ctrl_r2_result = ((((J | PS3Input.ButtonR2[pad]) & ctrl_r2_condition) | ((((J) & ~(ctrl_r2_condition)))))); \
	\
	J = ctrl_r2_result; \
	\
	/* R2 + R3 */ \
	\
	int32_t ctrl_r2_r3_condition = ((CTRL_R2(state) && CTRL_R3(state)) | -(CTRL_R2(state) && CTRL_R3(state))) >> 31; \
	uint32_t  ctrl_r2_r3_result = ((((J | PS3Input.ButtonR2_ButtonR3[pad]) & ctrl_r2_r3_condition) | ((((J) & ~(ctrl_r2_r3_condition)))))); \
	\
	J = ctrl_r2_r3_result; \
	\
	/* L2 + Rstick Up */ \
	\
	int32_t ctrl_l2_rstick_up_condition = ((CTRL_L2(state) && CTRL_RSTICK_UP(button_was_pressed)) | -(CTRL_L2(state) && CTRL_RSTICK_UP(button_was_pressed))) >> 31; \
	uint32_t  ctrl_l2_rstick_up_result = ((((J | PS3Input.ButtonL2_AnalogR_Up[pad]) & ctrl_l2_rstick_up_condition) | ((((J) & ~(ctrl_l2_rstick_up_condition)))))); \
	\
	J = ctrl_l2_rstick_up_result; \
	\
	/* L2 + Rstick Down */ \
	\
	int32_t ctrl_l2_rstick_down_condition = ((CTRL_L2(state) && CTRL_RSTICK_DOWN(button_was_pressed)) | -(CTRL_L2(state) && CTRL_RSTICK_DOWN(button_was_pressed))) >> 31; \
	uint32_t  ctrl_l2_rstick_down_result = ((((J | PS3Input.ButtonL2_AnalogR_Down[pad]) & ctrl_l2_rstick_down_condition) | ((((J) & ~(ctrl_l2_rstick_down_condition)))))); \
	\
	J = ctrl_l2_rstick_down_result; \
	\
	/* L2 + Rstick Left */ \
	\
	int32_t ctrl_l2_rstick_left_condition = ((CTRL_L2(state) && CTRL_RSTICK_LEFT(button_was_pressed)) | -(CTRL_L2(state) && CTRL_RSTICK_LEFT(button_was_pressed))) >> 31; \
	uint32_t  ctrl_l2_rstick_left_result = ((((J | PS3Input.ButtonL2_AnalogR_Left[pad]) & ctrl_l2_rstick_left_condition) | ((((J) & ~(ctrl_l2_rstick_left_condition)))))); \
	\
	J = ctrl_l2_rstick_left_result; \
	\
	/* L2 + Rstick Right */ \
	\
	int32_t ctrl_l2_rstick_right_condition = ((CTRL_L2(state) && CTRL_RSTICK_RIGHT(button_was_pressed)) | -(CTRL_L2(state) && CTRL_RSTICK_RIGHT(button_was_pressed))) >> 31; \
	uint32_t  ctrl_l2_rstick_right_result = ((((J | PS3Input.ButtonL2_AnalogR_Right[pad]) & ctrl_l2_rstick_right_condition) | ((((J) & ~(ctrl_l2_rstick_right_condition)))))); \
	\
	J = ctrl_l2_rstick_right_result; \
	\
	/* R2 + Rstick Up */ \
	\
	int32_t ctrl_r2_rstick_up_condition = ((CTRL_R2(state) && CTRL_RSTICK_UP(button_was_pressed)) | -(CTRL_R2(state) && CTRL_RSTICK_UP(button_was_pressed))) >> 31; \
	uint32_t  ctrl_r2_rstick_up_result = ((((J | PS3Input.ButtonR2_AnalogR_Up[pad]) & ctrl_r2_rstick_up_condition) | ((((J) & ~(ctrl_r2_rstick_up_condition)))))); \
	\
	J = ctrl_r2_rstick_up_result; \
	\
	/* R2 + Rstick Down */ \
	\
	int32_t ctrl_r2_rstick_down_condition = ((CTRL_R2(state) && CTRL_RSTICK_DOWN(button_was_pressed)) | -(CTRL_R2(state) && CTRL_RSTICK_DOWN(button_was_pressed))) >> 31; \
	uint32_t  ctrl_r2_rstick_down_result = ((((J | PS3Input.ButtonR2_AnalogR_Down[pad]) & ctrl_r2_rstick_down_condition) | ((((J) & ~(ctrl_r2_rstick_down_condition)))))); \
	\
	J = ctrl_r2_rstick_down_result; \
	\
	/* R2 + Rstick Left */ \
	\
	int32_t ctrl_r2_rstick_left_condition = ((CTRL_R2(state) && CTRL_RSTICK_LEFT(button_was_pressed)) | -(CTRL_R2(state) && CTRL_RSTICK_LEFT(button_was_pressed))) >> 31; \
	uint32_t  ctrl_r2_rstick_left_result = ((((J | PS3Input.ButtonR2_AnalogR_Left[pad]) & ctrl_r2_rstick_left_condition) | ((((J) & ~(ctrl_r2_rstick_left_condition)))))); \
	\
	J = ctrl_r2_rstick_left_result; \
	\
	/* R2 + Rstick Right */ \
	\
	int32_t ctrl_r2_rstick_right_condition = ((CTRL_R2(state) && CTRL_RSTICK_RIGHT(button_was_pressed)) | -(CTRL_R2(state) && CTRL_RSTICK_RIGHT(button_was_pressed))) >> 31; \
	uint32_t  ctrl_r2_rstick_right_result = ((((J | PS3Input.ButtonR2_AnalogR_Right[pad]) & ctrl_r2_rstick_right_condition) | ((((J) & ~(ctrl_r2_rstick_right_condition)))))); \
	\
	J = ctrl_r2_rstick_right_result;\
	/* Analog Right Stick - Up */

#define extra_buttons_gb(numplayer) \
{ \
	if(J < BTN_QUICKLOAD-1) \
	gbJoymask[numplayer] = J; \
	else \
	{ \
		if(J & BTN_QUICKLOAD) \
		{ \
			emulator_load_current_save_state_slot(gbReadSaveState); \
		} \
		if(J & BTN_QUICKSAVE) \
		{ \
			emulator_save_current_save_state_slot(gbWriteSaveState); \
		} \
		if(J  & BTN_EXITTOMENU) \
		{ \
			gbWriteBatteryFile(MakeFName(FILETYPE_BATTERY)); \
			is_running = 0; \
			mode_switch = MODE_MENU; \
		} \
		\
		if(J & BTN_DECREMENTSAVE) \
		{ \
			emulator_decrement_current_save_state_slot(); \
		} \
		\
		if(J & BTN_INCREMENTSAVE) \
		{ \
			emulator_increment_current_save_state_slot(); \
		} \
		if(J & BTN_INGAME_MENU) \
		{ \
			ingame_menu_enable(1); \
			is_running = 0; \
			mode_switch = MODE_MENU; \
		} \
	} \
}

#define extra_buttons(buttonarray) \
	if(J < BTN_QUICKLOAD-1) \
		buttonarray = J; \
	else \
	{ \
		if(J & BTN_QUICKLOAD) \
		{ \
			emulator_load_current_save_state_slot(CPUReadState); \
		} \
		if(J & BTN_QUICKSAVE) \
		{ \
			emulator_save_current_save_state_slot(CPUWriteState); \
		} \
		if(J & BTN_EXITTOMENU) \
		{ \
			CPUWriteBatteryFile(MakeFName(FILETYPE_BATTERY)); \
			is_running = 0; \
			mode_switch = MODE_MENU; \
		} \
		\
		if(J & BTN_DECREMENTSAVE) \
		{ \
			emulator_decrement_current_save_state_slot(); \
		} \
		\
		if(J & BTN_INCREMENTSAVE) \
		{ \
			emulator_increment_current_save_state_slot(); \
		} \
		\
		if(J & BTN_INGAME_MENU) \
		{ \
			ingame_menu_enable(1); \
			is_running = 0; \
			mode_switch = MODE_MENU; \
		} \
	}

void systemReadJoypadGB(int n)
{
	//n is the max amount of players connected for us to loop through
	const uint64_t pads_connected = cell_pad_input_pads_connected();
	static uint64_t old_state[MAX_PADS];
	for(int i = 0; i < pads_connected; i++)
	{
		mainInputloop(i, old_state[i]);
		old_state[i] = state;
		extra_buttons_gb(i);
	}
}

#define systemReadJoypadGBA(pad) \
	static uint64_t old_state; \
	mainInputloop(pad, old_state); \
	old_state = state; \
	extra_buttons(joy);
	



// return information about the given joystick, -1 for default joystick

// Returns msecs since startup.
#ifdef USE_FRAMESKIP
uint32_t systemGetClock()
{
	//uint64_t now = get_usec();
	//return (now - startTime) / 1000;
	//FIXME - bogus value
	return 0;
}

bool systemPauseOnFrame()
{
	return 0;
}

#endif


void systemMessage(int id, const char * fmt, ...)
{
#ifdef CELL_DEBUG
	va_list args;
	va_start(args,fmt);
	va_end(args);

#endif

	// FIXME: really implement this...
	//PushScreenMessage(fmt);
}


void systemSetTitle(const char * title)
{
	//LOG_DBG("systemSetTitle(%s)\n", title);
}

#define init_sound() \
   audio_driver = NULL; \
   audio_handle = NULL; \
   audio_net = false; \
   audio_blocking = true;

bool systemSoundInit()
{
	soundShutdown();
	init_sound();
	return true;
}

void systemScreenMessage(const char *)
{

}

#ifdef USE_MOTION_SENSOR
void systemUpdateMotionSensor()
{

}


int  systemGetSensorX()
{
	return 0;
}


int  systemGetSensorY()
{
	return 0;
}
#endif


bool systemCanChangeSoundQuality()
{
	return true;
}


void systemShowSpeed(int speed)
{
	systemSpeed = speed;
	//LOG_DBG("systemShowSpeed: %3d%%(%d, %d fps)\n", systemSpeed, systemFrameSkip, renderedFrames );
}


void system10Frames(int rate)
{
	systemFrameSkip = 0;
}


void systemFrame()
{
}


void systemGbBorderOn()
{
}

float Emulator_GetFontSize()
{
	return Settings.PS3FontSize/100.0;
}


bool Emulator_IsROMLoaded()
{
	return current_rom != NULL && need_load_rom == false;
}

static void emulator_shutdown()
{
	if (Emulator_IsROMLoaded())
	{
		if(Settings.EmulatedSystem == IMAGE_GBA)
			CPUWriteBatteryFile(MakeFName(FILETYPE_BATTERY));
		else
			gbWriteBatteryFile(MakeFName(FILETYPE_BATTERY));
	}

	//add saving back of conf file
	emulator_save_settings(CONFIG_FILE);
#ifdef PS3_PROFILING
	// do any clean up... save stuff etc
	// ...

	// shutdown everything
	Graphics->DeinitDbgFont();
	Graphics->Deinit();

	// VBA - shutdown sound, release CEllAudio thread
	soundShutdown();

	if (Graphics)
		delete Graphics;

	cellSysmoduleUnloadModule(CELL_SYSMODULE_FS);
	cellSysmoduleUnloadModule(CELL_SYSMODULE_IO);
	cellSysmoduleUnloadModule(CELL_SYSMODULE_PNGDEC);
	cellSysutilUnregisterCallback(0);

	// ENABLE NET IO
	net_stdio_enable(1);
	// force exit --
	exit(0);
#else
	//Cleaner way to exit without graphics corruption when it returns to XMB
#ifdef MULTIMAN_SUPPORT
	if(return_to_MM)
	{
		if(audio_handle)
		{
			audio_driver->free(audio_handle);
			audio_handle = NULL; 
		}

		cellSysmoduleUnloadModule(CELL_SYSMODULE_AVCONF_EXT);
		sys_spu_initialize(6, 0);
		char multiMAN[512];
		snprintf(multiMAN, sizeof(multiMAN), "%s", "/dev_hdd0/game/BLES80608/USRDIR/RELOAD.SELF");
		sys_game_process_exitspawn2((char*) multiMAN, NULL, NULL, NULL, 0, 2048, SYS_PROCESS_PRIMARY_STACK_SIZE_64K);		
		sys_process_exit(0);
	}
	else
#endif
		sys_process_exit(0);
#endif
}

/* PS3 frontend - controls related macros */

#define init_setting_uint(charstring, setting, defaultvalue) \
	if(!(config_get_uint(currentconfig, charstring, &setting))) \
		setting = defaultvalue; 

#define init_setting_int(charstring, setting, defaultvalue) \
	if(!(config_get_int(currentconfig, charstring, &setting))) \
		setting = defaultvalue; 

#define init_setting_bool(charstring, setting, defaultvalue) \
	if(!(config_get_bool(currentconfig, charstring, &setting))) \
		setting = defaultvalue; 

#define init_setting_bool(charstring, setting, defaultvalue) \
	if(!(config_get_bool(currentconfig, charstring, &setting))) \
		setting =	defaultvalue;

#define init_setting_char(charstring, setting, defaultvalue) \
	if(!(config_get_char_array(currentconfig, charstring, setting, sizeof(setting)))) \
		strncpy(setting,defaultvalue, sizeof(setting));


#define string_concat_ps3_controls(padno, string) "PS3Player"#padno"::"#string""

#define map_ps3_standard_controls(padno) \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, DPad_Up),PS3Input.DPad_Up[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, DPad_Down),PS3Input.DPad_Down[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, DPad_Left),PS3Input.DPad_Left[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, DPad_Right),PS3Input.DPad_Right[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, ButtonCircle),PS3Input.ButtonCircle[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, ButtonCross), PS3Input.ButtonCross[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, ButtonTriangle), PS3Input.ButtonTriangle[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, ButtonSquare), PS3Input.ButtonSquare[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, ButtonSelect), PS3Input.ButtonSelect[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, ButtonStart), PS3Input.ButtonStart[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, ButtonL1), PS3Input.ButtonL1[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, ButtonR1), PS3Input.ButtonR1[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, ButtonL2), PS3Input.ButtonL2[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, ButtonR2), PS3Input.ButtonR2[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, ButtonL2_ButtonL3), PS3Input.ButtonL2_ButtonL3[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, ButtonL2_ButtonR3),PS3Input.ButtonL2_ButtonR3[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, ButtonR3),PS3Input.ButtonR3[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, ButtonL3), PS3Input.ButtonL3[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, ButtonL2_ButtonR2),PS3Input.ButtonL2_ButtonR2[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, ButtonL2_AnalogR_Right), PS3Input.ButtonL2_AnalogR_Right[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, ButtonL2_AnalogR_Left),PS3Input.ButtonL2_AnalogR_Left[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, ButtonL2_AnalogR_Up), PS3Input.ButtonL2_AnalogR_Up[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, ButtonL2_AnalogR_Down), PS3Input.ButtonL2_AnalogR_Down[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, ButtonR2_AnalogR_Right),PS3Input.ButtonR2_AnalogR_Right[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, ButtonR2_AnalogR_Left), PS3Input.ButtonR2_AnalogR_Left[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, ButtonR2_AnalogR_Up), PS3Input.ButtonR2_AnalogR_Up[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, ButtonR2_AnalogR_Down),PS3Input.ButtonR2_AnalogR_Down[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, ButtonL2_ButtonR2_AnalogR_Down), PS3Input.ButtonL2_ButtonR2_AnalogR_Down[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, ButtonR2_ButtonR3), PS3Input.ButtonR2_ButtonR3[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, ButtonR3_ButtonL3), PS3Input.ButtonR3_ButtonL3[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, AnalogR_Up), PS3Input.AnalogR_Up[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, AnalogR_Down), PS3Input.AnalogR_Down[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, AnalogR_Left), PS3Input.AnalogR_Left[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, AnalogR_Right),PS3Input.AnalogR_Right[padno]); \
   \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, AnalogR_Up_Type), PS3Input.AnalogR_Up_Type[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, AnalogR_Down_Type), PS3Input.AnalogR_Down_Type[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, AnalogR_Left_Type), PS3Input.AnalogR_Left_Type[padno]); \
   config_set_uint(currentconfig, string_concat_ps3_controls(padno, AnalogR_Right_Type), PS3Input.AnalogR_Right_Type[padno]);

#define get_ps3_standard_controls(padno) \
init_setting_uint(string_concat_ps3_controls(padno, DPad_Up),PS3Input.DPad_Up[padno],BTN_UP); \
init_setting_uint(string_concat_ps3_controls(padno, DPad_Down),PS3Input.DPad_Down[padno],BTN_DOWN); \
init_setting_uint(string_concat_ps3_controls(padno, DPad_Left),PS3Input.DPad_Left[padno],BTN_LEFT); \
init_setting_uint(string_concat_ps3_controls(padno, DPad_Right),PS3Input.DPad_Right[padno],BTN_RIGHT); \
init_setting_uint(string_concat_ps3_controls(padno, ButtonCircle),PS3Input.ButtonCircle[padno],BTN_A); \
init_setting_uint(string_concat_ps3_controls(padno, ButtonCross),PS3Input.ButtonCross[padno],BTN_B); \
init_setting_uint(string_concat_ps3_controls(padno, ButtonTriangle),PS3Input.ButtonTriangle[padno],BTN_NONE); \
init_setting_uint(string_concat_ps3_controls(padno, ButtonSquare),PS3Input.ButtonSquare[padno],BTN_NONE); \
init_setting_uint(string_concat_ps3_controls(padno, ButtonSelect),PS3Input.ButtonSelect[padno],BTN_SELECT); \
init_setting_uint(string_concat_ps3_controls(padno, ButtonStart),PS3Input.ButtonStart[padno],BTN_START); \
init_setting_uint(string_concat_ps3_controls(padno, ButtonL1), PS3Input.ButtonL1[padno], BTN_NONE); \
init_setting_uint(string_concat_ps3_controls(padno, ButtonR1), PS3Input.ButtonR1[padno], BTN_NONE); \
init_setting_uint(string_concat_ps3_controls(padno, ButtonL2), PS3Input.ButtonL2[padno], BTN_NONE); \
init_setting_uint(string_concat_ps3_controls(padno, ButtonR2), PS3Input.ButtonR2[padno], BTN_NONE); \
init_setting_uint(string_concat_ps3_controls(padno, ButtonL2_ButtonL3), PS3Input.ButtonL2_ButtonL3[padno], BTN_NONE); \
init_setting_uint(string_concat_ps3_controls(padno, ButtonL2_ButtonR3), PS3Input.ButtonL2_ButtonR3[padno], BTN_QUICKLOAD); \
init_setting_uint(string_concat_ps3_controls(padno, ButtonR3), PS3Input.ButtonR3[padno], BTN_NONE); \
init_setting_uint(string_concat_ps3_controls(padno, ButtonL3), PS3Input.ButtonL3[padno], BTN_NONE); \
init_setting_uint(string_concat_ps3_controls(padno, ButtonL2_ButtonR2), PS3Input.ButtonL2_ButtonR2[padno], BTN_NONE); \
init_setting_uint(string_concat_ps3_controls(padno, ButtonL2_AnalogR_Right), PS3Input.ButtonL2_AnalogR_Right[padno], BTN_NONE); \
init_setting_uint(string_concat_ps3_controls(padno, ButtonL2_AnalogR_Left), PS3Input.ButtonL2_AnalogR_Left[padno], BTN_NONE); \
init_setting_uint(string_concat_ps3_controls(padno, ButtonL2_AnalogR_Up), PS3Input.ButtonL2_AnalogR_Up[padno], BTN_NONE); \
init_setting_uint(string_concat_ps3_controls(padno, ButtonL2_AnalogR_Down), PS3Input.ButtonL2_AnalogR_Down[padno], BTN_NONE); \
init_setting_uint(string_concat_ps3_controls(padno, ButtonR2_AnalogR_Right), PS3Input.ButtonL2_AnalogR_Right[padno], BTN_NONE); \
init_setting_uint(string_concat_ps3_controls(padno, ButtonR2_AnalogR_Left), PS3Input.ButtonR2_AnalogR_Left[padno], BTN_NONE); \
init_setting_uint(string_concat_ps3_controls(padno, ButtonR2_AnalogR_Up), PS3Input.ButtonR2_AnalogR_Up[padno], BTN_NONE); \
init_setting_uint(string_concat_ps3_controls(padno, ButtonR2_AnalogR_Down), PS3Input.ButtonR2_AnalogR_Down[padno], BTN_NONE); \
init_setting_uint(string_concat_ps3_controls(padno, ButtonL2_ButtonR2_AnalogR_Down), PS3Input.ButtonL2_ButtonR2_AnalogR_Down[padno], BTN_NONE); \
init_setting_uint(string_concat_ps3_controls(padno, ButtonR2_ButtonR3), PS3Input.ButtonR2_ButtonR3[padno], BTN_QUICKSAVE); \
init_setting_uint(string_concat_ps3_controls(padno, ButtonR3_ButtonL3), PS3Input.ButtonR3_ButtonL3[padno], BTN_EXITTOMENU); \
init_setting_uint(string_concat_ps3_controls(padno, AnalogR_Up), PS3Input.AnalogR_Up[padno], BTN_NONE); \
init_setting_uint(string_concat_ps3_controls(padno, AnalogR_Down), PS3Input.AnalogR_Down[padno], BTN_NONE); \
init_setting_uint(string_concat_ps3_controls(padno, AnalogR_Left), PS3Input.AnalogR_Left[padno], BTN_DECREMENTSAVE); \
init_setting_uint(string_concat_ps3_controls(padno, AnalogR_Right), PS3Input.AnalogR_Right[padno], BTN_INCREMENTSAVE); \
init_setting_uint(string_concat_ps3_controls(padno, AnalogR_Up_Type), PS3Input.AnalogR_Up_Type[padno], 0); \
init_setting_uint(string_concat_ps3_controls(padno, AnalogR_Down_Type), PS3Input.AnalogR_Down_Type[padno], 0); \
init_setting_uint(string_concat_ps3_controls(padno, AnalogR_Left_Type), PS3Input.AnalogR_Left_Type[padno], 0); \
init_setting_uint(string_concat_ps3_controls(padno, AnalogR_Right_Type), PS3Input.AnalogR_Right_Type[padno], 0);

#define map_ps3_button_array(buttonarray) \
   for(int i = 0; i < MAX_PADS; i++) \
   { \
      INPUT_MAPBUTTON(PS3Input.DPad_Up[i],false,              buttonarray[0]); \
      INPUT_MAPBUTTON(PS3Input.DPad_Down[i],false,            buttonarray[1]); \
      INPUT_MAPBUTTON(PS3Input.DPad_Left[i],false,            buttonarray[2]); \
      INPUT_MAPBUTTON(PS3Input.DPad_Right[i],false,           buttonarray[3]); \
      INPUT_MAPBUTTON(PS3Input.ButtonCircle[i],false,         buttonarray[4]); \
      INPUT_MAPBUTTON(PS3Input.ButtonCross[i],false,          buttonarray[5]); \
      INPUT_MAPBUTTON(PS3Input.ButtonTriangle[i],false,       buttonarray[6]); \
      INPUT_MAPBUTTON(PS3Input.ButtonSquare[i],false,         buttonarray[7]); \
      INPUT_MAPBUTTON(PS3Input.ButtonSelect[i],false,         buttonarray[8]); \
      INPUT_MAPBUTTON(PS3Input.ButtonStart[i],false,          buttonarray[9]); \
      INPUT_MAPBUTTON(PS3Input.ButtonL1[i],false,             buttonarray[10]); \
      INPUT_MAPBUTTON(PS3Input.ButtonL2[i],false,             buttonarray[11]); \
      INPUT_MAPBUTTON(PS3Input.ButtonR2[i],false,             buttonarray[12]); \
      INPUT_MAPBUTTON(PS3Input.ButtonL3[i],false,             buttonarray[13]); \
      INPUT_MAPBUTTON(PS3Input.ButtonR3[i],false,             buttonarray[14]); \
      INPUT_MAPBUTTON(PS3Input.ButtonR1[i],false,             buttonarray[15]); \
      INPUT_MAPBUTTON(PS3Input.ButtonL2_ButtonL3[i],false,    buttonarray[16]); \
      INPUT_MAPBUTTON(PS3Input.ButtonL2_ButtonR2[i],false,    buttonarray[17]); \
      INPUT_MAPBUTTON(PS3Input.ButtonL2_ButtonR3[i],false,    buttonarray[18]); \
      INPUT_MAPBUTTON(PS3Input.ButtonR2_ButtonR3[i],false,    buttonarray[19]); \
      INPUT_MAPBUTTON(PS3Input.AnalogR_Up[i],false,           buttonarray[20]); \
      INPUT_MAPBUTTON(PS3Input.AnalogR_Down[i],false,         buttonarray[21]); \
      INPUT_MAPBUTTON(PS3Input.AnalogR_Left[i],false,         buttonarray[22]); \
      INPUT_MAPBUTTON(PS3Input.AnalogR_Right[i],false,        buttonarray[23]); \
      INPUT_MAPBUTTON(PS3Input.ButtonL2_AnalogR_Right[i],false, buttonarray[24]); \
      INPUT_MAPBUTTON(PS3Input.ButtonL2_AnalogR_Left[i],false, buttonarray[25]); \
      INPUT_MAPBUTTON(PS3Input.ButtonL2_AnalogR_Up[i],false, buttonarray[26]); \
      INPUT_MAPBUTTON(PS3Input.ButtonL2_AnalogR_Down[i],false, buttonarray[27]); \
      INPUT_MAPBUTTON(PS3Input.ButtonR2_AnalogR_Right[i],false, buttonarray[28]); \
      INPUT_MAPBUTTON(PS3Input.ButtonR2_AnalogR_Left[i],false, buttonarray[29]); \
      INPUT_MAPBUTTON(PS3Input.ButtonR2_AnalogR_Up[i],false, buttonarray[30]); \
      INPUT_MAPBUTTON(PS3Input.ButtonR2_AnalogR_Down[i],false, buttonarray[31]); \
      INPUT_MAPBUTTON(PS3Input.ButtonL2_ButtonR2_AnalogR_Down[i],false, buttonarray[32]); \
      INPUT_MAPBUTTON(PS3Input.ButtonR3_ButtonL3[i],false, buttonarray[33]); \
      PS3Input.AnalogR_Up_Type[i] = buttonarray[34]; \
      PS3Input.AnalogR_Down_Type[i] = buttonarray[35]; \
      PS3Input.AnalogR_Left_Type[i] = buttonarray[36]; \
      PS3Input.AnalogR_Right_Type[i] = buttonarray[37]; \
   }
   
void emulator_implementation_button_mapping_settings(int map_button_option_enum)
{
	config_file_t * currentconfig = config_file_new(SYS_CONFIG_FILE);
	switch(map_button_option_enum)
	{
		case MAP_BUTTONS_OPTION_SETTER:
			map_ps3_standard_controls(0);
			map_ps3_standard_controls(1);
			map_ps3_standard_controls(2);
			map_ps3_standard_controls(3);
			map_ps3_standard_controls(4);
			map_ps3_standard_controls(5);
			map_ps3_standard_controls(6);
			map_ps3_standard_controls(MAX_PADS);
			config_file_write(currentconfig, SYS_CONFIG_FILE);
			break;
		case MAP_BUTTONS_OPTION_GETTER:
			get_ps3_standard_controls(0);
			get_ps3_standard_controls(1);
			get_ps3_standard_controls(2);
			get_ps3_standard_controls(3);
			get_ps3_standard_controls(4);
			get_ps3_standard_controls(5);
			get_ps3_standard_controls(6);
			get_ps3_standard_controls(MAX_PADS);
			break;
		case MAP_BUTTONS_OPTION_DEFAULT:
			{
				uint32_t array_btn[] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B, BTN_NONE, BTN_NONE, BTN_SELECT, BTN_START, BTN_L, BTN_NONE, BTN_NONE, BTN_NONE, BTN_INGAME_MENU, BTN_R, BTN_NONE, BTN_NONE, BTN_QUICKLOAD, BTN_QUICKSAVE, BTN_NONE, BTN_NONE, BTN_DECREMENTSAVE, BTN_INCREMENTSAVE, BTN_NONE, BTN_NONE, BTN_NONE, BTN_NONE, BTN_NONE, BTN_NONE, BTN_NONE, BTN_NONE, BTN_NONE, BTN_EXITTOMENU, 0, 0, 0, 0};
				map_ps3_button_array(array_btn);
			}
			break;
		case MAP_BUTTONS_OPTION_NEW:
			{
				uint32_t array_btn[] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B, BTN_NONE, BTN_NONE, BTN_SELECT, BTN_START, BTN_L, BTN_NONE, BTN_NONE, BTN_NONE, BTN_INGAME_MENU, BTN_R, BTN_NONE, BTN_NONE, BTN_NONE, BTN_NONE, BTN_NONE, BTN_NONE, BTN_NONE, BTN_NONE, BTN_NONE, BTN_NONE, BTN_NONE, BTN_NONE, BTN_INCREMENTSAVE, BTN_DECREMENTSAVE, BTN_QUICKLOAD, BTN_QUICKSAVE, BTN_NONE, BTN_EXITTOMENU, 0, 0, 0, 0};
				map_ps3_button_array(array_btn);
			}
			break;
	}
}

static bool file_exists(const char * filename)
{
	CellFsStat sb;
	if(cellFsStat(filename,&sb) == CELL_FS_SUCCEEDED)
		return true;
	else
		return false;
}

static void emulator_init_settings()
{
	memset((&Settings), 0, (sizeof(Settings)));

	if(!file_exists(SYS_CONFIG_FILE))
	{
		FILE * f;
		f = fopen(SYS_CONFIG_FILE, "w");
		fclose(f);
	}

	config_file_t * currentconfig = config_file_new(SYS_CONFIG_FILE);

	init_setting_uint("PS3General::ApplyShaderPresetOnStartup", Settings.ApplyShaderPresetOnStartup, 0);
	init_setting_uint("PS3General::KeepAspect", Settings.PS3KeepAspect, ASPECT_RATIO_4_3);
	init_setting_uint("PS3General::Smooth", Settings.PS3Smooth, 1);
	init_setting_uint("PS3General::Smooth2", Settings.PS3Smooth2, 1);
	init_setting_char("PS3General::PS3CurrentShader", Settings.PS3CurrentShader, DEFAULT_SHADER_FILE);
	init_setting_char("PS3General::PS3CurrentShader2", Settings.PS3CurrentShader2, DEFAULT_SHADER_FILE);
	init_setting_char("PS3General::Border", Settings.PS3CurrentBorder, DEFAULT_BORDER_FILE);
	init_setting_uint("PS3General::PS3TripleBuffering",Settings.TripleBuffering, 1);
	init_setting_char("PS3General::ShaderPresetPath", Settings.ShaderPresetPath, DEFAULT_PRESET_FILE);
	init_setting_char("PS3General::ShaderPresetTitle", Settings.ShaderPresetTitle, "Custom");
	init_setting_uint("PS3General::ScaleFactor", Settings.ScaleFactor, 2);
	init_setting_uint("PS3General::ViewportX", Settings.ViewportX, 0);
	init_setting_uint("PS3General::ViewportY", Settings.ViewportY, 0);
	init_setting_uint("PS3General::ViewportWidth", Settings.ViewportWidth, 0);
	init_setting_uint("PS3General::ViewportHeight", Settings.ViewportHeight, 0);
	init_setting_uint("PS3General::ScaleEnabled", Settings.ScaleEnabled, 1);
	init_setting_uint("PS3General::PS3CurrentResolution", Settings.PS3CurrentResolution, NULL);
	init_setting_uint("PS3General::OverscanEnabled", Settings.PS3OverscanEnabled, 0);
	init_setting_int("PS3General::OverscanAmount", Settings.PS3OverscanAmount, 0);
	init_setting_uint("PS3General::PS3PALTemporalMode60Hz", Settings.PS3PALTemporalMode60Hz, 0);
	init_setting_uint("Sound::SoundMode", Settings.SoundMode, SOUND_MODE_NORMAL);
	init_setting_char("RSound::RSoundServerIPAddress",  Settings.RSoundServerIPAddress, "0.0.0.0");
	init_setting_uint("PS3General::Throttled", Settings.Throttled, 1);
	init_setting_uint("PS3General::PS3FontSize", Settings.PS3FontSize, 100);
	init_setting_char("PS3Paths::PathSaveStates", Settings.PS3PathSaveStates, usrDirPath);
	init_setting_char("PS3Paths::PathSRAM", Settings.PS3PathSRAM, usrDirPath);
	init_setting_char("PS3Paths::PathROMDirectory", Settings.PS3PathROMDirectory, "/");
	init_setting_uint("PS3General::ControlScheme", Settings.ControlScheme, CONTROL_SCHEME_DEFAULT);
	init_setting_uint("PS3General::CurrentSaveStateSlot",  Settings.CurrentSaveStateSlot, 0);
	init_setting_uint("PS3General::ScreenshotsEnabled", Settings.ScreenshotsEnabled, 0);
	char tmp_str[256];
	if (config_get_char_array(currentconfig,"PS3General::GameAwareShaderPath", tmp_str, sizeof(tmp_str)))
		config_get_char_array(currentconfig, "PS3General::GameAwareShaderPath", Settings.GameAwareShaderPath, sizeof(Settings.GameAwareShaderPath));

	/* emulator-specific */
	init_setting_uint("PS3General::ControlStyle", Settings.ControlStyle, CONTROL_STYLE_ORIGINAL);
	init_setting_uint("VBA::SGBBorders", Settings.SGBBorders, 1);
	init_setting_char("VBA::GBABIOS", Settings.GBABIOS, "");

	emulator_implementation_switch_control_scheme();
}

void emulator_implementation_set_shader_preset(const char * fname)
{
	config_file_t * currentconfig = config_file_new(fname);

	init_setting_uint("ScaleFactor", Settings.ScaleFactor, Settings.ScaleFactor);
	init_setting_uint("Smooth", Settings.PS3Smooth, Settings.PS3Smooth);
	init_setting_uint("Smooth2", Settings.PS3Smooth2, Settings.PS3Smooth2);
	init_setting_uint("ScaleEnabled", Settings.ScaleEnabled, Settings.ScaleEnabled);
	if(config_string_exists(currentconfig, "PS3CurrentShader", Settings.PS3CurrentShader))
	{
		char shadertemp[MAX_PATH_LENGTH];
		init_setting_char("PS3CurrentShader", shadertemp, DEFAULT_SHADER_FILE);
		snprintf(Settings.PS3CurrentShader, sizeof(Settings.PS3CurrentShader), "%s/%s", SHADERS_DIR_PATH, shadertemp);
	}
	if(config_string_exists(currentconfig, "PS3CurrentShader2", Settings.PS3CurrentShader2))
	{
		char shadertemp2[MAX_PATH_LENGTH];
		init_setting_char("PS3CurrentShader2", shadertemp2, DEFAULT_SHADER_FILE);
		snprintf(Settings.PS3CurrentShader2, sizeof(Settings.PS3CurrentShader2), "%s/%s", SHADERS_DIR_PATH, shadertemp2);
	}

	strncpy(Settings.ShaderPresetPath, fname, sizeof(Settings.ShaderPresetPath));

	init_setting_char("ShaderPresetTitle", Settings.ShaderPresetTitle, "Custom/None");
	printf("Gets to ShaderPresetTitle: %s\n", Settings.ShaderPresetTitle);

	//FIXME: This does not yet work
#if 0
	if (currentconfig.Exists("PS3General::Border"))
	{
		Settings.PS3CurrentBorder = currentconfig.GetString("PS3General::Border");
		//need_border_reload_hack = true;
	}
#endif
	init_setting_uint("KeepAspect", Settings.PS3KeepAspect, Settings.PS3KeepAspect);
	init_setting_uint("OverscanEnabled", Settings.PS3OverscanEnabled, Settings.PS3OverscanEnabled);
	init_setting_int("OverscanAmount", Settings.PS3OverscanAmount, Settings.PS3OverscanAmount);
	init_setting_uint("ViewportX", Settings.ViewportX, Settings.ViewportX);
	init_setting_uint("ViewportY", Settings.ViewportY, Settings.ViewportY);
	init_setting_uint("ViewportWidth", Settings.ViewportWidth, Settings.ViewportWidth);
	init_setting_uint("ViewportHeight", Settings.ViewportHeight, Settings.ViewportHeight);
	Graphics->LoadFragmentShader(Settings.PS3CurrentShader);
	Graphics->LoadFragmentShader(Settings.PS3CurrentShader2, 1);
	Graphics->SetFBOScale(Settings.ScaleEnabled,Settings.ScaleFactor);
	Graphics->set_aspect_ratio(Settings.PS3KeepAspect, srcWidth, srcHeight, 1);
	Graphics->SetOverscan(Settings.PS3OverscanEnabled, (float)Settings.PS3OverscanAmount/100);
	Graphics->SetSmooth(Settings.PS3Smooth);
	Graphics->SetSmooth(Settings.PS3Smooth2, 1);
}


void emulator_implementation_switch_control_scheme(void)
{
	switch(Settings.ControlScheme)
	{
		case CONTROL_SCHEME_DEFAULT:
			emulator_implementation_button_mapping_settings(MAP_BUTTONS_OPTION_DEFAULT);
			break;
		case CONTROL_SCHEME_NEW:
			emulator_implementation_button_mapping_settings(MAP_BUTTONS_OPTION_NEW);
			break;
		case CONTROL_SCHEME_CUSTOM:
			emulator_implementation_button_mapping_settings(MAP_BUTTONS_OPTION_GETTER);
			break;
	}
}

/* PS3 frontend callbacks */

static void cb_save_custom_controls(int button_type, void *userdata)
{
	switch(button_type)
	{
		case CELL_MSGDIALOG_BUTTON_YES:
			Settings.SaveCustomControlScheme = true;
			break;
		case CELL_MSGDIALOG_BUTTON_ESCAPE:
		case CELL_MSGDIALOG_BUTTON_NO:
			Settings.SaveCustomControlScheme = false;
			break;
	}
	dialog_is_running = false;
	cellMsgDialogClose(0.0f);
}

void emulator_implementation_save_custom_controls(bool showdialog)
{
	if(Settings.ControlScheme == CONTROL_SCHEME_CUSTOM)
	{
		if(showdialog)
		{
			dialog_is_running = true;
			cellMsgDialogOpen2(CELL_MSGDIALOG_DIALOG_TYPE_NORMAL|\
			CELL_MSGDIALOG_TYPE_BG_VISIBLE|\
			CELL_MSGDIALOG_TYPE_BUTTON_TYPE_YESNO|\
			CELL_MSGDIALOG_TYPE_DISABLE_CANCEL_OFF|\
			CELL_MSGDIALOG_TYPE_DEFAULT_CURSOR_YES,\
			"Do you want to save the custom controller settings?",\
			cb_save_custom_controls,NULL,NULL);
			while(dialog_is_running && is_running)
			{
				glClear(GL_COLOR_BUFFER_BIT);
				psglSwap();
				cellSysutilCheckCallback();	
			}
		}
		if(!showdialog || Settings.SaveCustomControlScheme)
		{
			emulator_implementation_button_mapping_settings(MAP_BUTTONS_OPTION_SETTER);
		}
	}
}

void emulator_save_settings(uint64_t filetosave)
{
	config_file_t * currentconfig = config_file_new(SYS_CONFIG_FILE);
	char filepath[MAX_PATH_LENGTH];

	switch(filetosave)
	{
		case CONFIG_FILE:
			strncpy(filepath, SYS_CONFIG_FILE, sizeof(filepath));

			config_set_uint(currentconfig, "PS3General::ControlScheme",Settings.ControlScheme);
			config_set_uint(currentconfig, "PS3General::CurrentSaveStateSlot",Settings.CurrentSaveStateSlot);
			config_set_uint(currentconfig, "PS3General::KeepAspect",Settings.PS3KeepAspect);
			config_set_uint(currentconfig, "PS3General::ApplyShaderPresetOnStartup", Settings.ApplyShaderPresetOnStartup);
			config_set_uint(currentconfig, "PS3General::ViewportX", Graphics->get_viewport_x());
			config_set_uint(currentconfig, "PS3General::ViewportY", Graphics->get_viewport_y());
			config_set_uint(currentconfig, "PS3General::ViewportWidth", Graphics->get_viewport_width());
			config_set_uint(currentconfig, "PS3General::ViewportHeight", Graphics->get_viewport_height());
			config_set_uint(currentconfig, "PS3General::Smooth", Settings.PS3Smooth);
			config_set_uint(currentconfig, "PS3General::Smooth2", Settings.PS3Smooth2);
			config_set_uint(currentconfig, "PS3General::ScaleFactor", Settings.ScaleFactor);
			config_set_uint(currentconfig, "PS3General::OverscanEnabled", Settings.PS3OverscanEnabled);
			config_set_uint(currentconfig, "PS3General::OverscanAmount",Settings.PS3OverscanAmount);
			config_set_uint(currentconfig, "PS3General::PS3FontSize",Settings.PS3FontSize);
			config_set_uint(currentconfig, "PS3General::Throttled",Settings.Throttled);
			config_set_uint(currentconfig, "PS3General::PS3PALTemporalMode60Hz",Settings.PS3PALTemporalMode60Hz);
			config_set_uint(currentconfig, "PS3General::PS3TripleBuffering",Settings.TripleBuffering);
			config_set_uint(currentconfig, "PS3General::ScreenshotsEnabled",Settings.ScreenshotsEnabled);
			config_set_uint(currentconfig, "Sound::SoundMode",Settings.SoundMode);
			config_set_uint(currentconfig, "PS3General::PS3CurrentResolution",Graphics->GetCurrentResolution());
			config_set_string(currentconfig, "PS3General::ShaderPresetPath", Settings.ShaderPresetPath);
			config_set_string(currentconfig, "PS3General::ShaderPresetTitle", Settings.ShaderPresetTitle);
			config_set_string(currentconfig, "PS3General::PS3CurrentShader",Graphics->GetFragmentShaderPath());
			config_set_string(currentconfig, "PS3General::PS3CurrentShader2", Graphics->GetFragmentShaderPath(1));
			config_set_string(currentconfig, "PS3General::Border", Settings.PS3CurrentBorder);
			config_set_string(currentconfig, "PS3General::GameAwareShaderPath", Settings.GameAwareShaderPath);
			config_set_string(currentconfig, "PS3Paths::PathSaveStates", Settings.PS3PathSaveStates);
			config_set_string(currentconfig, "PS3Paths::PathSRAM", Settings.PS3PathSRAM);
			config_set_string(currentconfig, "PS3Paths::PathROMDirectory", Settings.PS3PathROMDirectory);
			config_set_string(currentconfig, "RSound::RSoundServerIPAddress", Settings.RSoundServerIPAddress);
			config_set_uint(currentconfig, "PS3General::ScaleEnabled", Settings.ScaleEnabled);

			//Emulator-specific settings
			config_set_string(currentconfig, "VBA::GBABIOS",Settings.GBABIOS);
			config_set_uint(currentconfig, "PS3General::ControlStyle",Settings.ControlStyle);
			config_set_uint(currentconfig, "VBA::SGBBorders",Settings.SGBBorders);

			config_file_write(currentconfig, filepath);
			emulator_implementation_button_mapping_settings(MAP_BUTTONS_OPTION_SETTER);
			break;
		case SHADER_PRESET_FILE:
			snprintf(filepath, sizeof(filepath), "%s/test.conf", usrDirPath);
			currentconfig = config_file_new(filepath);

			config_set_string(currentconfig, "PS3General::PS3CurrentShader", Graphics->GetFragmentShaderPath());
			config_set_string(currentconfig, "PS3General::PS3CurrentShader2", Graphics->GetFragmentShaderPath(1));
			config_set_string(currentconfig, "PS3General::Border", Settings.PS3CurrentBorder);
			config_set_uint(currentconfig, "PS3General::Smooth", Settings.PS3Smooth);
			config_set_uint(currentconfig, "PS3General::Smooth2", Settings.PS3Smooth2);
			char tmp[10] = "Test";
			config_set_string(currentconfig, "PS3General::ShaderPresetTitle", tmp);
			config_set_uint(currentconfig, "PS3General::ViewportX", Graphics->get_viewport_x());
			config_set_uint(currentconfig, "PS3General::ViewportY", Graphics->get_viewport_y());
			config_set_uint(currentconfig, "PS3General::ViewportWidth", Graphics->get_viewport_width());
			config_set_uint(currentconfig, "PS3General::ViewportHeight", Graphics->get_viewport_height());
			config_set_uint(currentconfig, "PS3General::ScaleFactor", Settings.ScaleFactor);
			config_set_uint(currentconfig, "PS3General::ScaleEnabled", Settings.ScaleEnabled);
			config_set_uint(currentconfig, "PS3General::OverscanEnabled", Settings.PS3OverscanEnabled);
			config_set_uint(currentconfig, "PS3General::OverscanAmount", Settings.PS3OverscanAmount);

			config_file_write(currentconfig, filepath);
			break;
	}
}

void Emulator_StopROMRunning()
{
	is_running = false;
}

void Emulator_StartROMRunning(uint32_t set_is_running)
{
	if(set_is_running)
		is_running = 1;
	mode_switch = MODE_EMULATION;
}


void LoadImagePreferences()
{
	FILE *f = fopen(MakeFName(FILETYPE_IMAGE_PREFS), "r");
	if(!f)
		return;

	char buffer[7];
	buffer[0] = '[';
	buffer[1] = rom[0xac];
	buffer[2] = rom[0xad];
	buffer[3] = rom[0xae];
	buffer[4] = rom[0xaf];
	buffer[5] = ']';
	buffer[6] = 0;

	char readBuffer[2048];

	bool found = false;

	do
	{
		char *s = fgets(readBuffer, 2048, f);

		if(s == NULL)
			break;

		char *p  = strchr(s, ';');

		if(p)
			*p = 0;

		char *token = strtok(s, " \t\n\r=");

		if(!token || strlen(token) == 0)
			continue;

		if(!strcmp(token, buffer))
		{
			found = true;
			break;
		}
	}while(1);

	if(found)
	{
		do
		{
			char *s = fgets(readBuffer, 2048, f);
			if(s == NULL)
				break;

			char *p = strchr(s, ';');
			if(p)
				*p = 0;

			char *token = strtok(s, " \t\n\r=");
			if(!token || (strlen(token) == 0))
				continue;

			if(token[0] == '[') // starting another image settings
				break;

			char *value = strtok(NULL, "\t\n\r=");

			if(value == NULL)
				continue;

			if(strcmp(token, "rtcEnabled") == 0)
				enableRtc = atoi(value);
			else if(strcmp(token, "flashSize") == 0)
				flashSize = atoi(value);
			else if(strcmp(token, "saveType") == 0)
			{
				int save = atoi(value);
				if(save >= 0 && save <= 5)
					cpuSaveType = save;
			}
			else if(strcmp(token, "mirroringEnabled") == 0)
				mirroringEnable = (atoi(value) == 0 ? false : true);
		}while(1);
	}
	fclose(f);
}

void vba_toggle_sgb_border(bool set_border)
{
	gbBorderOn = set_border;
	sgb_border_change = true;
}

#define sgb_settings() \
	srcWidth = 256; \
	srcHeight = 224; \
	gbBorderLineSkip = 256; \
	gbBorderColumnSkip = 48; \
	gbBorderRowSkip = 40;

#define gb_settings() \
	srcWidth = 160; \
	srcHeight = 144; \
	gbBorderLineSkip = 160; \
	gbBorderColumnSkip = 0; \
	gbBorderRowSkip = 0;

#define gba_settings() \
	srcWidth = 240; \
	srcHeight = 160;

#define sound_init() \
   soundInit(); \
   soundSetSampleRate(48000);

static void vba_set_screen_dimensions()
{
	if (Settings.EmulatedSystem == IMAGE_GB)
	{
		if(gbBorderOn)
		{
			sgb_settings();
		}
		else
		{
			gb_settings();
		}
	}
	else if (Settings.EmulatedSystem == IMAGE_GBA)
	{
		gba_settings();
	}
}

static void vba_init()
{
	// FIXME: reconsider where we call this. IT MUST BE before loading/initing VBA
	system_init();

	if (Settings.EmulatedSystem == IMAGE_GB)
	{
		if(gbBorderOn)
		{
			sgb_settings();
		}
		else
		{
			gb_settings();
		}

		sound_init();
		gbGetHardwareType();

		// support for
		// if (gbHardware & 5)
		//	gbCPUInit(gbBiosFileName, useBios);

		gbSoundReset();
		gbSoundSetDeclicking(false);
		gbReset();
	}
	else if (Settings.EmulatedSystem == IMAGE_GBA)
	{
		gba_settings();

		// VBA - set all the defaults
		cpuSaveType = 0;			//automatic
		flashSize = 0x10000;			//1M
		enableRtc = false;			//realtime clock
		mirroringEnable = false;

#ifdef USE_AGBPRINT
		agbPrintEnable(false);		
#endif

		// Load per image compatibility prefs from vba-over.ini
		LoadImagePreferences();

		if(flashSize == 0x10000 || flashSize == 0x20000)
			flashSetSize(flashSize);

		rtcEnable(enableRtc);		
		doMirroring(mirroringEnable);

		sound_init();

		//Loading of the BIOS
		CPUInit((strcmp(Settings.GBABIOS,"\0") == 0) ? NULL : Settings.GBABIOS, (strcmp(Settings.GBABIOS,"\0") == 0) ? false : true);

		CPUReset();
		soundReset();
	}
	else
		emulator_shutdown(); //FIXME: be more graceful
}

#define set_game_system(system) \
   Settings.EmulatedSystem = system; \
   vba_init();

static int emulator_detect_game_system(char * filename)
{
	char file_to_check[MAX_PATH_LENGTH];
	int system_returnvalue;

	if (!strncasecmp(".zip", &filename[strlen(filename) - 4], 4) ||
	!strncasecmp(".ZIP", &filename[strlen(filename) - 4], 4))
	{
		/*ZIP file, we will have to extract the first file from the
		archive and look at its ROM filename */

		get_zipfilename(filename, file_to_check);
	}
	else
	{
		strncpy(file_to_check, filename, sizeof(file_to_check));
	}

	if (utilIsGBImage(file_to_check))
	{
		return IMAGE_GB;
	}
	else if(utilIsGBAImage(file_to_check))
	{
		return IMAGE_GBA;
	}
	else
	{
		return IMAGE_UNKNOWN;
	}
}

static void vba_init_rom()
{
	if(need_load_rom)
	{
		int whichsystem = emulator_detect_game_system(current_rom);
		Settings.EmulatedSystem = whichsystem;

		switch(Settings.EmulatedSystem)
		{
			case IMAGE_GB:
				if (!gbLoadRom(current_rom))
					emulator_shutdown();

				set_game_system(IMAGE_GB);

				gbReadBatteryFile(MakeFName(FILETYPE_BATTERY));

				need_load_rom = false;

				if(Settings.LastEmulatedSystem != IMAGE_GB || sgb_border_change)
					reinit_video = 1;

				Settings.LastEmulatedSystem = IMAGE_GB;
				break;
			case IMAGE_GBA:
				gbaRomSize = CPULoadRom(current_rom);

				if (!gbaRomSize)
					emulator_shutdown();

				set_game_system(IMAGE_GBA);

				CPUReadBatteryFile(MakeFName(FILETYPE_BATTERY));

				need_load_rom = false;

				if(Settings.LastEmulatedSystem != IMAGE_GBA)
					reinit_video = 1;

				Settings.LastEmulatedSystem = IMAGE_GBA;
				break;
			case IMAGE_UNKNOWN:
				Settings.EmulatedSystem = IMAGE_UNKNOWN;
				emulator_shutdown();
		}
	}
}

void Emulator_RequestLoadROM(const char * filename)
{
	if (current_rom == NULL || strcmp(filename, current_rom) != 0 || sgb_border_change)
	{
		if (current_rom != NULL)
			free(current_rom);

		current_rom = strdup(filename);
		need_load_rom = true;
	}
	else
	{
		need_load_rom = false;

		//reset rom instead of loading it again
		if(Settings.EmulatedSystem == IMAGE_GBA)
			CPUReset();
		else
			gbReset();
	}
	vba_init_rom();
}



static void sysutil_exit_callback (uint64_t status, uint64_t param, void *userdata)
{
	(void) param;
	(void) userdata;

	switch (status)
	{
		case CELL_SYSUTIL_REQUEST_EXITGAME:
			menu_is_running = 0;
			is_running = 0;
			is_ingame_menu_running = 0;
#ifdef MULTIMAN_SUPPORT
			return_to_MM = false;
#endif
			mode_switch = MODE_EXIT;
			break;
		case CELL_SYSUTIL_DRAWING_BEGIN:
		case CELL_SYSUTIL_DRAWING_END:
			break;
		case CELL_SYSUTIL_OSKDIALOG_LOADED:
			break;
		case CELL_SYSUTIL_OSKDIALOG_FINISHED:
			oskutil_stop(&oskutil_handle);
			break;
		case CELL_SYSUTIL_OSKDIALOG_UNLOADED:
			oskutil_close(&oskutil_handle);
			break;
	}
}


//FIXME: Turn GREEN into WHITE and RED into LIGHTBLUE once the overlay is in
#define ingame_menu_reset_entry_colors(ingame_menu_item) \
{ \
   for(int i = 0; i <= MENU_ITEM_LAST; i++) \
      menuitem_colors[i] = GREEN; \
   menuitem_colors[ingame_menu_item] = RED; \
}


static void ingame_menu(void)
{
	uint64_t menuitem_colors[MENU_ITEM_LAST];
	char comment[256];
	char aspectratio[256];
	do
	{
		input_code_state_begin();
		uint64_t stuck_in_loop = 1;

		if(CTRL_CIRCLE(state))
		{
			is_running = 1;
			ingame_menu_item = 0;
			is_ingame_menu_running = 0;
			Emulator_StartROMRunning(0);
		}

		Graphics->Draw(pix);

		switch(ingame_menu_item)
		{
			case MENU_ITEM_LOAD_STATE:
				if(CTRL_CROSS(button_was_pressed))
				{
					if(Settings.EmulatedSystem == IMAGE_GBA)
					{
						emulator_load_current_save_state_slot(CPUReadState);
					}
					else
					{
						emulator_load_current_save_state_slot(gbReadSaveState);
					}

					is_running = 1;
					ingame_menu_item = 0;
					is_ingame_menu_running = 0;
					Emulator_StartROMRunning(0);
				}

				if(CTRL_LEFT(button_was_pressed) || CTRL_LSTICK_LEFT(button_was_pressed))
				{
					emulator_decrement_current_save_state_slot();
				}

				if(CTRL_RIGHT(button_was_pressed) || CTRL_LSTICK_RIGHT(button_was_pressed))
				{
					emulator_increment_current_save_state_slot();
				}

				ingame_menu_reset_entry_colors(ingame_menu_item);
				strcpy(comment,"Press LEFT or RIGHT to change the current save state slot.\nPress CROSS to load the state from the currently selected save state slot.");
				break;
			case MENU_ITEM_SAVE_STATE:
				if(CTRL_CROSS(button_was_pressed))
				{
					if(Settings.EmulatedSystem == IMAGE_GBA)
					{
						emulator_save_current_save_state_slot(CPUWriteState);
					}
					else
					{
						emulator_save_current_save_state_slot(gbWriteSaveState);
					}
					snprintf(special_action_msg, sizeof(special_action_msg), "Saved to save state slot #%d", Settings.CurrentSaveStateSlot);
					is_running = 1;
					ingame_menu_item = 0;
					is_ingame_menu_running = 0;
					Emulator_StartROMRunning(0);
				}

				if(CTRL_LEFT(button_was_pressed) || CTRL_LSTICK_LEFT(button_was_pressed))
				{
					emulator_decrement_current_save_state_slot();
				}

				if(CTRL_RIGHT(button_was_pressed) || CTRL_LSTICK_RIGHT(button_was_pressed))
				{
					emulator_increment_current_save_state_slot();
				}

				ingame_menu_reset_entry_colors (ingame_menu_item);
				strcpy(comment, "Press LEFT or RIGHT to change the current save state slot.\nPress CROSS to save the state to the currently selected save state slot.");
				break;
			case MENU_ITEM_KEEP_ASPECT_RATIO:
				if(CTRL_LEFT(button_was_pressed) || CTRL_LSTICK_LEFT(button_was_pressed))
				{
					if(Settings.PS3KeepAspect > 0)
					{
						Settings.PS3KeepAspect--;
						Graphics->set_aspect_ratio(Settings.PS3KeepAspect, srcWidth, srcHeight, 1);
					}
				}
				if(CTRL_RIGHT(button_was_pressed)  || CTRL_LSTICK_RIGHT(button_was_pressed))
				{
					if(Settings.PS3KeepAspect < LAST_ASPECT_RATIO)
					{
						Settings.PS3KeepAspect++;
						Graphics->set_aspect_ratio(Settings.PS3KeepAspect, srcWidth, srcHeight, 1);
					}
				}
				if(CTRL_START(button_was_pressed))
				{
					Settings.PS3KeepAspect = ASPECT_RATIO_4_3;
					Graphics->set_aspect_ratio(Settings.PS3KeepAspect, srcWidth, srcHeight, 1);
				}
				ingame_menu_reset_entry_colors (ingame_menu_item);
				strcpy(comment, "Press LEFT or RIGHT to change the [Aspect Ratio].\nPress START to reset back to default values.");
				break;
			case MENU_ITEM_OVERSCAN_AMOUNT:
				if(CTRL_LEFT(button_was_pressed)  ||  CTRL_LSTICK_LEFT(button_was_pressed) || CTRL_CROSS(button_was_pressed) ||
						CTRL_LSTICK_LEFT(button_was_held))
				{
					Settings.PS3OverscanAmount--;
					Settings.PS3OverscanEnabled = 1;
					Graphics->SetOverscan(Settings.PS3OverscanEnabled, (float)Settings.PS3OverscanAmount/100);

					if(Settings.PS3OverscanAmount == 0)
					{
						Settings.PS3OverscanEnabled = 0;
						Graphics->SetOverscan(Settings.PS3OverscanEnabled, (float)Settings.PS3OverscanAmount/100);
					}
				}

				if(CTRL_RIGHT(button_was_pressed) || CTRL_LSTICK_RIGHT(button_was_pressed) || CTRL_CROSS(button_was_pressed) ||
						CTRL_LSTICK_RIGHT(button_was_held))
				{
					Settings.PS3OverscanAmount++;
					Settings.PS3OverscanEnabled = 1;
					Graphics->SetOverscan(Settings.PS3OverscanEnabled, (float)Settings.PS3OverscanAmount/100);

					if(Settings.PS3OverscanAmount == 0)
					{
						Settings.PS3OverscanEnabled = 0;
						Graphics->SetOverscan(Settings.PS3OverscanEnabled, (float)Settings.PS3OverscanAmount/100);
					}
				}

				if(CTRL_START(button_was_pressed))
				{
					Settings.PS3OverscanAmount = 0;
					Settings.PS3OverscanEnabled = 0;
					Graphics->SetOverscan(Settings.PS3OverscanEnabled, (float)Settings.PS3OverscanAmount/100);
				}
				ingame_menu_reset_entry_colors (ingame_menu_item);
				strcpy(comment, "Press LEFT or RIGHT to change the [Overscan] settings.\nPress START to reset back to default values.");
				break;
			case MENU_ITEM_FRAME_ADVANCE:
				if(CTRL_CROSS(state) || CTRL_R2(state) || CTRL_L2(state))
				{
					is_running = 0;
					ingame_menu_item = MENU_ITEM_FRAME_ADVANCE;
					is_ingame_menu_running = 0;
					Emulator_StartROMRunning(0);
				}
				ingame_menu_reset_entry_colors (ingame_menu_item);
				strcpy(comment, "Press 'CROSS', 'L2' or 'R2' button to step one frame.\nNOTE: Pressing the button rapidly will advance the frame more slowly\nand prevent buttons from being input.");
				break;
			case MENU_ITEM_RESIZE_MODE:
				ingame_menu_reset_entry_colors (ingame_menu_item);
				if(CTRL_CROSS(state))
				{
					Graphics->set_aspect_ratio(ASPECT_RATIO_CUSTOM, srcWidth, srcHeight, 1);
					do
					{
						Graphics->Draw(pix);
						state = cell_pad_input_poll_device(0);
						Graphics->resize_aspect_mode_input_loop(state);
						if(CTRL_CIRCLE(state))
						{
							sys_timer_usleep(FILEBROWSER_DELAY);
							stuck_in_loop = 0;
						}

						psglSwap();
						cellSysutilCheckCallback();
						old_state = state;
					}while(stuck_in_loop && is_ingame_menu_running);
				}
				strcpy(comment, "Allows you to resize the screen by moving around the two analog sticks.\nPress TRIANGLE to reset to default values, and CIRCLE to go back to the\nin-game menu.");
				break;
			case MENU_ITEM_SCREENSHOT_MODE:
				if(CTRL_CROSS(state))
				{
					do
					{
						state = cell_pad_input_poll_device(0);
						if(CTRL_CIRCLE(state))
						{
							sys_timer_usleep(FILEBROWSER_DELAY);
							stuck_in_loop = 0;
						}

						Graphics->Draw(pix);
						psglSwap();
						cellSysutilCheckCallback();
						old_state = state;
					}while(stuck_in_loop && is_ingame_menu_running);
				}

				ingame_menu_reset_entry_colors (ingame_menu_item);
				strcpy(comment, "Allows you to take a screenshot without any text clutter.\nPress CIRCLE to go back to the in-game menu while in 'Screenshot Mode'.");
				break;
			case MENU_ITEM_RETURN_TO_GAME:
				if(CTRL_CROSS(button_was_pressed))
				{
					is_running = 1;
					ingame_menu_item = 0;
					is_ingame_menu_running = 0;
					Emulator_StartROMRunning(0);
				} 
				ingame_menu_reset_entry_colors (ingame_menu_item);
				strcpy(comment, "Press 'CROSS' to return back to the game.");
				break;
			case MENU_ITEM_RESET:
				if(CTRL_CROSS(button_was_pressed))
				{
					if(Settings.EmulatedSystem == IMAGE_GBA)
						CPUReset();
					else
						gbReset();

					is_running = 1;
					ingame_menu_item = 0;
					is_ingame_menu_running = 0;
					Emulator_StartROMRunning(0);
				} 
				ingame_menu_reset_entry_colors (ingame_menu_item);
				strcpy(comment, "Press 'CROSS' to reset the game.");
				break;
			case MENU_ITEM_RETURN_TO_MENU:
				if(CTRL_CROSS(button_was_pressed))
				{
					is_running = 1;
					ingame_menu_item = 0;
					is_ingame_menu_running = 0;
					mode_switch = MODE_MENU;
				}

				ingame_menu_reset_entry_colors (ingame_menu_item);
				strcpy(comment, "Press 'CROSS' to return to the ROM Browser menu.");
				break;
#ifdef MULTIMAN_SUPPORT
			case MENU_ITEM_RETURN_TO_MULTIMAN:
				if(CTRL_CROSS(button_was_pressed))
				{
					is_running = 0;
					is_ingame_menu_running = 0;
					mode_switch = MODE_EXIT;
				}

				ingame_menu_reset_entry_colors (ingame_menu_item);

				strcpy(comment, "Press 'CROSS' to quit the emulator and return to multiMAN.");
				break;
#endif
			case MENU_ITEM_RETURN_TO_XMB:
				if(CTRL_CROSS(button_was_pressed))
				{
					is_running = 0;
					is_ingame_menu_running = 0;
#ifdef MULTIMAN_SUPPORT
					return_to_MM = false;
#endif
					mode_switch = MODE_EXIT;
				}

				ingame_menu_reset_entry_colors (ingame_menu_item);

				strcpy(comment, "Press 'CROSS' to quit the emulator and return to the XMB.");
				break;
		}

		if(CTRL_UP(button_was_pressed) || CTRL_LSTICK_UP(button_was_pressed))
		{
			if(ingame_menu_item > 0)
				ingame_menu_item--;
		}

		if(CTRL_DOWN(button_was_pressed) || CTRL_LSTICK_DOWN(button_was_pressed))
		{
			if(ingame_menu_item < MENU_ITEM_LAST)
				ingame_menu_item++;
		}

#define x_position 0.3f
#define font_size 1.1f
#define ypos 0.19f
#define ypos_increment 0.04f
		cellDbgFontPrintf	(x_position,	0.10f,	1.4f+0.01f,	BLUE,               "Quick Menu");
		cellDbgFontPrintf	(x_position,	0.10f,	1.4f,	WHITE,               "Quick Menu");
		cellDbgFontPrintf	(x_position,	ypos,	font_size+0.01f,	BLUE,	"Load State #%d", Settings.CurrentSaveStateSlot);
		cellDbgFontPrintf	(x_position,	ypos,	font_size,	menuitem_colors[MENU_ITEM_LOAD_STATE],	"Load State #%d", Settings.CurrentSaveStateSlot);
		cellDbgFontPrintf	(x_position,	ypos+(ypos_increment*MENU_ITEM_SAVE_STATE),	font_size+0.01f,	BLUE,	"Save State #%d", Settings.CurrentSaveStateSlot);
		cellDbgFontPrintf	(x_position,	ypos+(ypos_increment*MENU_ITEM_SAVE_STATE),	font_size,	menuitem_colors[MENU_ITEM_SAVE_STATE],	"Save State #%d", Settings.CurrentSaveStateSlot);
		cellDbgFontPrintf	(x_position,	(ypos+(ypos_increment*MENU_ITEM_KEEP_ASPECT_RATIO)),	font_size+0.01f,	BLUE,	"Aspect Ratio: %s %s %d:%d", Graphics->calculate_aspect_ratio_before_game_load() ?"(Auto)" : "", Settings.PS3KeepAspect == LAST_ASPECT_RATIO ? "Custom" : "", (int)Graphics->get_aspect_ratio_int(0), (int)Graphics->get_aspect_ratio_int(1));
		cellDbgFontPrintf	(x_position,	(ypos+(ypos_increment*MENU_ITEM_KEEP_ASPECT_RATIO)),	font_size,	menuitem_colors[MENU_ITEM_KEEP_ASPECT_RATIO],	"Aspect Ratio: %s %s %d:%d", Graphics->calculate_aspect_ratio_before_game_load() ?"(Auto)" : "", Settings.PS3KeepAspect == LAST_ASPECT_RATIO ? "Custom" : "", (int)Graphics->get_aspect_ratio_int(0), (int)Graphics->get_aspect_ratio_int(1));
		cellDbgFontPrintf	(x_position,	(ypos+(ypos_increment*MENU_ITEM_OVERSCAN_AMOUNT)),	font_size+0.01f,	BLUE,	"Overscan: %f", (float)Settings.PS3OverscanAmount/100);
		cellDbgFontPrintf	(x_position,	(ypos+(ypos_increment*MENU_ITEM_OVERSCAN_AMOUNT)),	font_size,	menuitem_colors[MENU_ITEM_OVERSCAN_AMOUNT],	"Overscan: %f", (float)Settings.PS3OverscanAmount/100);
		cellDbgFontPrintf	(x_position,	(ypos+(ypos_increment*MENU_ITEM_RESIZE_MODE)),	font_size+0.01f,	BLUE,	"Resize Mode");
		cellDbgFontPrintf	(x_position,	(ypos+(ypos_increment*MENU_ITEM_RESIZE_MODE)),	font_size,	menuitem_colors[MENU_ITEM_RESIZE_MODE],	"Resize Mode");
		cellDbgFontPuts	(x_position,	(ypos+(ypos_increment*MENU_ITEM_FRAME_ADVANCE)),	font_size+0.01f,	BLUE,	"Frame Advance");
		cellDbgFontPuts	(x_position,	(ypos+(ypos_increment*MENU_ITEM_FRAME_ADVANCE)),	font_size,	menuitem_colors[MENU_ITEM_FRAME_ADVANCE],	"Frame Advance");
		cellDbgFontPuts	(x_position,	(ypos+(ypos_increment*MENU_ITEM_SCREENSHOT_MODE)),	font_size+0.01f,	BLUE,	"Screenshot Mode");
		cellDbgFontPuts	(x_position,	(ypos+(ypos_increment*MENU_ITEM_SCREENSHOT_MODE)),	font_size,	menuitem_colors[MENU_ITEM_SCREENSHOT_MODE],	"Screenshot Mode");
		cellDbgFontDraw();
		cellDbgFontPuts	(x_position,	(ypos+(ypos_increment*MENU_ITEM_RESET)),	font_size+0.01f,	BLUE,	"Reset");
		cellDbgFontPuts	(x_position,	(ypos+(ypos_increment*MENU_ITEM_RESET)),	font_size,	menuitem_colors[MENU_ITEM_RESET],	"Reset");
		cellDbgFontPuts   (x_position,   (ypos+(ypos_increment*MENU_ITEM_RETURN_TO_GAME)),   font_size+0.01f,  BLUE,  "Return to Game");
		cellDbgFontPuts   (x_position,   (ypos+(ypos_increment*MENU_ITEM_RETURN_TO_GAME)),   font_size,  menuitem_colors[MENU_ITEM_RETURN_TO_GAME],  "Return to Game");
		cellDbgFontPuts	(x_position,	(ypos+(ypos_increment*MENU_ITEM_RETURN_TO_MENU)),	font_size+0.01f,	BLUE,	"Return to Menu");
		cellDbgFontPuts	(x_position,	(ypos+(ypos_increment*MENU_ITEM_RETURN_TO_MENU)),	font_size,	menuitem_colors[MENU_ITEM_RETURN_TO_MENU],	"Return to Menu");
#ifdef MULTIMAN_SUPPORT
		cellDbgFontPuts	(x_position,	(ypos+(ypos_increment*MENU_ITEM_RETURN_TO_MULTIMAN)),	font_size+0.01f,	BLUE,	"Return to multiMAN");
		cellDbgFontPuts	(x_position,	(ypos+(ypos_increment*MENU_ITEM_RETURN_TO_MULTIMAN)),	font_size,	menuitem_colors[MENU_ITEM_RETURN_TO_MULTIMAN],	"Return to multiMAN");
#endif
		cellDbgFontPuts	(x_position,	(ypos+(ypos_increment*MENU_ITEM_RETURN_TO_XMB)),	font_size+0.01f,	BLUE,	"Return to XMB");
		cellDbgFontPuts	(x_position,	(ypos+(ypos_increment*MENU_ITEM_RETURN_TO_XMB)),	font_size,	menuitem_colors[MENU_ITEM_RETURN_TO_XMB],	"Return to XMB");
		if(Graphics->frame_count < special_action_msg_expired)
		{
			cellDbgFontPrintf (0.09f, 0.90f, 1.51f, BLUE,	special_action_msg);
			cellDbgFontPrintf (0.09f, 0.90f, 1.50f, WHITE,	special_action_msg);
			cellDbgFontDraw();
		}
		else
		{
			special_action_msg_expired = 0;
			cellDbgFontPrintf (0.09f,   0.90f,   0.98f+0.01f,      BLUE,           comment);
			cellDbgFontPrintf (0.09f,   0.90f,   0.98f,      LIGHTBLUE,           comment);
		}
		cellDbgFontDraw();
		psglSwap();
		old_state = state;
		cellSysutilCheckCallback();
	}while(is_ingame_menu_running);
}

static void emulator_start()
{
	if (Graphics->GetCurrentResolution() == CELL_VIDEO_OUT_RESOLUTION_576)
	{
		if(Graphics->CheckResolution(CELL_VIDEO_OUT_RESOLUTION_576))
		{
			if(!Graphics->GetPAL60Hz())
			{
				//PAL60 is OFF
				Settings.PS3PALTemporalMode60Hz = true;
				Graphics->SetPAL60Hz(Settings.PS3PALTemporalMode60Hz);
				Graphics->SwitchResolution(Graphics->GetCurrentResolution(), Settings.PS3PALTemporalMode60Hz, Settings.TripleBuffering, NULL, NULL);
			}
		}
	}

	if (reinit_video || Graphics->calculate_aspect_ratio_before_game_load())
	{
		if(Graphics->calculate_aspect_ratio_before_game_load())
			Graphics->set_aspect_ratio(Settings.PS3KeepAspect, srcWidth, srcHeight, 1);

		if(sgb_border_change)
		{
			vba_set_screen_dimensions();
			sgb_border_change = false;
		}

		Graphics->SetDimensions(srcWidth, srcHeight, (srcWidth * 4) + 4);
		reinit_video = false;
	}

	soundResume();

	if(Settings.EmulatedSystem == IMAGE_GBA)
	{
		do
		{
#ifdef USE_FRAMESKIP
			CPULoop(0);
#else
			CPULoop();
#endif
			systemReadJoypadGBA(0);

#ifdef CELL_DEBUG_CONSOLE
			cellConsolePoll();
#endif

			cellSysutilCheckCallback();
		}while(is_running);
	}
	else
	{
		do
		{
#ifdef USE_FRAMESKIP
			gbEmulate(0);
#else
			gbEmulate();
#endif
#ifdef CELL_DEBUG_CONSOLE
			cellConsolePoll();
#endif

			cellSysutilCheckCallback();
		}while(is_running);
	}

	soundPause();
}

void emulator_implementation_set_texture(const char * fname)
{
	strcpy(Settings.PS3CurrentBorder,fname);
	Graphics->LoadMenuTexture(PS3Graphics::TEXTURE_BACKDROP, fname);
	Graphics->LoadMenuTexture(PS3Graphics::TEXTURE_MENU, DEFAULT_MENU_BORDER_FILE);
}

static void get_path_settings(bool multiman_support)
{
	unsigned int get_type;
	unsigned int get_attributes;
	CellGameContentSize size;
	char dirName[CELL_GAME_DIRNAME_SIZE];

	memset(&size, 0x00, sizeof(CellGameContentSize));

	int ret = cellGameBootCheck(&get_type, &get_attributes, &size, dirName); 
	if(ret < 0)
	{
		printf("cellGameBootCheck() Error: 0x%x\n", ret);
	}
	else
	{
		printf("cellGameBootCheck() OK\n");
		printf("  get_type = [%d] get_attributes = [0x%08x] dirName = [%s]\n", get_type, get_attributes, dirName);
		printf("  hddFreeSizeKB = [%d] sizeKB = [%d] sysSizeKB = [%d]\n", size.hddFreeSizeKB, size.sizeKB, size.sysSizeKB);

		ret = cellGameContentPermit(contentInfoPath, usrDirPath);

		if(multiman_support)
		{
			snprintf(contentInfoPath, sizeof(contentInfoPath), "/dev_hdd0/game/%s", EMULATOR_CONTENT_DIR);
			snprintf(usrDirPath, sizeof(usrDirPath), "/dev_hdd0/game/%s/USRDIR", EMULATOR_CONTENT_DIR);
		}

		if(ret < 0)
		{
			printf("cellGameContentPermit() Error: 0x%x\n", ret);
		}
		else
		{
			printf("cellGameContentPermit() OK\n");
			printf("contentInfoPath:[%s]\n", contentInfoPath);
			printf("usrDirPath:[%s]\n",  usrDirPath);
		}

		/* now we fill in all the variables */
		snprintf(DEFAULT_PRESET_FILE, sizeof(DEFAULT_PRESET_FILE), "%s/presets/stock.conf", usrDirPath);
		snprintf(DEFAULT_BORDER_FILE, sizeof(DEFAULT_BORDER_FILE), "%s/borders/Centered-1080p/sonic-the-hedgehog-1.png", usrDirPath);
		snprintf(DEFAULT_MENU_BORDER_FILE, sizeof(DEFAULT_MENU_BORDER_FILE), "%s/borders/Menu/main-menu.png", usrDirPath);
		//snprintf(GAME_AWARE_SHADER_DIR_PATH, sizeof(GAME_AWARE_SHADER_DIR_PATH), "%s/gameaware\0", usrDirPath);
		snprintf(PRESETS_DIR_PATH, sizeof(PRESETS_DIR_PATH), "%s/presets", usrDirPath); 
		snprintf(BORDERS_DIR_PATH, sizeof(BORDERS_DIR_PATH), "%s/borders", usrDirPath); 
		snprintf(SHADERS_DIR_PATH, sizeof(SHADERS_DIR_PATH), "%s/shaders", usrDirPath);
		snprintf(DEFAULT_SHADER_FILE, sizeof(DEFAULT_SHADER_FILE), "%s/shaders/stock.cg", usrDirPath);
		snprintf(SYS_CONFIG_FILE, sizeof(SYS_CONFIG_FILE), "%s/vba.conf", usrDirPath);
		snprintf(DEFAULT_MENU_SHADER_FILE, sizeof(DEFAULT_MENU_SHADER_FILE), "%s/shaders/Borders/Menu/border-only.cg\0", usrDirPath);
	}
}

int main(int argc, char **argv)
{
	// Initialize 6 SPUs but reserve 1 SPU as a raw SPU for PSGL
	sys_spu_initialize(6, 1);

	cellSysmoduleLoadModule(CELL_SYSMODULE_FS);
	cellSysmoduleLoadModule(CELL_SYSMODULE_IO);
	cellSysmoduleLoadModule(CELL_SYSMODULE_AVCONF_EXT);
	cellSysmoduleLoadModule(CELL_SYSMODULE_PNGDEC);
	cellSysmoduleLoadModule(CELL_SYSMODULE_JPGDEC);
	cellSysmoduleLoadModule(CELL_SYSMODULE_SYSUTIL_GAME);

	cellSysutilRegisterCallback(0, sysutil_exit_callback, NULL);

#ifdef PS3_PROFILING
	net_stdio_set_target(PS3_PROFILING_IP, PS3_PROFILING_PORT);
	net_stdio_set_paths("/", 2);
	net_stdio_enable(0);
#endif

#ifdef MULTIMAN_SUPPORT
	return_to_MM = true;

	if(argc > 1)
	{
		mode_switch = MODE_MULTIMAN_STARTUP;	
		strncpy(MULTIMAN_GAME_TO_BOOT, argv[1], sizeof(MULTIMAN_GAME_TO_BOOT));
	}
#else
	mode_switch = MODE_MENU;
#endif

	get_path_settings(return_to_MM);

	emulator_init_settings();

#if(CELL_SDK_VERSION > 0x340000)
	if (Settings.ScreenshotsEnabled)
	{
		cellSysmoduleLoadModule(CELL_SYSMODULE_SYSUTIL_SCREENSHOT);
		CellScreenShotSetParam  screenshot_param = {0, 0, 0, 0};

		screenshot_param.photo_title = EMULATOR_NAME;
		screenshot_param.game_title = EMULATOR_NAME;
		cellScreenShotSetParameter (&screenshot_param);
		cellScreenShotEnable();
	}
#endif

	Graphics = new PS3Graphics((uint32_t)Settings.PS3CurrentResolution, Settings.PS3KeepAspect, Settings.PS3Smooth,  Settings.PS3Smooth2, Settings.PS3CurrentShader, Settings.PS3CurrentShader2, DEFAULT_MENU_SHADER_FILE, Settings.PS3OverscanEnabled, (float)Settings.PS3OverscanAmount/100, Settings.PS3PALTemporalMode60Hz, Settings.Throttled, Settings.TripleBuffering, Settings.ViewportX, Settings.ViewportY, Settings.ViewportWidth, Settings.ViewportHeight);
	Graphics->Init(Settings.ScaleEnabled, Settings.ScaleFactor);
	Graphics->InitDbgFont();

	cell_pad_input_init();

	oskutil_init(&oskutil_handle, 0);

	Graphics->ResetFrameCounter();
	vba_toggle_sgb_border(Settings.SGBBorders);

	emulator_toggle_sound(Settings.SoundMode);

	emulator_implementation_set_texture(Settings.PS3CurrentBorder);

	do
	{
		switch(mode_switch)
		{
			case MODE_MENU:
				MenuMainLoop();
				break;
			case MODE_EMULATION:
				if(ingame_menu_item != 0)
					is_ingame_menu_running = 1;

				emulator_start();

				if(is_ingame_menu_running)
					ingame_menu();

				break;
			case MODE_MULTIMAN_STARTUP:
				Emulator_StartROMRunning();
				Emulator_RequestLoadROM(MULTIMAN_GAME_TO_BOOT);
				break;
			case MODE_EXIT:
				emulator_shutdown();
		}
	}while(1);
}


void systemOnSoundShutdown()
{
	if (audio_handle)
	{
		audio_driver->free(audio_handle);
		audio_handle = NULL;
	}
}

void systemSoundNonblock(bool enable)
{
	audio_blocking = !enable;
}

void systemSoundSetThrottle(unsigned short throttle)
{
}

static void cb_dialog_ok(int button_type, void *userdata)
{
	switch(button_type)
	{
		case CELL_MSGDIALOG_BUTTON_ESCAPE:
			dialog_is_running = false;
			break;
	}
}

void emulator_toggle_sound(uint64_t soundmode)
{
	cell_audio_params params;
	memset(&params, 0, sizeof(params));
	params.channels=2;
	params.samplerate=48000;
	params.buffer_size=8192;
	params.sample_cb = NULL;
	params.userdata = NULL;

	switch(soundmode)
	{
		case SOUND_MODE_RSOUND:
			params.device = Settings.RSoundServerIPAddress;
			params.headset = 0;
			break;
		case SOUND_MODE_HEADSET:
			params.device = NULL;
			params.headset = 1;
			break;
		case SOUND_MODE_NORMAL: 
			params.device = NULL;
			params.headset = 0;
			break;
	}

      if(audio_handle)
      {
         audio_driver->free(audio_handle);
         audio_handle = NULL; 
      }
      if(soundmode == SOUND_MODE_RSOUND)
      {
		audio_driver = &cell_audio_rsound;
		audio_handle =  audio_driver->init(&params);

		if(!audio_handle || !(strlen(Settings.RSoundServerIPAddress) > 0))
		{
			audio_driver = &cell_audio_audioport;
			audio_handle = audio_driver->init(&params);

			Settings.SoundMode = SOUND_MODE_NORMAL;
			dialog_is_running = true;
			cellMsgDialogOpen2(CELL_MSGDIALOG_TYPE_SE_TYPE_ERROR|CELL_MSGDIALOG_TYPE_BG_VISIBLE|\
			CELL_MSGDIALOG_TYPE_BUTTON_TYPE_NONE|CELL_MSGDIALOG_TYPE_DISABLE_CANCEL_OFF|\
			CELL_MSGDIALOG_TYPE_DEFAULT_CURSOR_OK, "Couldn't connect to RSound server at specified IP address. Falling back to\
			regular audio.",cb_dialog_ok,NULL,NULL);

			do{
				glClear(GL_COLOR_BUFFER_BIT);
				psglSwap();
				cell_console_poll();
				cellSysutilCheckCallback();
			}while(dialog_is_running && is_running);
		}
      }
      else
      {
         audio_driver = &cell_audio_audioport;
         audio_handle =  audio_driver->init(&params);
      }
}

bool systemSoundInitDriver(long samplerate)
{
	emulator_toggle_sound(Settings.SoundMode);
	if(Settings.SoundMode == SOUND_MODE_RSOUND)
			audio_net = true;
	else
			audio_net = false;

	return true;
}


void systemSoundPause()
{
	audio_driver->pause(audio_handle);
}


void systemSoundReset()
{
	systemSoundInitDriver(0);
}


void systemSoundResume()
{
	audio_driver->unpause(audio_handle);
}

void systemOnWriteDataToSoundBuffer(int16_t * finalWave, int length)
{
	audio_driver->write(audio_handle,finalWave, length);
}
