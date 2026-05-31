#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#define _USE_MATH_DEFINES
#include <math.h>

#if defined(_WIN32) && !defined(_XBOX)
#include <windows.h>
#endif
#include "libretro.h"

extern "C" {

// -----------------------------------------------------------------------------
// Core Configuration & Globals
// -----------------------------------------------------------------------------

// [BOILERPLATE] Toggle this to true for OpenGL cores, false for Software CPU cores.
constexpr bool CORE_USE_HW_RENDER = false;

constexpr unsigned maxWidth = 1024;
constexpr unsigned maxHeight = 768;
constexpr unsigned maxTotalPixels = maxWidth * maxHeight;

constexpr unsigned audioSampleRate = 44100;
constexpr unsigned fps = 60;
constexpr unsigned samplesPerFrame = audioSampleRate / fps;

static unsigned currentWidth = 320;
static unsigned currentHeight = 240;
static float currentAspect = (float)currentWidth / (float)currentHeight;

static struct retro_log_callback logging;
static retro_log_printf_t log_cb;

char system_directory[4096];
char save_directory[4096];
char retro_game_path[4096];

// --- Software Rendering Boilerplate ---
static uint32_t *frame_buf = nullptr;

// --- Hardware Rendering Boilerplate ---
static struct retro_hw_render_callback hw_render;
static bool gl_context_active = false;

// --- Save RAM & Save States ---
static uint8_t sram[0x2000];
struct CoreState {
   uint32_t frame_count;
   double internal_timer;
};
static CoreState core_state = {0, 0.0};

// -----------------------------------------------------------------------------
// Libretro Callbacks
// -----------------------------------------------------------------------------
static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

// -----------------------------------------------------------------------------
// Utilities
// -----------------------------------------------------------------------------
static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
   (void)level;
   va_list va;
   va_start(va, fmt);
   vfprintf(stderr, fmt, va);
   va_end(va);
}

static void show_message(const char* msg, unsigned frames)
{
   if (!environ_cb) return;
   struct retro_message retro_msg = { msg, frames };
   environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &retro_msg);
}

static void update_geometry(unsigned new_width, unsigned new_height, float new_aspect)
{
   currentWidth = new_width;
   currentHeight = new_height;
   currentAspect = new_aspect;

   struct retro_game_geometry geom;
   geom.base_width   = currentWidth;
   geom.base_height  = currentHeight;
   geom.max_width    = maxWidth;
   geom.max_height   = maxHeight;
   geom.aspect_ratio = currentAspect;

   if (environ_cb) environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geom);
}

// -----------------------------------------------------------------------------
// OpenGL Context Lifecycle Callbacks (Only used if CORE_USE_HW_RENDER = true)
// -----------------------------------------------------------------------------
static void context_reset(void)
{
   gl_context_active = true;
   if (log_cb) log_cb(RETRO_LOG_INFO, "OpenGL Context Reset/Created.\n");
   // [BOILERPLATE] Load GL functions here (e.g. via GLAD), create FBOs/Shaders
}

static void context_destroy(void)
{
   gl_context_active = false;
   if (log_cb) log_cb(RETRO_LOG_INFO, "OpenGL Context Destroyed.\n");
   // [BOILERPLATE] Destroy FBOs, VBOs, Shaders to prevent memory leaks
}

// -----------------------------------------------------------------------------
// Initialization & Environment Setup
// -----------------------------------------------------------------------------
RETRO_API void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

   if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
      log_cb = logging.log;
   else
      log_cb = fallback_log;

   bool no_rom = true;
   cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_rom);

   // [BOILERPLATE] Remove devices you don't intend to support
   static const struct retro_controller_description controllers[] = {
      { "RetroPad", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0) },
      { "RetroPad w/ Analog", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 0) },
      { "Mouse", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_MOUSE, 0) },
      { "Keyboard", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_KEYBOARD, 0) },
      { "Lightgun", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_LIGHTGUN, 0) },
      { "Pointer", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_POINTER, 0) },
   };

   static const struct retro_controller_info ports[] = {
      { controllers, 6 }, 
      { nullptr, 0 },
   };
   cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);

   // [BOILERPLATE] Example Core Options
   struct retro_variable variables[] = {
      { "template_option_1", "Sample Core Option; Enabled|Disabled" },
      { nullptr, nullptr },
   };
   cb(RETRO_ENVIRONMENT_SET_VARIABLES, variables);
}

RETRO_API void retro_init(void)
{
   if (!CORE_USE_HW_RENDER) {
      if (!frame_buf) frame_buf = new uint32_t[maxTotalPixels];
      memset(frame_buf, 0, maxTotalPixels * sizeof(uint32_t));
   }

   const char *sys_dir = nullptr;
   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sys_dir) && sys_dir) {
      snprintf(system_directory, sizeof(system_directory), "%s", sys_dir);
   }

   const char *sav_dir = nullptr;
   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &sav_dir) && sav_dir) {
      snprintf(save_directory, sizeof(save_directory), "%s", sav_dir);
   }

   memset(sram, 0, sizeof(sram));
}

RETRO_API void retro_deinit(void)
{
   if (!CORE_USE_HW_RENDER) {
      if (frame_buf) {
         delete[] frame_buf;
         frame_buf = nullptr;
      }
   }
}

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }
RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) {}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "Universal Template Core";
   info->library_version  = "1.0";
   info->need_fullpath    = true;
   info->valid_extensions = "bin|rom|iso"; 
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
   info->geometry.base_width   = currentWidth;
   info->geometry.base_height  = currentHeight;
   info->geometry.max_width    = maxWidth;
   info->geometry.max_height   = maxHeight;
   info->geometry.aspect_ratio = currentAspect;
   
   info->timing.fps            = fps;
   info->timing.sample_rate    = audioSampleRate;
}

RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }
RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }

RETRO_API void retro_reset(void)
{
   core_state.frame_count = 0;
   core_state.internal_timer = 0.0;
}

static void check_variables(void)
{
   struct retro_variable var = {0};
   var.key = "template_option_1";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      // Handle option changes
   }
}

static void keyboard_cb(bool down, unsigned keycode, uint32_t character, uint16_t mod) {}

RETRO_API void retro_run(void)
{
   if (input_poll_cb) input_poll_cb();
   
   // [BOILERPLATE] Handle Input (e.g. input_state_cb(...))
   
   // [BOILERPLATE] Emulate a frame
   core_state.frame_count++;

   // -------------------------------------------------------------------------
   // Video Output Path
   // -------------------------------------------------------------------------
   if (CORE_USE_HW_RENDER) {
      if (gl_context_active) {
         // [BOILERPLATE] Issue GL draw calls here
         
         // Tell frontend the HW frame is ready
         video_cb(RETRO_HW_FRAME_BUFFER_VALID, currentWidth, currentHeight, 0);
      }
   } else {
      // [BOILERPLATE] Write CPU pixels to frame_buf here

      // Send software buffer to frontend
      video_cb(frame_buf, currentWidth, currentHeight, currentWidth * sizeof(uint32_t));
   }

   // -------------------------------------------------------------------------
   // Audio Output Path
   // -------------------------------------------------------------------------
   int16_t audio_buf[samplesPerFrame * 2] = {0};
   // [BOILERPLATE] Fill audio_buf with samples here
   audio_batch_cb(audio_buf, samplesPerFrame);

   // Options Update
   bool updated = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
      check_variables();
   }
}

RETRO_API bool retro_load_game(const struct retro_game_info *info)
{
   // 1. Pixel Format
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) return false;

   // 2. Hardware Context Negotiation (If Enabled)
   if (CORE_USE_HW_RENDER) {
      hw_render.context_type       = RETRO_HW_CONTEXT_OPENGL_CORE; 
      hw_render.version_major      = 3; 
      hw_render.version_minor      = 3;
      hw_render.context_reset      = context_reset;
      hw_render.context_destroy    = context_destroy;
      hw_render.depth              = true;  
      hw_render.stencil            = false; 
      hw_render.bottom_left_origin = true;  

      if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render)) {
         if (log_cb) log_cb(RETRO_LOG_ERROR, "Failed to initialize OpenGL rendering.\n");
         return false; 
      }
   }

   // 3. Setup Input Descriptors (Used by frontend mapping menu)
   struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A (Right)" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      // [BOILERPLATE] Add remainder of descriptors here
      { 0 }, 
   };
   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

   // 4. Load Content
   if (info && info->path) {
      snprintf(retro_game_path, sizeof(retro_game_path), "%s", info->path);
      // [BOILERPLATE] Load ROM data from info->data or info->path here
   } else {
      retro_game_path[0] = '\0';
   }

   // 5. Connect Keyboard Hook
   struct retro_keyboard_callback kb_cb = { keyboard_cb };
   environ_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &kb_cb);

   check_variables();
   retro_reset();

   return true;
}

RETRO_API void retro_unload_game(void) {}
RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
RETRO_API bool retro_load_game_special(unsigned, const struct retro_game_info*, size_t) { return false; }

// -----------------------------------------------------------------------------
// Serialization & SRAM Callbacks
// -----------------------------------------------------------------------------
RETRO_API size_t retro_serialize_size(void) { return sizeof(CoreState) + sizeof(sram); }

RETRO_API bool retro_serialize(void *data, size_t size)
{
   if (size < retro_serialize_size()) return false;
   uint8_t *ptr = (uint8_t*)data;
   memcpy(ptr, &core_state, sizeof(CoreState)); ptr += sizeof(CoreState);
   memcpy(ptr, sram, sizeof(sram));
   return true;
}

RETRO_API bool retro_unserialize(const void *data, size_t size)
{
   if (size < retro_serialize_size()) return false;
   const uint8_t *ptr = (const uint8_t*)data;
   memcpy(&core_state, ptr, sizeof(CoreState)); ptr += sizeof(CoreState);
   memcpy(sram, ptr, sizeof(sram));
   return true;
}

RETRO_API void *retro_get_memory_data(unsigned id) { return (id == RETRO_MEMORY_SAVE_RAM) ? sram : nullptr; }
RETRO_API size_t retro_get_memory_size(unsigned id) { return (id == RETRO_MEMORY_SAVE_RAM) ? sizeof(sram) : 0; }
RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned, bool, const char*) {}

} // extern "C"