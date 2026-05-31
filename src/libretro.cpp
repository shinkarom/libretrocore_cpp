/**
 * Libretro C++ Core Template
 * -----------------------------------------------------------------------------
 * A fully functional, highly documented starting point for Libretro cores.
 * Features: Software/OpenGL toggle, Full RetroPad+Analog mapping, Push Audio, 
 * Dynamic Geometry, SRAM, Savestates, Core Options, and Contentless Booting.
 */

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

// =============================================================================
// Core Configuration & Globals
// =============================================================================

/* 
 * [HW_RENDER TOGGLE]
 * true  = Hardware rendering. Allocates no software buffer. Requires OpenGL.
 * false = Software rendering. Allocates a CPU framebuffer. 
 * The compiler will optimize away the unused code paths.
 */
constexpr bool CORE_USE_HW_RENDER = false;

/* 
 * [MAX RESOLUTION]
 * To support dynamic resolution changes mid-game without dangerous memory 
 * reallocations, we allocate one buffer large enough for the MAXIMUM possible 
 * resolution the core will ever output.
 */
constexpr unsigned maxWidth = 1024;
constexpr unsigned maxHeight = 768;
constexpr unsigned maxTotalPixels = maxWidth * maxHeight;

/*
 * [AUDIO PARAMS]
 * Libretro frontends expect audio to be perfectly synced to the video framerate.
 * 44100 Hz / 60 FPS = 735 audio frames per video frame.
 * Since audio is STEREO, the actual buffer will hold 735 * 2 = 1470 int16 samples.
 */
constexpr unsigned audioSampleRate = 44100;
constexpr unsigned fps = 60;
constexpr unsigned samplesPerFrame = audioSampleRate / fps;

/* Active viewport geometry. Update these and call update_geometry() to change dynamically. */
static unsigned currentWidth = 320;
static unsigned currentHeight = 240;
static float currentAspect = (float)currentWidth / (float)currentHeight;

static struct retro_log_callback logging;
static retro_log_printf_t log_cb;

char system_directory[4096]; // Path to the system/BIOS folder
char save_directory[4096];   // Path to the frontend's save folder
char retro_game_path[4096];  // Path to the currently loaded ROM

// --- Software Rendering Variables ---
static uint32_t *frame_buf = nullptr; // 32-bit XRGB8888 buffer

// --- Hardware Rendering Variables ---
static struct retro_hw_render_callback hw_render;
static bool gl_context_active = false;

// --- Save RAM & Save States ---
static uint8_t sram[0x2000]; // Dummy 8KB Save RAM

/* 
 * [SAVESTATE STRUCT]
 * Pack everything your emulator needs to restore its exact state into a flat struct.
 * WARNING: Never put pointers in this struct (e.g., `uint8_t* memory`), as the 
 * memory addresses will be invalid when restoring a savestate later. Use arrays or offsets.
 */
struct CoreState {
   uint32_t frame_count;
   double internal_timer;
};
static CoreState core_state = {0, 0.0};

// =============================================================================
// Libretro Callbacks (Set by the frontend)
// =============================================================================
static retro_environment_t environ_cb;           // Query frontend features/settings
static retro_video_refresh_t video_cb;           // Push video frames to frontend
static retro_audio_sample_batch_t audio_batch_cb;// Push audio frames to frontend
static retro_input_poll_t input_poll_cb;         // Ask frontend to update controller state
static retro_input_state_t input_state_cb;       // Read specific controller buttons

// Legacy single-sample audio callback. Provided for API compliance, but audio_batch_cb is preferred.
static retro_audio_sample_t audio_cb; 

// =============================================================================
// Utilities
// =============================================================================

// Fallback logger if the frontend doesn't provide one
static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
   (void)level;
   va_list va;
   va_start(va, fmt);
   vfprintf(stderr, fmt, va);
   va_end(va);
}

// Helper to draw OSD text on the screen
static void show_message(const char* msg, unsigned frames)
{
   if (!environ_cb) return;
   struct retro_message retro_msg = { msg, frames };
   environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &retro_msg);
}

// Helper to change the resolution/aspect ratio mid-game
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

// Helper to generate a 440Hz test tone (Stereo Interleaved)
static void generate_audio(int16_t *buffer, size_t num_frames)
{
   static double phase = 0.0;
   const double phase_increment = (2.0 * M_PI * 440.0) / audioSampleRate;

   for (size_t i = 0; i < num_frames; i++) {
      int16_t sample = (int16_t)(0x800 * sin(phase));
      buffer[i * 2 + 0] = sample; // Left Channel
      buffer[i * 2 + 1] = sample; // Right Channel

      phase += phase_increment;
      if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
   }
}

// =============================================================================
// OpenGL Context Lifecycle (Only triggered if CORE_USE_HW_RENDER = true)
// =============================================================================

/*
 * Called when the frontend creates a GL context, or recreates it after it was lost.
 * (e.g., user minimized the window, alt-tabbed, or toggled fullscreen).
 * Action: Load GL function pointers (via GLAD/GLEW), compile shaders, create FBOs.
 */
static void context_reset(void)
{
   gl_context_active = true;
   if (log_cb) log_cb(RETRO_LOG_INFO, "OpenGL Context Reset/Created.\n");
}

/*
 * Called before the frontend destroys the GL context.
 * Action: Delete all textures, FBOs, and shaders here. If you don't, you will memory leak!
 */
static void context_destroy(void)
{
   gl_context_active = false;
   if (log_cb) log_cb(RETRO_LOG_INFO, "OpenGL Context Destroyed.\n");
}

// =============================================================================
// Libretro Core Lifecycle
// =============================================================================

/* 
 * Step 1: Called first. Use this to negotiate features with the frontend.
 * Memory allocation should NOT happen here.
 */
RETRO_API void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

   if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging)) log_cb = logging.log;
   else log_cb = fallback_log;

   // Allow the core to start without a ROM (useful for booting directly to a BIOS menu)
   bool no_rom = true;
   cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_rom);

   // Declare the input devices this core supports
   static const struct retro_controller_description controllers[] = {
      { "RetroPad", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0) },
      { "RetroPad w/ Analog", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 0) },
      { "Mouse", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_MOUSE, 0) },
      { "Keyboard", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_KEYBOARD, 0) },
      { "Lightgun", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_LIGHTGUN, 0) },
      { "Pointer", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_POINTER, 0) },
   };

   static const struct retro_controller_info ports[] = {
      { controllers, 6 }, { nullptr, 0 },
   };
   cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);

   // Declare frontend user-settings (Core Options)
   struct retro_variable variables[] = {
      { "template_option_1", "Sample Core Option; Enabled|Disabled" },
      { nullptr, nullptr },
   };
   cb(RETRO_ENVIRONMENT_SET_VARIABLES, variables);
}

/* 
 * Step 2: Called when the core is initialized. 
 * Action: Allocate main memory, buffers, and fetch directory paths.
 */
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

// Setup basic libretro info (version, name, supported extensions)
RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }
RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) {}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "Universal Template Core";
   info->library_version  = "1.0";
   info->need_fullpath    = true; // If true, info->path in load_game is an absolute path.
   info->valid_extensions = "bin|rom|iso"; 
}

// Define default resolution and timing
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

// Hook frontend callbacks to our local static variables
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }
RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }

RETRO_API void retro_reset(void)
{
   // Emulate console 'Reset' button press here
   core_state.frame_count = 0;
   core_state.internal_timer = 0.0;
}

// Helper to apply Core Option changes
static void check_variables(void)
{
   struct retro_variable var = {0};
   var.key = "template_option_1";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      // var.value contains the string selected by the user (e.g. "Enabled")
   }
}

// Hardware Keyboard hook (Used for emulating home computers, not joypads)
static void keyboard_cb(bool down, unsigned keycode, uint32_t character, uint16_t mod) {}

/*
 * Step 4: The Main Execution Loop. Called once per frame (e.g. 60 times a second).
 * MUST execute in this exact order: Poll Input -> Emulate -> Video Output -> Audio Output.
 */
RETRO_API void retro_run(void)
{
   // 1. Mandatory Input Poll. Frontend will not update input states without this.
   if (input_poll_cb) input_poll_cb();
   
   // 2. Read Inputs (Example)
   // bool btn_start = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START);
   
   // 3. Emulate one frame of the system here
   core_state.frame_count++;

   // 4. Video Output
   if (CORE_USE_HW_RENDER) {
      if (gl_context_active) {
         // (Issue GL draw calls here to the default framebuffer 0)
         
         // RETRO_HW_FRAME_BUFFER_VALID tells the frontend the frame is already on the GPU.
         // Pitch is ignored (0) for hardware contexts.
         video_cb(RETRO_HW_FRAME_BUFFER_VALID, currentWidth, currentHeight, 0);
      }
   } else {
      // For software rendering, pitch = bytes per row. 
      // For 32bpp XRGB8888, pitch is (width * 4).
      video_cb(frame_buf, currentWidth, currentHeight, currentWidth * sizeof(uint32_t));
   }

   // 5. Audio Output (Push Method)
   // Buffer must be int16_t, interleaved Stereo (Left, Right, Left, Right...)
   int16_t audio_buf[samplesPerFrame * 2];
   generate_audio(audio_buf, samplesPerFrame); 
   audio_batch_cb(audio_buf, samplesPerFrame);

   // 6. Check if user changed settings in the frontend menu
   bool updated = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
      check_variables();
   }
}

/*
 * Step 3: Called to load a specific game.
 * Action: Set pixel formats, HW context (if needed), input mappings, and load the ROM to memory.
 */
RETRO_API bool retro_load_game(const struct retro_game_info *info)
{
   // Request 32-bit color format (0x00RRGGBB). Mandatory for most modern cores.
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) return false;

   // Negotiate OpenGL context if hardware rendering is enabled
   if (CORE_USE_HW_RENDER) {
      hw_render.context_type       = RETRO_HW_CONTEXT_OPENGL_CORE; 
      hw_render.version_major      = 3; // Request GL 3.3 Core Profile
      hw_render.version_minor      = 3;
      hw_render.context_reset      = context_reset;
      hw_render.context_destroy    = context_destroy;
      hw_render.depth              = true;  // Request Depth Buffer
      hw_render.stencil            = false; 
      hw_render.bottom_left_origin = true;  

      if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render)) {
         if (log_cb) log_cb(RETRO_LOG_ERROR, "Failed to initialize OpenGL rendering.\n");
         return false; 
      }
   }

   // Describe controller layout. This lets the frontend UI map buttons cleanly.
   struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A (Right)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B (Down)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "X (Up)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Y (Left)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "L1 / Left Bumper" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "R1 / Right Bumper" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "L2 / Left Trigger" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "R2 / Right Trigger" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,     "L3 / Left Stick Click" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,     "R3 / Right Stick Click" },
      
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },
      { 0 }, 
   };
   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

   /* 
    * If 'SUPPORT_NO_GAME' is true, info might be NULL. 
    * Always check before attempting to copy path strings! 
    */
   if (info && info->path) {
      snprintf(retro_game_path, sizeof(retro_game_path), "%s", info->path);
      // Load ROM data via file I/O (info->path) or directly from RAM (info->data).
   } else {
      retro_game_path[0] = '\0';
   }

   struct retro_keyboard_callback kb_cb = { keyboard_cb };
   environ_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &kb_cb);

   check_variables();
   retro_reset();

   return true;
}

RETRO_API void retro_unload_game(void) {}
RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

// Used for subsystem loading (e.g. loading a Super Game Boy cartridge + Super Game Boy ROM)
RETRO_API bool retro_load_game_special(unsigned, const struct retro_game_info*, size_t) { return false; }

// =============================================================================
// Serialization (Savestates & Rewind)
// =============================================================================

// Must return the exact size in bytes required to store your emulator's entire state.
RETRO_API size_t retro_serialize_size(void) { 
   return sizeof(CoreState) + sizeof(sram); 
}

// Writes current state into the provided generic byte buffer 'data'
RETRO_API bool retro_serialize(void *data, size_t size)
{
   if (size < retro_serialize_size()) return false;
   uint8_t *ptr = (uint8_t*)data;
   
   // Copy variables sequentially into the buffer
   memcpy(ptr, &core_state, sizeof(CoreState)); 
   ptr += sizeof(CoreState);
   
   memcpy(ptr, sram, sizeof(sram));
   
   return true;
}

// Restores state from the provided generic byte buffer 'data'
RETRO_API bool retro_unserialize(const void *data, size_t size)
{
   if (size < retro_serialize_size()) return false;
   const uint8_t *ptr = (const uint8_t*)data;
   
   // Extract variables sequentially from the buffer
   memcpy(&core_state, ptr, sizeof(CoreState)); 
   ptr += sizeof(CoreState);
   
   memcpy(sram, ptr, sizeof(sram));
   
   return true;
}

// =============================================================================
// Save Data (SRAM / Memory Cards)
// =============================================================================

// Frontend calls this to get a pointer to the in-game save data. 
// When the game unloads, the frontend automatically writes this memory to disk.
RETRO_API void *retro_get_memory_data(unsigned id) { 
   return (id == RETRO_MEMORY_SAVE_RAM) ? sram : nullptr; 
}

RETRO_API size_t retro_get_memory_size(unsigned id) { 
   return (id == RETRO_MEMORY_SAVE_RAM) ? sizeof(sram) : 0; 
}

// =============================================================================
// Cheats
// =============================================================================
RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned, bool, const char*) {}

} // extern "C"