#ifndef SCRCPY_H
#define SCRCPY_H

#include <SDL2/SDL_stdinc.h>
#include <recorder.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <libavformat/avformat.h>
#include <sys/time.h>
#include <SDL2/SDL.h>

#include "command.h"
#include "common.h"
#include "controller.h"
#include "decoder.h"
#include "device.h"
#include "events.h"
#include "file_handler.h"
#include "frames.h"
#include "fps_counter.h"
#include "input_manager.h"
#include "log.h"
#include "lock_util.h"
#include "net.h"
#include "recorder.h"
#include "screen.h"
#include "server.h"
#include "tiny_xpm.h"
#include "udpserver.h"

struct scrcpy_options {
    const char *serial;
    const char *crop;
    const char *record_filename;
    enum recorder_format record_format;
    Uint16 port;
    Uint16 max_size;
    Uint32 bit_rate;
    SDL_bool show_touches;
    SDL_bool fullscreen;
    SDL_bool always_on_top;
};

SDL_bool scrcpy(const struct scrcpy_options *options);

void api_input_manager_process_text_input(const char* text);

void api_input_manager_process_keyevent(enum android_keycode keycode, int actions, const char *name);

void api_input_manager_process_touchevent(int x, int y, int w, int h, int actions);


#endif
