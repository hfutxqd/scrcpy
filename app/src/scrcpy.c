#include "scrcpy.h"

static struct server server = SERVER_INITIALIZER;
static struct screen screen = SCREEN_INITIALIZER;
static struct frames frames;
static struct decoder decoder;
static struct controller controller;
static struct file_handler file_handler;
static struct recorder recorder;

static struct input_manager input_manager = {
    .controller = &controller,
    .frames = &frames,
    .screen = &screen,
};

#if defined(__APPLE__) || defined(__WINDOWS__)
# define CONTINUOUS_RESIZING_WORKAROUND
#endif

#ifdef CONTINUOUS_RESIZING_WORKAROUND
// On Windows and MacOS, resizing blocks the event loop, so resizing events are
// not triggered. As a workaround, handle them in an event handler.
//
// <https://bugzilla.libsdl.org/show_bug.cgi?id=2077>
// <https://stackoverflow.com/a/40693139/1987178>
static int event_watcher(void *data, SDL_Event *event) {
    if (event->type == SDL_WINDOWEVENT && event->window.event == SDL_WINDOWEVENT_RESIZED) {
        // called from another thread, not very safe, but it's a workaround!
        screen_render(&screen);
    }
    return 0;
}
#endif

static SDL_bool is_apk(const char *file) {
    const char *ext = strrchr(file, '.');
    return ext && !strcmp(ext, ".apk");
}

static SDL_bool event_loop(void) {
#ifdef CONTINUOUS_RESIZING_WORKAROUND
    SDL_AddEventWatch(event_watcher, NULL);
#endif
    SDL_Event event;
    while (SDL_WaitEvent(&event)) {
        switch (event.type) {
            case EVENT_DECODER_STOPPED:
                LOGD("Video decoder stopped");
                return SDL_FALSE;
            case SDL_QUIT:
                LOGD("User requested to quit");
                return SDL_TRUE;
            case EVENT_NEW_FRAME:
                if (!screen.has_frame) {
                    screen.has_frame = SDL_TRUE;
                    // this is the very first frame, show the window
                    screen_show_window(&screen);
                }
                if (!screen_update_frame(&screen, &frames)) {
                    return SDL_FALSE;
                }
                break;
            case SDL_WINDOWEVENT:
                switch (event.window.event) {
                    case SDL_WINDOWEVENT_EXPOSED:
                    case SDL_WINDOWEVENT_SIZE_CHANGED:
                        screen_render(&screen);
                        break;
                }
                break;
            case SDL_TEXTINPUT:
                input_manager_process_text_input(&input_manager, &event.text);
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                input_manager_process_key(&input_manager, &event.key);
                break;
            case SDL_MOUSEMOTION:
                input_manager_process_mouse_motion(&input_manager, &event.motion);
                break;
            case SDL_MOUSEWHEEL:
                input_manager_process_mouse_wheel(&input_manager, &event.wheel);
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                input_manager_process_mouse_button(&input_manager, &event.button);
                break;
            case SDL_DROPFILE: {
                file_handler_action_t action;
                if (is_apk(event.drop.file)) {
                    action = ACTION_INSTALL_APK;
                } else {
                    action = ACTION_PUSH_FILE;
                }
                file_handler_request(&file_handler, action, event.drop.file);
                break;
            }
        }
    }
    return SDL_FALSE;
}

static process_t set_show_touches_enabled(const char *serial, SDL_bool enabled) {
    const char *value = enabled ? "1" : "0";
    const char *const adb_cmd[] = {
        "shell", "settings", "put", "system", "show_touches", value
    };
    return adb_execute(serial, adb_cmd, ARRAY_LEN(adb_cmd));
}

static void wait_show_touches(process_t process) {
    // reap the process, ignore the result
    process_check_success(process, "show_touches");
}

static SDL_LogPriority sdl_priority_from_av_level(int level) {
    switch (level) {
        case AV_LOG_PANIC:
        case AV_LOG_FATAL:
            return SDL_LOG_PRIORITY_CRITICAL;
        case AV_LOG_ERROR:
            return SDL_LOG_PRIORITY_ERROR;
        case AV_LOG_WARNING:
            return SDL_LOG_PRIORITY_WARN;
        case AV_LOG_INFO:
            return SDL_LOG_PRIORITY_INFO;
    }
    // do not forward others, which are too verbose
    return 0;
}

static void
av_log_callback(void *avcl, int level, const char *fmt, va_list vl) {
    SDL_LogPriority priority = sdl_priority_from_av_level(level);
    if (priority == 0) {
        return;
    }
    char *local_fmt = SDL_malloc(strlen(fmt) + 10);
    if (!local_fmt) {
        LOGC("Cannot allocate string");
        return;
    }
    // strcpy is safe here, the destination is large enough
    strcpy(local_fmt, "[FFmpeg] ");
    strcpy(local_fmt + 9, fmt);
    SDL_LogMessageV(SDL_LOG_CATEGORY_VIDEO, priority, local_fmt, vl);
    SDL_free(local_fmt);
}

SDL_bool scrcpy(const struct scrcpy_options *options) {
    SDL_bool send_frame_meta = !!options->record_filename;
    if (!server_start(&server, options->serial, options->port,
                      options->max_size, options->bit_rate, options->crop,
                      send_frame_meta)) {
        return SDL_FALSE;
    }

    process_t proc_show_touches = PROCESS_NONE;
    SDL_bool show_touches_waited;
    if (options->show_touches) {
        LOGI("Enable show_touches");
        proc_show_touches = set_show_touches_enabled(options->serial, SDL_TRUE);
        show_touches_waited = SDL_FALSE;
    }

    SDL_bool ret = SDL_TRUE;

    if (!sdl_init_and_configure()) {
        ret = SDL_FALSE;
        goto finally_destroy_server;
    }

    socket_t device_socket = server_connect_to(&server);
    if (device_socket == INVALID_SOCKET) {
        server_stop(&server);
        ret = SDL_FALSE;
        goto finally_destroy_server;
    }

    char device_name[DEVICE_NAME_FIELD_LENGTH];
    struct size frame_size;

    // screenrecord does not send frames when the screen content does not change
    // therefore, we transmit the screen size before the video stream, to be able
    // to init the window immediately
    if (!device_read_info(device_socket, device_name, &frame_size)) {
        server_stop(&server);
        ret = SDL_FALSE;
        goto finally_destroy_server;
    }

    if (!frames_init(&frames)) {
        server_stop(&server);
        ret = SDL_FALSE;
        goto finally_destroy_server;
    }

    if (!file_handler_init(&file_handler, server.serial)) {
        ret = SDL_FALSE;
        server_stop(&server);
        goto finally_destroy_frames;
    }

    struct recorder *rec = NULL;
    if (options->record_filename) {
        if (!recorder_init(&recorder,
                           options->record_filename,
                           options->record_format,
                           frame_size)) {
            ret = SDL_FALSE;
            server_stop(&server);
            goto finally_destroy_file_handler;
        }
        rec = &recorder;
    }

    av_log_set_callback(av_log_callback);

    decoder_init(&decoder, &frames, device_socket, rec);

    // now we consumed the header values, the socket receives the video stream
    // start the decoder
    if (!decoder_start(&decoder)) {
        ret = SDL_FALSE;
        server_stop(&server);
        goto finally_destroy_recorder;
    }

    if (!controller_init(&controller, device_socket)) {
        ret = SDL_FALSE;
        goto finally_stop_decoder;
    }

    if (!controller_start(&controller)) {
        ret = SDL_FALSE;
        goto finally_destroy_controller;
    }

    if (!screen_init_rendering(&screen, device_name, frame_size, options->always_on_top)) {
        ret = SDL_FALSE;
        goto finally_stop_and_join_controller;
    }

    if (options->show_touches) {
        wait_show_touches(proc_show_touches);
        show_touches_waited = SDL_TRUE;
    }

    if (options->fullscreen) {
        screen_switch_fullscreen(&screen);
    }

    start_udp_server();

    ret = event_loop();
    LOGD("quit...");

    screen_destroy(&screen);

finally_stop_and_join_controller:
    controller_stop(&controller);
    controller_join(&controller);
finally_destroy_controller:
    controller_destroy(&controller);
finally_stop_decoder:
    decoder_stop(&decoder);
    // stop the server before decoder_join() to wake up the decoder
    server_stop(&server);
    decoder_join(&decoder);
finally_destroy_file_handler:
    file_handler_stop(&file_handler);
    file_handler_join(&file_handler);
    file_handler_destroy(&file_handler);
finally_destroy_recorder:
    if (options->record_filename) {
        recorder_destroy(&recorder);
    }
finally_destroy_frames:
    frames_destroy(&frames);
finally_destroy_server:
    if (options->show_touches) {
        if (!show_touches_waited) {
            // wait the process which enabled "show touches"
            wait_show_touches(proc_show_touches);
        }
        LOGI("Disable show_touches");
        proc_show_touches = set_show_touches_enabled(options->serial, SDL_FALSE);
        wait_show_touches(proc_show_touches);
    }

    server_destroy(&server);

    return ret;
}


void api_input_manager_process_text_input(const char* text) {
    LOGD("api text: %s\n", text);
    struct control_event control_event;
    control_event.type = CONTROL_EVENT_TYPE_TEXT;
    control_event.text_event.text = text;
    if (!control_event.text_event.text) {
        LOGW("Cannot strdup input text");
        return;
    }
    controller_push_event((&input_manager)->controller, &control_event);
}


static const int ACTION_DOWN = 1;
static const int ACTION_UP = 1 << 1;

void api_input_manager_process_keyevent(enum android_keycode keycode, int actions, const char *name) {
    struct control_event control_event;
    control_event.type = CONTROL_EVENT_TYPE_KEYCODE;
    control_event.keycode_event.keycode = keycode;
    control_event.keycode_event.metastate = 0;

    if (actions & ACTION_DOWN) {
        control_event.keycode_event.action = AKEY_EVENT_ACTION_DOWN;
        if (!controller_push_event((&input_manager)->controller, &control_event)) {
            LOGW("Cannot send %s (DOWN)", name);
            return;
        }
    }

    if (actions & ACTION_UP) {
        control_event.keycode_event.action = AKEY_EVENT_ACTION_UP;
        if (!controller_push_event((&input_manager)->controller, &control_event)) {
            LOGW("Cannot send %s (UP)", name);
        }
    }
}


void api_input_manager_process_touchevent(int x, int y, int w, int h, int action) {
    struct control_event control_event;
    control_event.type = CONTROL_EVENT_TYPE_MOUSE;

    control_event.mouse_event.action = action;
    control_event.mouse_event.buttons = 0;
    struct size screen_size;
    screen_size.width = w;
    screen_size.height = h;
    control_event.mouse_event.position.screen_size = screen_size;
    control_event.mouse_event.position.point.x = x;
    control_event.mouse_event.position.point.y = y;

    if (!controller_push_event((&input_manager)->controller, &control_event)) {
        LOGW("Cannot send touch event.");
        return;
    }
}

