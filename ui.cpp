/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <cutils/android_reboot.h>

#include "common.h"
#include "roots.h"
#include "device.h"
#include "minui/minui.h"
#include "screen_ui.h"
#include "ui.h"

#define UI_WAIT_KEY_TIMEOUT_SEC    120
#define ROTATION BOARD_RECOVERY_ROTATION

// There's only (at most) one of these objects, and global callbacks
// (for pthread_create, and the input event system) need to find it,
// so use a global variable.
static RecoveryUI* self = NULL;

RecoveryUI::RecoveryUI() :
    key_queue_len(0),
    key_last_down(-1),
    key_long_press(false),
    key_down_count(0),
    enable_reboot(true),
    consecutive_power_keys(0),
    consecutive_alternate_keys(0),
    last_key(-1),
    event_count(0),
    move_pile(0) {
    pthread_mutex_init(&key_queue_mutex, NULL);
    pthread_cond_init(&key_queue_cond, NULL);
    self = this;
    memset(key_pressed, 0, sizeof(key_pressed));
}

void RecoveryUI::Init() {
    lastEvent.x = lastEvent.y = lastEvent.point_id = 0;
    firstEvent.x = firstEvent.y = firstEvent.point_id = 0;
    for (int i = 0; i < 5; i++) {
         mTouchEvent[i].x = mTouchEvent[i].y = mTouchEvent[i].point_id = 0;
    }
    ev_init(input_callback, NULL);
	pthread_create(&event_t, NULL, watch_eventX_thread, NULL);
    pthread_create(&input_t, NULL, input_thread, NULL);
}

int RecoveryUI::menu_select(-1);

int RecoveryUI::touch_handle_input(input_event ev){
    int touch_code = 0;
    int rotation = ROTATION;
    if (ev.type == EV_ABS) {
        switch(ev.code) {
            case ABS_MT_TRACKING_ID:
                lastEvent.point_id = ev.value;
                event_count++;
                break;

            case ABS_MT_TOUCH_MAJOR:
                if (ev.value == 0) {
                }
                break;

            case ABS_MT_WIDTH_MAJOR:
                break;

            case ABS_MT_POSITION_X:
                if (ev.value != 0) {
                    if (90 == rotation) lastEvent.y = gr_fb_height() -ev.value;
                    else if (180 == rotation) lastEvent.x = gr_fb_width() - ev.value;
                    else if (270 == rotation) lastEvent.y = ev.value;
                    else lastEvent.x = ev.value;
                    event_count++;
                }

                break;
            case ABS_MT_POSITION_Y:
                if (ev.value != 0) {
                    if (90 == rotation) lastEvent.x = ev.value;
                    else if (180 == rotation) lastEvent.y = gr_fb_height() - ev.value;
                    else if (270 == rotation) lastEvent.x = gr_fb_width() - ev.value;
                    else lastEvent.y = ev.value;
                    event_count++;
                }

                break;
            default :
                break;
        }
        return 1;
    } else if (ev.type == EV_SYN) {
        //the down events have been catch,now,deal with its
        if (event_count == 3) {
            if(firstEvent.y == 0) {
                firstEvent.y = lastEvent.y;
            }
            if ((lastEvent.y - mTouchEvent[lastEvent.point_id].y) * move_pile < 0) {
                move_pile = 0;
                key_queue_len = 0;
            } else if (lastEvent.y != 0) {
                move_pile += lastEvent.y - mTouchEvent[lastEvent.point_id].y;
            }
            if (mTouchEvent[lastEvent.point_id].y == 0) {
                mTouchEvent[lastEvent.point_id].y = lastEvent.y;
            } else if ((lastEvent.y - mTouchEvent[lastEvent.point_id].y) > 20) {
                touch_code = Device::kHighlightDown;
                mTouchEvent[lastEvent.point_id].y = lastEvent.y;
            } else if ((lastEvent.y - mTouchEvent[lastEvent.point_id].y) < -20) {
                touch_code = Device::kHighlightUp;
                mTouchEvent[lastEvent.point_id].y = lastEvent.y;
            }
        }
        if (event_count == 0 && lastEvent.y != 0) { //the point move up
            if ((firstEvent.y == lastEvent.y)) {
                int *sreenPara = self->GetScreenPara();
                int select = lastEvent.y / sreenPara[2] - sreenPara[0] + sreenPara[3];
                if (select >= 0 && select < sreenPara[1] && select < sreenPara[3] + sreenPara[4]) {
                    menu_select = select;
                    touch_code = Device::kInvokeItem;
                }
            }
            lastEvent.x = lastEvent.y = lastEvent.point_id = 0;
            firstEvent.x = firstEvent.y = firstEvent.point_id = 0;
            for (int i = 0; i < 5; i++) {
                mTouchEvent[i].x = mTouchEvent[i].y = mTouchEvent[i].point_id = 0;
            }
        }
        if (ev.code == 0 && ev.value == 0) {
            event_count = 0;
        }
        pthread_mutex_lock(&key_queue_mutex);
        const int queue_max = sizeof(key_queue) / sizeof(key_queue[0]);
        if (key_queue_len < queue_max && touch_code != 0) {
            key_queue[key_queue_len++] = touch_code;
            pthread_cond_signal(&key_queue_cond);
        }
        pthread_mutex_unlock(&key_queue_mutex);
        return 0;
    }
    return 0;
}

int RecoveryUI::input_callback(int fd, uint32_t epevents, void* data)
{
    struct input_event ev;
    int ret;

    ret = ev_get_input(fd, epevents, &ev);
    if (ret)
        return -1;

#ifdef BOARD_TOUCH_RECOVERY
    if (self->touch_handle_input(ev))
        return 0;
#endif

    if (ev.type == EV_SYN) {
        return 0;
    } else if (ev.type == EV_REL) {
        if (ev.code == REL_Y) {
            // accumulate the up or down motion reported by
            // the trackball.  When it exceeds a threshold
            // (positive or negative), fake an up/down
            // key event.
            self->rel_sum += ev.value;
            if (self->rel_sum > 3) {
                self->process_key(KEY_DOWN, 1);   // press down key
                self->process_key(KEY_DOWN, 0);   // and release it
                self->rel_sum = 0;
            } else if (self->rel_sum < -3) {
                self->process_key(KEY_UP, 1);     // press up key
                self->process_key(KEY_UP, 0);     // and release it
                self->rel_sum = 0;
            }
        }
    } else {
        self->rel_sum = 0;
    }

    if (ev.type == EV_KEY && ev.code <= KEY_MAX)
        self->process_key(ev.code, ev.value);

    return 0;
}

// Process a key-up or -down event.  A key is "registered" when it is
// pressed and then released, with no other keypresses or releases in
// between.  Registered keys are passed to CheckKey() to see if it
// should trigger a visibility toggle, an immediate reboot, or be
// queued to be processed next time the foreground thread wants a key
// (eg, for the menu).
//
// We also keep track of which keys are currently down so that
// CheckKey can call IsKeyPressed to see what other keys are held when
// a key is registered.
//
// updown == 1 for key down events; 0 for key up events
void RecoveryUI::process_key(int key_code, int updown) {
    bool register_key = false;
    bool long_press = false;
    bool reboot_enabled;

    pthread_mutex_lock(&key_queue_mutex);
    key_pressed[key_code] = updown;
    if (updown) {
        ++key_down_count;
        key_last_down = key_code;
        key_long_press = false;
        pthread_t th;
        key_timer_t* info = new key_timer_t;
        info->ui = this;
        info->key_code = key_code;
        info->count = key_down_count;
        pthread_create(&th, NULL, &RecoveryUI::time_key_helper, info);
        pthread_detach(th);
    } else {
        if (key_last_down == key_code) {
            long_press = key_long_press;
            register_key = true;
        }
        key_last_down = -1;
    }
    reboot_enabled = enable_reboot;
    pthread_mutex_unlock(&key_queue_mutex);

    if (register_key) {
        NextCheckKeyIsLong(long_press);
        switch (CheckKey(key_code)) {
          case RecoveryUI::IGNORE:
            break;

          case RecoveryUI::TOGGLE:
            ShowText(!IsTextVisible());
            break;

          case RecoveryUI::REBOOT:
            if (reboot_enabled) {
                android_reboot(ANDROID_RB_RESTART, 0, 0);
            }
            break;

          case RecoveryUI::ENQUEUE:
            EnqueueKey(key_code);
            break;

          case RecoveryUI::MOUNT_SYSTEM:
#ifndef NO_RECOVERY_MOUNT
            ensure_path_mounted("/system");
            Print("Mounted /system.");
#endif
            break;
        }
    }
}

void* RecoveryUI::time_key_helper(void* cookie) {
    key_timer_t* info = (key_timer_t*) cookie;
    info->ui->time_key(info->key_code, info->count);
    delete info;
    return NULL;
}

void RecoveryUI::time_key(int key_code, int count) {
    usleep(750000);  // 750 ms == "long"
    bool long_press = false;
    pthread_mutex_lock(&key_queue_mutex);
    if (key_last_down == key_code && key_down_count == count) {
        long_press = key_long_press = true;
    }
    pthread_mutex_unlock(&key_queue_mutex);
    if (long_press) KeyLongPress(key_code);
}

void RecoveryUI::EnqueueKey(int key_code) {
    pthread_mutex_lock(&key_queue_mutex);
    const int queue_max = sizeof(key_queue) / sizeof(key_queue[0]);
    if (key_queue_len < queue_max) {
        key_queue[key_queue_len++] = key_code;
        pthread_cond_signal(&key_queue_cond);
    }
    pthread_mutex_unlock(&key_queue_mutex);
}


// Reads input events, handles special hot keys, and adds to the key queue.
void* RecoveryUI::input_thread(void *cookie)
{
    for (;;) {
        if (!ev_wait(-1))
            ev_dispatch();
    }
    return NULL;
}

void* RecoveryUI::watch_eventX_thread(void *cookie)
{
	if(ev_add_inotify()){
		fprintf(stderr,"ev_add_inotify failed\n");
		return NULL;
	}

    for (;;) {
		if(ev_select()){
			fprintf(stderr,"ev_select failed\n");
			return NULL;
		}
    }

    return NULL;
}

int RecoveryUI::WaitKey()
{
    pthread_mutex_lock(&key_queue_mutex);

    // Time out after UI_WAIT_KEY_TIMEOUT_SEC, unless a USB cable is
    // plugged in.
    do {
        struct timeval now;
        struct timespec timeout;
        gettimeofday(&now, NULL);
        timeout.tv_sec = now.tv_sec;
        timeout.tv_nsec = now.tv_usec * 1000;
        timeout.tv_sec += UI_WAIT_KEY_TIMEOUT_SEC;

        int rc = 0;
        while (key_queue_len == 0 && rc != ETIMEDOUT) {
            rc = pthread_cond_timedwait(&key_queue_cond, &key_queue_mutex,
                                        &timeout);
        }
    } while (usb_connected() && key_queue_len == 0);

    int key = -1;
    if (key_queue_len > 0) {
        key = key_queue[0];
        memcpy(&key_queue[0], &key_queue[1], sizeof(int) * --key_queue_len);
    }
    pthread_mutex_unlock(&key_queue_mutex);
    return key;
}

// Return true if USB is connected.
bool RecoveryUI::usb_connected() {
    int fd = open("/sys/class/android_usb/android0/state", O_RDONLY);
    if (fd < 0) {
        printf("failed to open /sys/class/android_usb/android0/state: %s\n",
               strerror(errno));
        return 0;
    }

    char buf;
    /* USB is connected if android_usb state is CONNECTED or CONFIGURED */
    int connected = (read(fd, &buf, 1) == 1) && (buf == 'C');
    if (close(fd) < 0) {
        printf("failed to close /sys/class/android_usb/android0/state: %s\n",
               strerror(errno));
    }
    return connected;
}

bool RecoveryUI::IsKeyPressed(int key)
{
    pthread_mutex_lock(&key_queue_mutex);
    int pressed = key_pressed[key];
    pthread_mutex_unlock(&key_queue_mutex);
    return pressed;
}

void RecoveryUI::FlushKeys() {
    pthread_mutex_lock(&key_queue_mutex);
    key_queue_len = 0;
    pthread_mutex_unlock(&key_queue_mutex);
}

// The default CheckKey implementation assumes the device has power,
// volume up, and volume down keys.
//
// - Hold power and press vol-up to toggle display.
// - Press power seven times in a row to reboot.
// - Alternate vol-up and vol-down seven times to mount /system.
RecoveryUI::KeyAction RecoveryUI::CheckKey(int key) {
    if ((IsKeyPressed(KEY_POWER) && key == KEY_VOLUMEUP) || key == KEY_HOME) {
        return TOGGLE;
    }

    if (key == KEY_POWER) {
        pthread_mutex_lock(&key_queue_mutex);
        bool reboot_enabled = enable_reboot;
        pthread_mutex_unlock(&key_queue_mutex);

        if (reboot_enabled) {
            ++consecutive_power_keys;
            if (consecutive_power_keys >= 7) {
                return REBOOT;
            }
        }
    } else {
        consecutive_power_keys = 0;
    }

    if ((key == KEY_VOLUMEUP &&
         (last_key == KEY_VOLUMEDOWN || last_key == -1)) ||
        (key == KEY_VOLUMEDOWN &&
         (last_key == KEY_VOLUMEUP || last_key == -1))) {
        ++consecutive_alternate_keys;
        if (consecutive_alternate_keys >= 7) {
            consecutive_alternate_keys = 0;
            return MOUNT_SYSTEM;
        }
    } else {
        consecutive_alternate_keys = 0;
    }
    last_key = key;

    return ENQUEUE;
}

void RecoveryUI::NextCheckKeyIsLong(bool is_long_press) {
}

void RecoveryUI::KeyLongPress(int key) {
}

void RecoveryUI::SetEnableReboot(bool enabled) {
    pthread_mutex_lock(&key_queue_mutex);
    enable_reboot = enabled;
    pthread_mutex_unlock(&key_queue_mutex);
}
