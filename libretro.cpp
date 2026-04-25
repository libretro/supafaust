#include <mednafen/mednafen.h>
#include <mednafen/string/string.h>
#include <mednafen/MemoryStream.h>
#include <mednafen/ExtMemStream.h>
#include <mednafen/FileStream.h>
#include <mednafen/MThreading.h>

#include "libretro.h"

using namespace Mednafen;
//
//
//
static struct
{
 retro_environment_t environment;
 retro_video_refresh_t video_refresh;
 retro_audio_sample_t audio_sample;
 retro_audio_sample_batch_t audio_sample_batch;
 retro_input_poll_t input_poll;
 retro_input_state_t input_state;
 //
 retro_log_printf_t log;
} cb;

static int Initialized = 0;
static bool libretro_supports_bitmasks = false;

// MDFN_NOTICE_STATUS, MDFN_NOTICE_WARNING, MDFN_NOTICE_ERROR
void Mednafen::MDFND_OutputNotice(MDFN_NoticeType t, const char* s)
{
 if(cb.log && Initialized)
 {
  retro_log_level rll = RETRO_LOG_DEBUG;

  switch(t)
  {
   case MDFN_NOTICE_STATUS: rll = RETRO_LOG_INFO; break;
   case MDFN_NOTICE_WARNING: rll = RETRO_LOG_WARN; break;
   case MDFN_NOTICE_ERROR: rll = RETRO_LOG_ERROR; break;
  }
  cb.log(rll, "%s", s);
 }
 else
  printf("%s\n", s);
}

// Output from MDFN_printf(); fairly verbose informational messages.
void Mednafen::MDFND_OutputInfo(const char* s)
{
 if(cb.log)
  cb.log(RETRO_LOG_INFO, "%s", s);
 else
  fputs(s, stdout);
}

//
//
//
static MDFNGI *cgi = nullptr;
static std::unique_ptr<int32[]> lw;
static std::unique_ptr<MDFN_Surface> surf;
static uint8* port_data[16] = { 0 };
static size_t ports_active;

static double SoundRate;
static int32 SoundBufMaxSize;
static std::unique_ptr<int16[]> SoundBuf;

static size_t SaveStateSize;

static std::vector<std::string> Cheats;
static bool CheatsChanged;

static uint64 affinity_save;
//
//
//
static MDFN_COLD void Cleanup(void)
{
 if(cgi)
 {
  MDFNI_CloseGame();
  cgi = nullptr;
 }

 lw.reset(nullptr);
 surf.reset(nullptr);

 for(size_t i = 0; i < 16; i++)
  port_data[i] = nullptr;

 SoundBufMaxSize = 0;
 SoundBuf.reset(nullptr);

 if(affinity_save)
 {
  MThreading::Thread_SetAffinity(nullptr, affinity_save);
  //
  affinity_save = 0;
 }
}

//
//
//

static const struct retro_variable options[] =
{
 { "supafaust_pixel_format", "Pixel format; rgb565|xrgb8888|0rgb1555" },
 { "supafaust_correct_aspect", "Correct pixel aspect ratio; enabled|disabled|force_ntsc|force_pal" },
 { "supafaust_h_filter", "Horizontal blend/double filter; phr256blend_auto512|phr256blend_512|512_blend|512|phr256blend" },
 { "supafaust_deinterlacer", "Deinterlacer; bob_offset|weave|blend" },

 { "supafaust_slstart", "First displayed scanline in NTSC mode; 0|1|2|3|4|5|6|7|8|9|10|11|12|13|14|15|16" },
 { "supafaust_slend", "Last displayed scanline in NTSC mode; 223|222|221|220|219|218|217|216|215|214|213|212|211|210|209|208|207" },

 { "supafaust_slstartp", "First displayed scanline in PAL mode; 0|1|2|3|4|5|6|7|8|9|10|11|12|13|14|15|16|17|18|19|20|21|22|23|24" },
 { "supafaust_slendp", "Last displayed scanline in PAL mode; 238|237|236|235|234|233|232|231|230|229|228|227|226|225|224|223|222|221|220|219|218|217|216|215|214" },

 { "supafaust_region", "Region/Type of SNES to emulate; auto|ntsc|pal|ntsc_lie_auto|pal_lie_auto|ntsc_lie_pal|pal_lie_ntsc" },

 { "supafaust_frame_begin_vblank", "Begin frame in SNES VBlank to lower latency; enabled|disabled" },
 { "supafaust_run_ahead", "Enable 1-frame run-ahead; disabled|video|video+audio" },

 { "supafaust_multitap", "Enable multitap; disabled|port1|port2|port1+port2" },

 { "supafaust_cx4_clock_rate", "CX4 clock rate %; 100|125|150|175|200|250|300|400|500" },
 { "supafaust_superfx_clock_rate", "SuperFX clock rate %; 100|125|150|175|200|250|300|400|500|95" },
 { "supafaust_superfx_icache", "Emulate SuperFX instruction cache; disabled|enabled" },

 { "supafaust_audio_rate", "Internal resampler(output rate); disabled|44100|48000|96000" },

 { "supafaust_thread_affinity_emu", "Emulation thread affinity mask; 0x2|0x0|0x1|0x3|0x4|0x5|0x6|0x7|0x8|0x9|0xa|0xb|0xc|0xd|0xe|0xf" }, // Affinity is set in retro_load_game() and restored in retro_unload_game().  Set to 0 to disable changing affinity.
 { "supafaust_thread_affinity_ppu", "PPU render thread affinity mask; 0x1|0x0|0x2|0x3|0x4|0x5|0x6|0x7|0x8|0x9|0xa|0xb|0xc|0xd|0xe|0xf" },

 { "supafaust_renderer", "PPU renderer; mt|st" },

 { NULL, NULL }
};

static std::map<std::string, std::string> options_defaults;

MDFN_COLD RETRO_API void retro_set_environment(retro_environment_t v)
{
 cb.environment = v;
 cb.environment(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)options);
}

MDFN_COLD RETRO_API void retro_set_video_refresh(retro_video_refresh_t v)
{
 cb.video_refresh = v;
}

MDFN_COLD RETRO_API void retro_set_audio_sample(retro_audio_sample_t v)
{
 cb.audio_sample = v;
}

MDFN_COLD RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t v)
{
 cb.audio_sample_batch = v;
}

MDFN_COLD RETRO_API void retro_set_input_poll(retro_input_poll_t v)
{
 cb.input_poll = v;
}

MDFN_COLD RETRO_API void retro_set_input_state(retro_input_state_t v)
{
 cb.input_state = v;
}

static const char* get_option(const char* lr_name)
{
 struct retro_variable rv;

 rv.key = lr_name;
 rv.value = NULL;

 cb.environment(RETRO_ENVIRONMENT_GET_VARIABLE, &rv);
 if(!rv.value)
 {
  rv.value = options_defaults[lr_name].c_str();
  fprintf(stderr, "RETRO_ENVIRONMENT_GET_VARIABLE for \"%s\" failed, using default of \"%s\".\n", lr_name, rv.value);
 }

 return rv.value;
}

static long long get_option_signed(const char* lr_name)
{
 const char* v = get_option(lr_name);
 long long ret;
 char* eptr = NULL;

 if(!MDFN_strazicmp(v, "disabled"))
  return 0;
 else if(!MDFN_strazicmp(v, "enabled"))
  return 1;

 ret = strtoll(v, &eptr, 0);

 if(eptr == v || !eptr || *eptr != 0)
 {
  fprintf(stderr, "Invalid integer for option \"%s\": %s", lr_name, v);
  abort();
 }

 return ret;
}

static unsigned long long get_option_unsigned(const char* lr_name)
{
 const char* v = get_option(lr_name);
 unsigned long long ret;
 char* eptr = NULL;

 if(!MDFN_strazicmp(v, "disabled"))
  return 0;
 else if(!MDFN_strazicmp(v, "enabled"))
  return 1;

 ret = strtoull(v, &eptr, 0);

 if(eptr == v || !eptr || *eptr != 0)
 {
  fprintf(stderr, "Invalid integer for option \"%s\": %s", lr_name, v);
  abort();
 }

 return ret;
}

static void pipe_option(const char* lr_name, const char* mdfn_name)
{
 MDFNI_SetSetting(mdfn_name, get_option(lr_name));
}

static void pipe_option_bool(const char* lr_name, const char* mdfn_name)
{
 int v = get_option_signed(lr_name);

 MDFNI_SetSetting(mdfn_name, v ? "1" : "0");
}

static void check_variables(bool loaded)
{
   pipe_option("supafaust_slstart", "snes_faust.slstart");
   pipe_option("supafaust_slend", "snes_faust.slend");
   pipe_option("supafaust_slstartp", "snes_faust.slstartp");
   pipe_option("supafaust_slendp", "snes_faust.slendp");
}

MDFN_COLD RETRO_API void retro_init(void)
{
 assert(!Initialized);
 //
 MDFNI_Initialize();

 //
 assert(cb.environment);

 {
  //
  retro_log_callback lcb;

  memset(&lcb, 0, sizeof(lcb));
  if(cb.environment(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &lcb))
  {
   cb.log = lcb.log;
  }
 }
 //
 if (cb.environment(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
 {
  libretro_supports_bitmasks = true;
 }
 //
 {
  uint64 squirks = RETRO_SERIALIZATION_QUIRK_ENDIAN_DEPENDENT | RETRO_SERIALIZATION_QUIRK_PLATFORM_DEPENDENT;

  cb.environment(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS, &squirks);
 }
 //
 Initialized = 1;
}

MDFN_COLD RETRO_API void retro_deinit(void)
{
 assert(Initialized == 1);
 //
 MDFNI_Kill();
 //
 libretro_supports_bitmasks = false;
 //
 Initialized = -1;
}

RETRO_API unsigned retro_api_version(void)
{
 return RETRO_API_VERSION;
}

MDFN_COLD RETRO_API void retro_get_system_info(retro_system_info* info)
{
 assert(info);
 //
 memset(info, 0, sizeof(*info));
 info->library_name = "Beetle Supafaust";
 info->library_version = MEDNAFEN_VERSION GIT_VERSION;
 info->valid_extensions = "smc|swc|sfc|fig";
 info->need_fullpath = false;
 info->block_extract = false;
}

MDFN_COLD RETRO_API void retro_get_system_av_info(retro_system_av_info* info)
{
 assert(cgi);
 assert(info);
 //
 info->geometry.base_width = cgi->nominal_width;
 info->geometry.base_height = cgi->nominal_height;
 info->geometry.max_width = cgi->fb_width;
 info->geometry.max_height = cgi->fb_height;

 info->geometry.aspect_ratio = (float)cgi->nominal_width / cgi->nominal_height;
 //
 info->timing.fps = (double)cgi->fps / (65536 * 256);
 info->timing.sample_rate = SoundRate;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{

}

RETRO_API void retro_reset(void)
{
 MDFNI_Reset();
}

static void UpdateInput(void)
{
 cb.input_poll();

 for(size_t port = 0; port < ports_active; port++)
 {
   uint16 bs;
   if (libretro_supports_bitmasks)
   {
    bs = cb.input_state(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
   }
   else
   {
    bs = 0;
    //
    for(unsigned i = RETRO_DEVICE_ID_JOYPAD_B; i < (RETRO_DEVICE_ID_JOYPAD_R3 + 1); i++)
     bs |= (bool)cb.input_state(port, RETRO_DEVICE_JOYPAD, 0, i) << i;
    //
  }
  MDFN_en16lsb<false>(port_data[port], bs);
 }
}

void Mednafen::MDFND_MidSync(const EmulateSpecStruct* espec, const unsigned flags)
{
 const int16* const sbuf = espec->SoundBuf + espec->SoundBufSizeALMS;
 const int32 scount = espec->SoundBufSize - espec->SoundBufSizeALMS;

 if(scount)
  cb.audio_sample_batch(sbuf, scount);
 //
 if(flags & MIDSYNC_FLAG_UPDATE_INPUT)
  UpdateInput();
}

static INLINE MDFN_PixelFormat RPFtoMPF(const retro_pixel_format rpf)
{
 MDFN_PixelFormat ret;

 if(rpf == RETRO_PIXEL_FORMAT_RGB565)
 {
  ret = MDFN_PixelFormat::RGB16_565;
 }
 else if(rpf == RETRO_PIXEL_FORMAT_0RGB1555)
 {
  ret = MDFN_PixelFormat::IRGB16_1555;
 }
 else if(rpf == RETRO_PIXEL_FORMAT_XRGB8888)
 {
  ret = MDFN_PixelFormat::ARGB32_8888;
 }
 else
 {
  assert(0);
 }

 return ret;
}

static NO_INLINE void DoFrame(MDFN_Surface* s)
{
 UpdateInput();
 //
 EmulateSpecStruct espec;
 //int lr_avenable = 0x3;

 //if(!cb.environment(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &lr_avenable))
 // lr_avenable = 0x3;

 lw[0] = ~0;	// Must be ~0 and not another value.
 espec.surface = s;
 espec.LineWidths = lw.get();
 espec.skip = false; //!(lr_avenable & 0x1);		// Skips drawing the frame if true; espec.surface and espec.LineWidths must still be valid when true, however.

 espec.SoundRate = SoundRate;
 espec.SoundBuf = SoundBuf.get(); // Should have enough room for 100ms of sound to be on the safe side.
 espec.SoundBufMaxSize = SoundBufMaxSize;

 MDFNI_Emulate(&espec);
 //
 //
 //
 if(espec.surface->format.opp == 2)
 {
  cb.video_refresh(espec.surface->pix<uint16>() + espec.DisplayRect.x + espec.DisplayRect.y * espec.surface->pitchinpix,
	(lw[0] == ~0) ? espec.DisplayRect.w : lw[espec.DisplayRect.y],
	espec.DisplayRect.h,
	espec.surface->pitchinpix * sizeof(uint16));
 }
 else
 {
  cb.video_refresh(espec.surface->pix<uint32>() + espec.DisplayRect.x + espec.DisplayRect.y * espec.surface->pitchinpix,
	(lw[0] == ~0) ? espec.DisplayRect.w : lw[espec.DisplayRect.y],
	espec.DisplayRect.h,
	espec.surface->pitchinpix * sizeof(uint32));
 }
}

RETRO_API void retro_run(void)
{
 if(MDFN_UNLIKELY(CheatsChanged))
 {
  MDFNI_DelCheats();
  //
  MemoryPatch mp;

  for(size_t i = 0; i < Cheats.size(); i++)
  {
   for(auto const& cfie : cgi->CheatInfo.CheatFormatInfo)
   {
     if(!cfie.DecodeCheat(Cheats[i], &mp))
     {
      MDFNI_AddCheat(mp);
      //
      mp = MemoryPatch();
     }
     break;
   }
  }

  CheatsChanged = false;
 }
 //
 //
 //
  DoFrame(surf.get());

  bool updated = false;
  if (cb.environment(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
  {
      check_variables(true);
  }
}

RETRO_API size_t retro_serialize_size(void)
{
 return SaveStateSize;
}

RETRO_API bool retro_serialize(void* data, size_t size)
{
 ExtMemStream ssms(data, size);
 MDFNSS_SaveSM(&ssms, true);
 return true;
}

RETRO_API bool retro_unserialize(const void* data, size_t size)
{
 ExtMemStream ssms(data, size);
 MDFNSS_LoadSM(&ssms, true);
 return true;
}

MDFN_COLD RETRO_API bool retro_load_game(const retro_game_info* game)
{
 assert(game);
 assert(game->data);
 //
  MDFN_PixelFormat nf = MDFN_PixelFormat::ARGB32_8888;

  ports_active = 2;
  //
  options_defaults.clear();
  for(const struct retro_variable* opt = options; opt->key; opt++)
  {
   std::string v;

   for(size_t i = 0; opt->value[i]; i++)
   {
    if(opt->value[i] == ';')
    {
     do { i++; } while(MDFN_isspace(opt->value[i]));
     //
     size_t j;
     for(j = i; opt->value[j] && opt->value[j] != '|'; j++)
     {
      //
     }
     options_defaults[opt->key] = std::string(opt->value + i, j - i);
     break;
    }
   }
  }
  //
  pipe_option("supafaust_correct_aspect", "snes_faust.correct_aspect");
  pipe_option("supafaust_h_filter", "snes_faust.h_filter");
  pipe_option("supafaust_deinterlacer", "video.deinterlacer");
  pipe_option("supafaust_slstart", "snes_faust.slstart");
  pipe_option("supafaust_slend", "snes_faust.slend");
  pipe_option("supafaust_slstartp", "snes_faust.slstartp");
  pipe_option("supafaust_slendp", "snes_faust.slendp");
  pipe_option("supafaust_region", "snes_faust.region");
  pipe_option("supafaust_thread_affinity_ppu", "snes_faust.affinity.ppu");
  pipe_option("supafaust_renderer", "snes_faust.renderer");
  pipe_option_bool("supafaust_frame_begin_vblank", "snes_faust.frame_begin_vblank");
  //
  //
  {
   const char* const v = get_option("supafaust_pixel_format");
   retro_pixel_format rpf;

   if(!MDFN_strazicmp(v, "rgb565"))
    rpf = RETRO_PIXEL_FORMAT_RGB565;
   else if(!MDFN_strazicmp(v, "xrgb8888"))
    rpf = RETRO_PIXEL_FORMAT_XRGB8888;
   else
    rpf = RETRO_PIXEL_FORMAT_0RGB1555;

   nf = RPFtoMPF(rpf);

   cb.environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &rpf);
  }
  //
  //
  {
   static const struct
   {
    const char* lrv;
    const char* spex;
    const char* spex_sound;
   } ttab[] =
   {
    { "disabled",	"0", "0" },
    { "video",		"1", "0" },
    { "video+audio",	"1", "1" },
    { NULL }
   };

   const char* const v = get_option("supafaust_run_ahead");

   for(auto const& tte : ttab)
   {
    if(!tte.lrv)
    {
     fprintf(stderr, "Bad value for option supafaust_run_ahead.");
     abort(); 
    }

    if(!MDFN_strazicmp(v, tte.lrv))
    {
     MDFNI_SetSetting("snes_faust.spex", tte.spex);
     MDFNI_SetSetting("snes_faust.spex.sound", tte.spex_sound);
     break;
    }
   }
  }
  //
  //
  {
   struct retro_input_descriptor input_descriptors[] = {
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Y" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "X" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R" },

    { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B" },
    { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Y" },
    { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
    { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
    { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up" },
    { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down" },
    { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left" },
    { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
    { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A" },
    { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "X" },
    { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L" },
    { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R" },

    { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B" },
    { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Y" },
    { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
    { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
    { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up" },
    { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down" },
    { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left" },
    { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
    { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A" },
    { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "X" },
    { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L" },
    { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R" },

    { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B" },
    { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Y" },
    { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
    { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
    { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up" },
    { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down" },
    { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left" },
    { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
    { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A" },
    { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "X" },
    { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L" },
    { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R" },

    {0},
   };
   
   cb.environment(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, input_descriptors);
  }
  //
  //
  {
   static const struct
   {
    const char* lrv;
    const char* sport1;
    const char* sport2;
    size_t ports_active;
   } ttab[] =
   {
    { "disabled",    "0", "0", 2 },
    { "port1",	     "1", "0", 5 },
    { "port2",	     "0", "1", 5 },
    { "port1+port2", "1", "1", 8 }, 

    { NULL }
   };

   const char* const v = get_option("supafaust_multitap");

   for(auto const& tte : ttab)
   {
    if(!tte.lrv)
    {
     fprintf(stderr, "Bad value for option supafaust_multitap.");
     abort(); 
    }

    if(!MDFN_strazicmp(v, tte.lrv))
    {
     MDFNI_SetSetting("snes_faust.input.sport1.multitap", tte.sport1);
     MDFNI_SetSetting("snes_faust.input.sport2.multitap", tte.sport2);
     ports_active = tte.ports_active;
     break;
    }
   }
  }

  pipe_option("supafaust_cx4_clock_rate", "snes_faust.cx4.clock_rate");
  pipe_option("supafaust_superfx_clock_rate", "snes_faust.superfx.clock_rate");
  pipe_option_bool("supafaust_superfx_icache", "snes_faust.superfx.icache");

  MDFNI_SetSetting("snes_faust.renderer", "mt");
  //
  //
  //
  std::string dir, fbase, ext = ".sfc";
  
  if (game->path)
   NVFS.get_file_path_components(game->path, &dir, &fbase, &ext);

  ExtMemStream gs(game->data, game->size);
  GameFile gf({&NVFS, dir, &gs, MDFN_strazlower(ext.size() ? ext.substr(1) : ext), fbase, { &NVFS, dir, fbase }});
  cgi = MDFNI_LoadGame(&gf);
  //
  //
  for(size_t i = 0; i < ports_active; i++)
  {
   port_data[i] = MDFNI_SetInput(i/*port*/, 1/*device type id, gamepad*/);
  }

  SoundRate = get_option_signed("supafaust_audio_rate");
  if(!SoundRate)
   SoundRate = cgi->IdealSoundRate;
  assert(SoundRate);
  SoundBufMaxSize = ceil((SoundRate + 9) / 10);
  SoundBuf.reset(new int16[SoundBufMaxSize * cgi->soundchan]);
  lw.reset(new int32[cgi->fb_height]);
  memset(lw.get(), 0, sizeof(int32) * cgi->fb_height);
  surf.reset(new MDFN_Surface(nullptr, cgi->fb_width, cgi->fb_height, cgi->fb_width, nf));
  //
  {
   MemoryStream ssms(65536);
   MDFNSS_SaveSM(&ssms, true);

   SaveStateSize = ssms.size(); // FIXME for other than snes_faust, may grow.
   SaveStateSize += 64*2*4*2; // OwlBuffer-related kludge, leftover, 2 channels(l/r), 4 bytes per sample, *2 again for MSU1
  }
  //
  {
   const uint64 affinity = get_option_unsigned("supafaust_thread_affinity_emu");

   if(affinity != 0)
   {
    affinity_save = MThreading::Thread_SetAffinity(nullptr, affinity);
   }
  }
  //
  options_defaults.clear();

 return true;
}

MDFN_COLD RETRO_API bool retro_load_game_special(unsigned game_type, const retro_game_info* info, size_t num_info)
{
 return false;
}

MDFN_COLD RETRO_API void retro_unload_game(void)
{
 Cleanup();
}

RETRO_API unsigned retro_get_region(void)
{
 assert(cgi);
 //
 unsigned ret;

 switch(cgi->VideoSystem)
 {
  default:
  case VIDSYS_NONE:
  case VIDSYS_NTSC:
  case VIDSYS_PAL_M:
	ret = RETRO_REGION_NTSC;
	break;

  case VIDSYS_PAL:
  case VIDSYS_SECAM:
	ret = RETRO_REGION_PAL;
	break;

/*
  case VIDSYS_PAL_M:
  case VIDSYS_SECAM:
	ret = ~0U;
	break;
*/
 }

 return ret;
}

namespace MDFN_IEN_SNES_FAUST
{
 MDFN_COLD uint8* GetNV(uint32* size);
}

RETRO_API void* retro_get_memory_data(unsigned id)
{
 assert(cgi);
 if(/*!cgi->GetNV ||*/ id != RETRO_MEMORY_SAVE_RAM)
  return nullptr;
 //
 uint32 size;

 return MDFN_IEN_SNES_FAUST::GetNV(&size); //cgi->GetNV(&size);
}

RETRO_API size_t retro_get_memory_size(unsigned id)
{
 assert(cgi);
 if(/*!cgi->GetNV ||*/ id != RETRO_MEMORY_SAVE_RAM)
  return 0;
 //
 uint32 size = 0;

 MDFN_IEN_SNES_FAUST::GetNV(&size); //cgi->GetNV(&size);

 return size;
}

MDFN_COLD RETRO_API void retro_cheat_reset(void)
{
 Cheats.clear();
 CheatsChanged = true;
}

MDFN_COLD RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char* code)
{
 if(index >= Cheats.size())
  Cheats.resize(index + 1);
 //
 if(!enabled)
  Cheats[index] = "";
 else
  Cheats[index] = code;
 //
 CheatsChanged = true;
}
