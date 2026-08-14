#ifndef _DEF_WT_H_
#define _DEF_WT_H_

#include <vector>
#include <SDL.h>

#define APP_NAME "WifiTransfer"

// Hardware Config (R36S / Device)
#if defined(DEVICE_R36S)
   #define SCREEN_WIDTH             640
   #define SCREEN_HEIGHT            480
   #define HARDWARE_ACCELERATION    0
   #define FULLSCREEN               1
   #define FONT_NAME                "NotoSans-Regular.ttf"
   #define FONT_NAME_MONO           "NotoSansMono-Regular.ttf"
   #define FONT_SIZE                20
   #define LINE_HEIGHT              32
   #define MARGIN_X                 10
// Fallback for PC
#else
   #define SCREEN_WIDTH             640
   #define SCREEN_HEIGHT            480
   #define HARDWARE_ACCELERATION    1
   #define FULLSCREEN               0
   #define FONT_NAME                "NotoSans-Regular.ttf"
   #define FONT_NAME_MONO           "NotoSansMono-Regular.ttf"
   #define FONT_SIZE                20
   #define LINE_HEIGHT              32
   #define MARGIN_X                 10
#endif

// Colors (Cyberpunk Theme)
#define COLOR_TITLE_BG           20,  20,  30
#define COLOR_TEXT_NORMAL        255, 255, 255
#define COLOR_TEXT_CYAN          0,   255, 255
#define COLOR_TEXT_MAGENTA       255, 0,   255
#define COLOR_TEXT_YELLOW        255, 255, 0
#define COLOR_TEXT_RED           255, 50,  50
#define COLOR_BODY_BG            10,  10,  15
#define COLOR_SELECTION_BG       0,   120, 255

// Application Settings
#define MS_PER_FRAME             16
#define SERVER_PORT              8000

// Button events (R36S uses regular joystick buttons/hat for Dpad)
#if defined(DEVICE_R36S)
   #define BUTTON_PRESSED_A               event.type == SDL_JOYBUTTONDOWN && event.jbutton.button == 0
   #define BUTTON_PRESSED_B               event.type == SDL_JOYBUTTONDOWN && event.jbutton.button == 1
   #define BUTTON_PRESSED_X               event.type == SDL_JOYBUTTONDOWN && event.jbutton.button == 2
   #define BUTTON_PRESSED_Y               event.type == SDL_JOYBUTTONDOWN && event.jbutton.button == 3
   
   // DPad on R36S could be hat or buttons (13-16) or axis
   #define BUTTON_PRESSED_UP              (event.type == SDL_JOYHATMOTION && event.jhat.value == SDL_HAT_UP) || (event.type == SDL_JOYBUTTONDOWN && event.jbutton.button == 13)
   #define BUTTON_PRESSED_DOWN            (event.type == SDL_JOYHATMOTION && event.jhat.value == SDL_HAT_DOWN) || (event.type == SDL_JOYBUTTONDOWN && event.jbutton.button == 14)
   
#else
   #define BUTTON_PRESSED_A               event.type == SDL_KEYDOWN && event.key.repeat == 0 && event.key.keysym.sym == SDLK_RETURN
   #define BUTTON_PRESSED_B               event.type == SDL_KEYDOWN && event.key.repeat == 0 && event.key.keysym.sym == SDLK_BACKSPACE
   #define BUTTON_PRESSED_X               event.type == SDL_KEYDOWN && event.key.repeat == 0 && event.key.keysym.sym == SDLK_x
   #define BUTTON_PRESSED_Y               event.type == SDL_KEYDOWN && event.key.repeat == 0 && event.key.keysym.sym == SDLK_y
   
   #define BUTTON_PRESSED_UP              event.type == SDL_KEYDOWN && event.key.repeat == 0 && event.key.keysym.sym == SDLK_UP
   #define BUTTON_PRESSED_DOWN            event.type == SDL_KEYDOWN && event.key.repeat == 0 && event.key.keysym.sym == SDLK_DOWN
#endif

// Globals declaration
extern SDL_Window* g_window;
extern SDL_Renderer* g_renderer;
extern SDL_Joystick* g_joystick;

#endif
