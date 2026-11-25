// ============================================================================
// LINUX DIGITIZER EVENT PUMP & VIRTUAL UINPUT DEVICE DRIVER
// ============================================================================
// Project: crack_master
// Target Architecture: ARM64-v8a Linux Multi-Touch Input Subsystem
// Reference Implementation: Kernel Core Native Surface Engine
//
// SYSTEM ROLE:
// 1. Digitizer Detection:
//    Scans /dev/input/event0 .. event32 to find active multi-touch touchscreen devices
//    matching ABS_MT_POSITION_X / ABS_MT_POSITION_Y and INPUT_PROP_DIRECT.
// 2. Dual Touch Routing & Sticky Swallow:
//    - In-UI Touches: Delivers touch coordinates to Dear ImGui io.MouseDown and MousePos.
//      Swallow decision is latched on finger down and held until finger release.
//      isDown is NEVER cleared inside UI, ensuring 100% continuous dragging.
//    - In-Game Touches: Forwarded directly through the virtual uinput node so player
//      controls (movement joystick, fire button, jump) function with zero interruption.
// 3. Symmetrical Cleanup:
//    Touch_Close() releases digitizer grabs and closes all virtual device file descriptors.
// ============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <cmath>
#include <algorithm>
#include <mutex>
#include <linux/input.h>
#include <linux/uinput.h>

#include "imgui.h"

#define maxE 5
#define maxF 10
#define UNGRAB 0
#define GRAB 1

#ifndef INPUT_PROP_DIRECT
#define INPUT_PROP_DIRECT 0x01
#endif
#ifndef INPUT_PROP_MAX
#define INPUT_PROP_MAX 0x1f
#endif

static uint32_t orientation = 0;
static float screenHeight = 0, screenWidth = 0;

struct touchObj {
    bool isDown = false;
    bool isTmpDown = false;
    // Sticky swallow: set while a gesture that began inside a UI rect is still
    // held. Without it, dragging the floating logo past its own hitbox handed
    // the finger to uinput mid-drag and the game camera spun.
    bool swallow = false;
    // True between finger-down and the first SYN_REPORT of that gesture. The
    // swallow decision is taken once, here, so a game drag that wanders into a
    // UI rect is not hijacked halfway through.
    bool freshDown = false;
    int x = 0;
    int y = 0;
    int id = 0;
};

struct targ {
    int fdNum;
    float maxRawX;
    float maxRawY;
};

static struct {
    input_event downEvent[2]{{{}, EV_KEY, BTN_TOUCH,       1},
                             {{}, EV_KEY, BTN_TOOL_FINGER, 1}};
    input_event event[512]{0};
} input;

static targ targF[maxE];
static touchObj Finger[maxE][maxF];
static int fdNum = 0, origfd[maxE] = {-1, -1, -1, -1, -1}, nowfd = -1;
static pthread_t s_touchThreads[maxE] = {0};
static float s_digitizerMaxX = 0.0f, s_digitizerMaxY = 0.0f;

static bool Touch_initialized = false;
static bool Touch_readOnly = false;

static bool checkDeviceIsTouch(int fd, float &outMaxX, float &outMaxY);

static void genRandomString(char *string, int length) {
    int flag, i;
    srand((unsigned) time(NULL) + length);
    for (i = 0; i < length - 1; i++) {
        flag = rand() % 3;
        switch (flag) {
            case 0:
                string[i] = 'A' + rand() % 26;
                break;
            case 1:
                string[i] = 'a' + rand() % 26;
                break;
            case 2:
                string[i] = '0' + rand() % 10;
                break;
            default:
                string[i] = 'x';
                break;
        }
    }
    string[length - 1] = '\0';
}

static std::mutex s_uploadMutex;

// Multiple UI rects can be live at once (floating logo + menu + future panels).
// The render loop clears the pending list once per frame, each UI component
// appends its region, then Touch_CommitUiRects() publishes the set. Publishing
// by swap means the touch thread never sees a momentarily empty list, and a
// closed menu can no longer leave a stale rect swallowing taps on the logo.
static std::mutex s_uiRectMutex;
static constexpr int kMaxUiRects = 8;

struct UiRect {
    float x, y, w, h;
    // Non-zero = treated as a circle: (x,y) is the top-left of the bounding
    // box, radius is r. The floating logo is a circle but its bounding box
    // has four ~46x46 px dead corners that used to swallow game taps.
    float r;
};

static UiRect s_uiRects[kMaxUiRects];   // published, read by touch thread
static int s_uiRectCount = 0;
static UiRect s_uiPending[kMaxUiRects]; // staged by render thread
static int s_uiPendingCount = 0;

void Touch_ClearUiRects() {
    s_uiPendingCount = 0;
}

void Touch_AddUiRect(float x, float y, float w, float h) {
    if (s_uiPendingCount >= kMaxUiRects) return;
    s_uiPending[s_uiPendingCount].x = x;
    s_uiPending[s_uiPendingCount].y = y;
    s_uiPending[s_uiPendingCount].w = w;
    s_uiPending[s_uiPendingCount].h = h;
    s_uiPending[s_uiPendingCount].r = 0.0f;
    ++s_uiPendingCount;
}

void Touch_AddUiCircle(float cx, float cy, float radius) {
    if (s_uiPendingCount >= kMaxUiRects) return;
    s_uiPending[s_uiPendingCount].x = cx - radius;
    s_uiPending[s_uiPendingCount].y = cy - radius;
    s_uiPending[s_uiPendingCount].w = radius * 2.0f;
    s_uiPending[s_uiPendingCount].h = radius * 2.0f;
    s_uiPending[s_uiPendingCount].r = radius;
    ++s_uiPendingCount;
}

void Touch_CommitUiRects() {
    std::lock_guard<std::mutex> lock(s_uiRectMutex);
    for (int i = 0; i < s_uiPendingCount; ++i) {
        s_uiRects[i] = s_uiPending[i];
    }
    s_uiRectCount = s_uiPendingCount;
}

bool Touch_IsPointInUi(float x, float y) {
    std::lock_guard<std::mutex> lock(s_uiRectMutex);
    for (int i = 0; i < s_uiRectCount; ++i) {
        const UiRect& r = s_uiRects[i];
        if (r.r > 0.0f) {
            // Circular region: the logo. Bounding-box hits in the corners are
            // not UI, so the tap goes through to the game instead of dying on
            // an invisible square.
            const float cx = r.x + r.r;
            const float cy = r.y + r.r;
            const float dx = x - cx;
            const float dy = y - cy;
            if (dx * dx + dy * dy <= r.r * r.r) return true;
            continue;
        }
        if (x >= r.x && x <= (r.x + r.w) &&
            y >= r.y && y <= (r.y + r.h)) {
            return true;
        }
    }
    return false;
}

static void Upload() {
    if (Touch_readOnly || nowfd <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(s_uploadMutex);
    static bool isFirstDown = true;
    int tmpCnt = 0, tmpCnt2 = 0, i, j;

    // If every held finger belongs to the overlay, there is nothing to forward.
    // Writing here would emit a synthetic BTN_TOUCH=0 and the game would see a
    // phantom finger-up mid-drag.
    bool anySwallowed = false;
    bool anyForwardable = false;
    for (i = 0; i < fdNum; i++) {
        for (j = 0; j < maxF; j++) {
            if (Finger[i][j].isDown) {
                if (Finger[i][j].swallow) anySwallowed = true;
                else anyForwardable = true;
            }
        }
    }
    if (anySwallowed && !anyForwardable) {
        return;
    }

    for (i = 0; i < fdNum; i++) {
        for (j = 0; j < maxF; j++) {
            // Swallowed fingers never reach uinput: the gesture belongs to the
            // overlay, and forwarding it would drive the game camera as well.
            if (Finger[i][j].isDown && !Finger[i][j].swallow) {
                if (tmpCnt2++ > 10) {
                    goto finish;
                }
                input.event[tmpCnt].type = EV_ABS;
                input.event[tmpCnt].code = ABS_X;
                input.event[tmpCnt].value = Finger[i][j].x;
                tmpCnt++;

                input.event[tmpCnt].type = EV_ABS;
                input.event[tmpCnt].code = ABS_Y;
                input.event[tmpCnt].value = Finger[i][j].y;
                tmpCnt++;

                input.event[tmpCnt].type = EV_ABS;
                input.event[tmpCnt].code = ABS_MT_POSITION_X;
                input.event[tmpCnt].value = Finger[i][j].x;
                tmpCnt++;

                input.event[tmpCnt].type = EV_ABS;
                input.event[tmpCnt].code = ABS_MT_POSITION_Y;
                input.event[tmpCnt].value = Finger[i][j].y;
                tmpCnt++;

                input.event[tmpCnt].type = EV_ABS;
                input.event[tmpCnt].code = ABS_MT_TRACKING_ID;
                input.event[tmpCnt].value = Finger[i][j].id;
                tmpCnt++;

                input.event[tmpCnt].type = EV_SYN;
                input.event[tmpCnt].code = SYN_MT_REPORT;
                input.event[tmpCnt].value = 0;
                tmpCnt++;
            }
        }
    }
    finish:
    bool is = false;
    if (tmpCnt == 0) {
        input.event[tmpCnt].type = EV_SYN;
        input.event[tmpCnt].code = SYN_MT_REPORT;
        input.event[tmpCnt].value = 0;
        tmpCnt++;
        if (!isFirstDown) {
            isFirstDown = true;
            input.event[tmpCnt].type = EV_KEY;
            input.event[tmpCnt].code = BTN_TOUCH;
            input.event[tmpCnt].value = 0;
            tmpCnt++;
            input.event[tmpCnt].type = EV_KEY;
            input.event[tmpCnt].code = BTN_TOOL_FINGER;
            input.event[tmpCnt].value = 0;
            tmpCnt++;
        }
    } else {
        is = true;
    }
    input.event[tmpCnt].type = EV_SYN;
    input.event[tmpCnt].code = SYN_REPORT;
    input.event[tmpCnt].value = 0;
    tmpCnt++;

    if (nowfd > 0) {
        if (is && isFirstDown) {
            isFirstDown = false;
            write(nowfd, &input, sizeof(struct input_event) * (tmpCnt + 2));
        } else {
            write(nowfd, input.event, sizeof(struct input_event) * tmpCnt);
        }
    }
}

static void *TypeA(void *arg) {
    targ tmp = *(targ *) arg;
    int i = tmp.fdNum;
    float maxRawX = (tmp.maxRawX > 0.0f) ? tmp.maxRawX : 1080.0f;
    float maxRawY = (tmp.maxRawY > 0.0f) ? tmp.maxRawY : 2400.0f;
    int latest = 0;
    input_event inputEvent[64]{0};

    while (Touch_initialized) {
        if (origfd[i] < 0) break;
        auto readSize = (int32_t) read(origfd[i], inputEvent, sizeof(inputEvent));
        if (!Touch_initialized || origfd[i] < 0) break;
        if (readSize <= 0 || (readSize % sizeof(input_event)) != 0) {
            continue;
        }
        size_t count = size_t(readSize) / sizeof(input_event);
        for (size_t j = 0; j < count; j++) {
            input_event &ie = inputEvent[j];
            if (ie.type == EV_KEY) {
                if (ie.code == BTN_TOUCH) {
                    Finger[i][latest].isDown = (ie.value != 0);
                    if (ie.value != 0) {
                        Finger[i][latest].id = (i * 2 + 1) * maxF + latest;
                        Finger[i][latest].freshDown = true;
                    } else {
                        if (Finger[i][latest].swallow) {
                            ImGuiIO &io = ImGui::GetIO();
                            io.MouseDown[0] = false;
                        }
                        Finger[i][latest].swallow = false;
                        Finger[i][latest].freshDown = false;
                    }
                    continue;
                }
            }
            if (ie.type == EV_ABS) {
                if (ie.code == ABS_MT_SLOT) {
                    if (ie.value >= 0 && ie.value < maxF) {
                        latest = ie.value;
                    }
                    continue;
                }
                if (ie.code == ABS_MT_TRACKING_ID) {
                    if (ie.value == -1) {
                        if (Finger[i][latest].swallow) {
                            ImGuiIO &io = ImGui::GetIO();
                            io.MouseDown[0] = false;
                        }
                        Finger[i][latest].isDown = false;
                        Finger[i][latest].isTmpDown = false;
                        Finger[i][latest].swallow = false;
                        Finger[i][latest].freshDown = false;
                    } else {
                        Finger[i][latest].id = (i * 2 + 1) * maxF + latest;
                        Finger[i][latest].isDown = true;
                        Finger[i][latest].isTmpDown = true;
                        Finger[i][latest].freshDown = true;
                    }
                    continue;
                }
                if (ie.code == ABS_MT_POSITION_X || ie.code == ABS_X) {
                    Finger[i][latest].id = (i * 2 + 1) * maxF + latest;
                    Finger[i][latest].x = ie.value;
                    Finger[i][latest].isTmpDown = true;
                    continue;
                }
                if (ie.code == ABS_MT_POSITION_Y || ie.code == ABS_Y) {
                    Finger[i][latest].id = (i * 2 + 1) * maxF + latest;
                    Finger[i][latest].y = ie.value;
                    Finger[i][latest].isTmpDown = true;
                    continue;
                }
            }
            if (ie.code == SYN_REPORT) {
                if (Finger[i][latest].isDown) {
                    // Continuous pure normalized mathematical mapping [0.0 - 1.0]
                    float norm_u = std::clamp((float)Finger[i][latest].x / maxRawX, 0.0f, 1.0f);
                    float norm_v = std::clamp((float)Finger[i][latest].y / maxRawY, 0.0f, 1.0f);

                    float dispW = (screenWidth > 0.0f) ? screenWidth : 2400.0f;
                    float dispH = (screenHeight > 0.0f) ? screenHeight : 1080.0f;

                    // Ensure dispW is long edge and dispH is short edge for landscape
                    float longEdge  = std::max(dispW, dispH);
                    float shortEdge = std::min(dispW, dispH);

                    float x = 0.0f, y = 0.0f;
                    switch (orientation) {
                        case 1: // Landscape (standard mobile gaming orientation)
                            x = norm_v * longEdge;
                            y = (1.0f - norm_u) * shortEdge;
                            break;
                        case 2: // Reverse Portrait (180 deg)
                            x = (1.0f - norm_u) * shortEdge;
                            y = (1.0f - norm_v) * longEdge;
                            break;
                        case 3: // Reverse Landscape (270 deg)
                            x = (1.0f - norm_v) * longEdge;
                            y = norm_u * shortEdge;
                            break;
                        case 0: // Portrait
                        default:
                            x = norm_u * shortEdge;
                            y = norm_v * longEdge;
                            break;
                    }

                    x = std::clamp(x, 0.0f, (orientation == 1 || orientation == 3) ? longEdge : shortEdge);
                    y = std::clamp(y, 0.0f, (orientation == 1 || orientation == 3) ? shortEdge : longEdge);

                    // UI barrier: The swallow decision is taken on the first
                    // SYN_REPORT of the gesture only, then latched until the
                    // finger lifts. Deciding every frame meant a game drag that
                    // crossed a UI rect got hijacked mid-swipe; never clearing
                    // isDown here keeps io.MousePos live for the whole drag.
                    if (Finger[i][latest].freshDown) {
                        Finger[i][latest].freshDown = false;
                        Finger[i][latest].swallow = Touch_IsPointInUi(x, y);
                    }

                    // Only inject mouse input into ImGui if the touch belongs to the overlay UI
                    if (Finger[i][latest].swallow) {
                        ImGuiIO &io = ImGui::GetIO();
                        io.MousePos = ImVec2(x, y);
                        io.MouseDown[0] = true;
                    }
                } else {
                    if (Finger[i][latest].swallow) {
                        ImGuiIO &io = ImGui::GetIO();
                        io.MouseDown[0] = false;
                    }
                    Finger[i][latest].swallow = false;
                    Finger[i][latest].freshDown = false;
                }

                if (Finger[i][latest].isTmpDown) {
                    if (!Touch_readOnly && nowfd > 0) {
                        Upload();
                    }
                } else if (!Finger[i][latest].isDown) {
                    if (!Touch_readOnly && nowfd > 0) {
                        Upload();
                    }
                }
                continue;
            }
        }
    }
    return nullptr;
}

bool Touch_Init(int w, int h, uint32_t orientation_, bool readOnly) {
    char temp[128];
    DIR *dir = opendir("/dev/input/");
    if (!dir) return false;
    dirent *ptr = NULL;
    int eventCount = 0;
    while ((ptr = readdir(dir)) != NULL) {
        if (strstr(ptr->d_name, "event"))
            eventCount++;
    }
    closedir(dir);

    struct input_absinfo absX[maxE], absY[maxE];
    int fd, i;
    float maxArea = 0.0f;
    fdNum = 0;

    for (i = 0; i <= eventCount; i++) {
        sprintf(temp, "/dev/input/event%d", i);
        fd = open(temp, O_RDWR);
        if (fd < 0) {
            continue;
        }

        float devMaxX = 0.0f, devMaxY = 0.0f;
        if (checkDeviceIsTouch(fd, devMaxX, devMaxY)) {
            int tmp1 = ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &absX[fdNum]);
            int tmp2 = ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &absY[fdNum]);
            if (tmp1 == 0 && tmp2 == 0) {
                origfd[fdNum] = fd;
                if (!readOnly) {
                    ioctl(fd, EVIOCGRAB, GRAB);
                }

                // Keep the digitizer range of the primary screen: the device
                // with the largest resolution area. Normalising raw coordinates
                // against the wrong device's range skews every tap.
                float area = devMaxX * devMaxY;
                if (area > maxArea) {
                    maxArea = area;
                    s_digitizerMaxX = devMaxX;
                    s_digitizerMaxY = devMaxY;
                }

                fdNum++;
                if (fdNum >= maxE)
                    break;
            } else {
                close(fd);
            }
        } else {
            close(fd);
        }
    }

    if (fdNum == 0) {
        return false;
    }

    if (!readOnly) {
        struct uinput_user_dev ui_dev;
        nowfd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        if (nowfd <= 0) {
            // Graceful fallback: ungrab touch devices and run in passive mode if uinput is restricted
            readOnly = true;
            for (int k = 0; k < fdNum; k++) {
                ioctl(origfd[k], EVIOCGRAB, UNGRAB);
            }
        } else {
            int string_len = rand() % 10 + 5;
            char string[string_len];
            memset(&ui_dev, 0, sizeof(ui_dev));

            genRandomString(string, string_len);
            strncpy(ui_dev.name, string, UINPUT_MAX_NAME_SIZE);

            ui_dev.id.bustype = 0;
            ui_dev.id.vendor = rand() % 10 + 5;
            ui_dev.id.product = rand() % 10 + 5;
            ui_dev.id.version = rand() % 10 + 5;

            ioctl(nowfd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

            ioctl(nowfd, UI_SET_EVBIT, EV_ABS);
            ioctl(nowfd, UI_SET_ABSBIT, ABS_X);
            ioctl(nowfd, UI_SET_ABSBIT, ABS_Y);
            ioctl(nowfd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
            ioctl(nowfd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
            ioctl(nowfd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
            ioctl(nowfd, UI_SET_EVBIT, EV_SYN);
            ioctl(nowfd, UI_SET_EVBIT, EV_KEY);
            ioctl(nowfd, UI_SET_KEYBIT, BTN_TOOL_FINGER);
            ioctl(nowfd, UI_SET_KEYBIT, BTN_TOUCH);

            genRandomString(string, string_len);
            ioctl(nowfd, UI_SET_PHYS, string);

            ui_dev.absmin[ABS_MT_POSITION_X] = 0;
            ui_dev.absmax[ABS_MT_POSITION_X] = (int)s_digitizerMaxX;
            ui_dev.absmin[ABS_MT_POSITION_Y] = 0;
            ui_dev.absmax[ABS_MT_POSITION_Y] = (int)s_digitizerMaxY;
            ui_dev.absmin[ABS_X] = 0;
            ui_dev.absmax[ABS_X] = (int)s_digitizerMaxX;
            ui_dev.absmin[ABS_Y] = 0;
            ui_dev.absmax[ABS_Y] = (int)s_digitizerMaxY;
            ui_dev.absmin[ABS_MT_TRACKING_ID] = 0;
            ui_dev.absmax[ABS_MT_TRACKING_ID] = 65535;
            write(nowfd, &ui_dev, sizeof(ui_dev));

            if (ioctl(nowfd, UI_DEV_CREATE) != 0) {
                close(nowfd);
                nowfd = 0;
                readOnly = true;
                for (int k = 0; k < fdNum; k++) {
                    ioctl(origfd[k], EVIOCGRAB, UNGRAB);
                }
            }
        }
    }

    Touch_initialized = true;
    Touch_readOnly = readOnly;

    for (i = 0; i < fdNum; i++) {
        targF[i].fdNum = i;
        targF[i].maxRawX = (float)absX[i].maximum;
        targF[i].maxRawY = (float)absY[i].maximum;
        pthread_create(&s_touchThreads[i], NULL, TypeA, &targF[i]);
    }

    ::screenWidth = (float)w;
    ::screenHeight = (float)h; 
    ::orientation = orientation_;

    system("chmod 000 -R /proc/bus/input/* 2>/dev/null");
    return true;
}

void UpdateScreenData(int w, int h, uint32_t orientation_) {
    ::screenWidth = (float)w;
    ::screenHeight = (float)h; 
    ::orientation = orientation_;
}

static bool checkDeviceIsTouch(int fd, float &outMaxX, float &outMaxY) {
    uint8_t propBits[(INPUT_PROP_MAX + 7) / 8]{0};
    bool hasDirect = false;
    if (ioctl(fd, EVIOCGPROP(sizeof(propBits)), propBits) >= 0) {
        if (propBits[INPUT_PROP_DIRECT / 8] & (1 << (INPUT_PROP_DIRECT % 8))) {
            hasDirect = true;
        }
    }

    struct input_absinfo absX{}, absY{};
    bool hasX = (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &absX) == 0 && absX.maximum > 400);
    bool hasY = (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &absY) == 0 && absY.maximum > 400);

    if (hasX && hasY) {
        outMaxX = (float)absX.maximum;
        outMaxY = (float)absY.maximum;
        return true;
    }
    return false;
}

void Touch_Close() {
    if (Touch_initialized) {
        Touch_initialized = false;
        for (int i = 0; i < maxE; ++i) {
            if (origfd[i] >= 0) {
                if (!Touch_readOnly)
                    ioctl(origfd[i], EVIOCGRAB, UNGRAB);
                close(origfd[i]);
                origfd[i] = -1;
            }
        }
        for (int i = 0; i < fdNum; ++i) {
            if (s_touchThreads[i] != 0) {
                pthread_join(s_touchThreads[i], NULL);
                s_touchThreads[i] = 0;
            }
        }
        if (nowfd >= 0) {
            ioctl(nowfd, UI_DEV_DESTROY);
            close(nowfd);
            nowfd = -1;
        }
        fdNum = 0;
        memset(Finger, 0, sizeof(Finger));
        memset(input.event, 0, sizeof(input.event));
    }
}

void Touch_Down(float xt, float yt) {
    static int x, y;
    x = 0, y = 0;
    switch (orientation) {
        case 1: {
            x = screenHeight - yt;
            y = xt;
            break;
        }                          
        case 2: {
            x = screenWidth - xt;
            y = screenHeight - yt;
            break;
        }
        case 3: {
            y = screenWidth - xt;
            x = yt;
            break;
        }
        default: {
            x = xt;
            y = yt;
            break;
        }
    }
    touchObj &touch = Finger[0][9];
    touch.id = 19;
    touch.x = (int)(x * (s_digitizerMaxX / (screenWidth > 0.0f ? screenWidth : 1.0f)));
    touch.y = (int)(y * (s_digitizerMaxY / (screenHeight > 0.0f ? screenHeight : 1.0f)));        
    touch.isDown = true;
    Upload();
}

void Touch_Move(float x, float y) {
    Touch_Down(x, y);
}

void Touch_Up() {
    touchObj &touch = Finger[0][9];
    touch.isDown = false;
    Upload();
}

bool IsFingerDown(int slot) {
    if (slot < 0 || slot >= maxF) return false;
    return Finger[0][slot].isDown;
}

bool GetFingerScreenPos(int slot, float& outX, float& outY) {
    if (slot < 0 || slot >= maxF || !Finger[0][slot].isDown) return false;
    float x = (float)Finger[0][slot].x * (screenWidth / (s_digitizerMaxX > 0.0f ? s_digitizerMaxX : 1.0f));
    float y = (float)Finger[0][slot].y * (screenHeight / (s_digitizerMaxY > 0.0f ? s_digitizerMaxY : 1.0f));
    switch (orientation) {
        case 1: outX = y; outY = screenHeight - x; break;
        case 2: outX = screenWidth - x; outY = screenHeight - y; break;
        case 3: outX = screenWidth - y; outY = x; break;
        default: outX = x; outY = y; break;
    }
    return true;
}

void Touch_Down_Slot(int slot, float xt, float yt) {
    if (slot < 0 || slot >= maxF) slot = 6;
    if (fdNum <= 0) return;
    float x = 0, y = 0;
    switch (orientation) {
        case 1: x = screenHeight - yt; y = xt; break;
        case 2: x = screenWidth - xt; y = screenHeight - yt; break;
        case 3: y = screenWidth - xt; x = yt; break;
        default: x = xt; y = yt; break;
    }
    touchObj &touch = Finger[0][slot];
    touch.id = 10 + slot;
    touch.x = (int)(x * (s_digitizerMaxX / (screenWidth > 0.0f ? screenWidth : 1.0f)));
    touch.y = (int)(y * (s_digitizerMaxY / (screenHeight > 0.0f ? screenHeight : 1.0f)));
    touch.isDown = true;
    Upload();
}

void Touch_Move_Slot(int slot, float x, float y) {
    Touch_Down_Slot(slot, x, y);
}

void Touch_Up_Slot(int slot) {
    if (slot < 0 || slot >= maxF) slot = 6;
    if (fdNum <= 0) return;
    touchObj &touch = Finger[0][slot];
    touch.isDown = false;
    Upload();
}
