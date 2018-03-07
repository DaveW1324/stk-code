// Copyright (C) 2002-2007 Nikolaus Gebhardt
// Copyright (C) 2007-2011 Christian Stehno
// Copyright (C) 2016-2017 Dawid Gan
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#include "CIrrDeviceAndroid.h"

#ifdef _IRR_COMPILE_WITH_ANDROID_DEVICE_

#include <assert.h>
#include "os.h"
#include "CContextEGL.h"
#include "CFileSystem.h"
#include "COGLES2Driver.h"

namespace irr
{
    namespace video
    {
        IVideoDriver* createOGLES2Driver(const SIrrlichtCreationParameters& params,
            video::SExposedVideoData& data, io::IFileSystem* io);
    }
}

namespace irr
{

// These variables must be global. Otherwise initialization will reach infinite
// loop after creating the device second time (i.e. the NULL driver and then
// GLES2 driver). We get initialization events from Android only once.
bool CIrrDeviceAndroid::IsPaused = true;
bool CIrrDeviceAndroid::IsFocused = false;
bool CIrrDeviceAndroid::IsStarted = false;

// Execution of android_main() function is a kind of "onCreate" event, so this
// function should be used there to make sure that global window state variables
// have their default values on startup.
void CIrrDeviceAndroid::onCreate()
{
    IsPaused = true;
    IsFocused = false;
    IsStarted = false;
}

//! constructor
CIrrDeviceAndroid::CIrrDeviceAndroid(const SIrrlichtCreationParameters& param)
    : CIrrDeviceStub(param),
    Accelerometer(0),
    Gyroscope(0),
    IsMousePressed(false),
    GamepadAxisX(0),
    GamepadAxisY(0),
    DefaultOrientation(ORIENTATION_UNKNOWN)
{
    #ifdef _DEBUG
    setDebugName("CIrrDeviceAndroid");
    #endif

    Android = (android_app *)(param.PrivateData);
    assert(Android != NULL);

    Android->userData = this;
    Android->onAppCmd = handleAndroidCommand;
    Android->onInputEvent = handleInput;
    
    printConfig();
    createKeyMap();

    CursorControl = new CCursorControl();
    
    Close = Android->destroyRequested;

    // It typically shouldn't happen, but just in case...
    if (Close)
        return;

    SensorManager = ASensorManager_getInstance();
    SensorEventQueue = ASensorManager_createEventQueue(SensorManager,
                                Android->looper, LOOPER_ID_USER, NULL, NULL);

    ANativeActivity_setWindowFlags(Android->activity,
                                   AWINDOW_FLAG_KEEP_SCREEN_ON |
                                   AWINDOW_FLAG_FULLSCREEN, 0);

    os::Printer::log("Waiting for Android activity window to be created.", ELL_DEBUG);

    while (!IsStarted || !IsFocused || IsPaused)
    {
        s32 events = 0;
        android_poll_source* source = 0;

        s32 id = ALooper_pollAll(-1, NULL, &events, (void**)&source);

        if (id >=0 && source != NULL)
        {
            source->process(Android, source);
        }
    }
    
    assert(Android->window);
    os::Printer::log("Done", ELL_DEBUG);
    
    ExposedVideoData.OGLESAndroid.Window = Android->window;

    createVideoModeList();

    createDriver();

    if (VideoDriver)
        createGUIAndScene();
}


CIrrDeviceAndroid::~CIrrDeviceAndroid()
{
    Android->userData = NULL;
    Android->onAppCmd = NULL;
    Android->onInputEvent = NULL;
}

void CIrrDeviceAndroid::printConfig() 
{
    char language[3] = {};
    char country[3] = {};
    AConfiguration_getLanguage(Android->config, language);
    AConfiguration_getCountry(Android->config, country);
    int32_t mcc = AConfiguration_getMcc(Android->config);
    int32_t mnc = AConfiguration_getMnc(Android->config);
    int32_t orientation = AConfiguration_getOrientation(Android->config);
    int32_t touch = AConfiguration_getTouchscreen(Android->config);
    int32_t density = AConfiguration_getDensity(Android->config);
    int32_t keyboard = AConfiguration_getKeyboard(Android->config);
    int32_t navigation = AConfiguration_getNavigation(Android->config);
    int32_t keys_hidden = AConfiguration_getKeysHidden(Android->config);
    int32_t nav_hidden = AConfiguration_getNavHidden(Android->config);
    int32_t sdk_version = AConfiguration_getSdkVersion(Android->config);
    int32_t screen_size = AConfiguration_getScreenSize(Android->config);
    int32_t screen_long = AConfiguration_getScreenLong(Android->config);
    int32_t ui_mode_type = AConfiguration_getUiModeType(Android->config);
    int32_t ui_mode_night = AConfiguration_getUiModeNight(Android->config);

    os::Printer::log("Android configuration: ", ELL_DEBUG);
    os::Printer::log("   country:", country != NULL ? country : "unknown", ELL_DEBUG);
    os::Printer::log("   density:", core::stringc(density).c_str(), ELL_DEBUG);
    os::Printer::log("   keyboard:", core::stringc(keyboard).c_str(), ELL_DEBUG);
    os::Printer::log("   keys_hidden:", core::stringc(keys_hidden).c_str(), ELL_DEBUG);
    os::Printer::log("   language:", language != NULL ? language : "unknown", ELL_DEBUG);
    os::Printer::log("   mcc:", core::stringc(mcc).c_str(), ELL_DEBUG);
    os::Printer::log("   mnc:", core::stringc(mnc).c_str(), ELL_DEBUG);
    os::Printer::log("   nav_hidden:", core::stringc(nav_hidden).c_str(), ELL_DEBUG);
    os::Printer::log("   navigation:", core::stringc(navigation).c_str(), ELL_DEBUG);
    os::Printer::log("   orientation:", core::stringc(orientation).c_str(), ELL_DEBUG);
    os::Printer::log("   screen_long:", core::stringc(screen_long).c_str(), ELL_DEBUG);
    os::Printer::log("   screen_size:", core::stringc(screen_size).c_str(), ELL_DEBUG);
    os::Printer::log("   sdk_version:", core::stringc(sdk_version).c_str(), ELL_DEBUG);
    os::Printer::log("   touch:", core::stringc(touch).c_str(), ELL_DEBUG);
    os::Printer::log("   ui_mode_type:", core::stringc(ui_mode_type).c_str(), ELL_DEBUG);
    os::Printer::log("   ui_mode_night:", core::stringc(ui_mode_night).c_str(), ELL_DEBUG);
}

void CIrrDeviceAndroid::createVideoModeList()
{
    if (VideoModeList.getVideoModeCount() > 0)
        return;
        
    int width = ANativeWindow_getWidth(Android->window);
    int height = ANativeWindow_getHeight(Android->window);
    
    if (width > 0 && height > 0)
    {
        CreationParams.WindowSize.Width = width;
        CreationParams.WindowSize.Height = height;
    }

    core::dimension2d<u32> size = core::dimension2d<u32>(
                                    CreationParams.WindowSize.Width,
                                    CreationParams.WindowSize.Height);

    VideoModeList.addMode(size, 32);
    VideoModeList.setDesktop(32, size);
}

void CIrrDeviceAndroid::createDriver()
{
    // Create the driver.
    switch(CreationParams.DriverType)
    {
    case video::EDT_OGLES2:
        #ifdef _IRR_COMPILE_WITH_OGLES2_
        VideoDriver = video::createOGLES2Driver(CreationParams, ExposedVideoData, FileSystem);
        #else
        os::Printer::log("No OpenGL ES 2.0 support compiled in.", ELL_ERROR);
        #endif
        break;

    case video::EDT_NULL:
        VideoDriver = video::createNullDriver(FileSystem, CreationParams.WindowSize);
        break;

    default:
        os::Printer::log("Unable to create video driver of unknown type.", ELL_ERROR);
        break;
    }
}

bool CIrrDeviceAndroid::run()
{
    os::Timer::tick();
    
    while (!Close)
    {
        s32 Events = 0;
        android_poll_source* Source = 0;
        bool should_run = (IsStarted && IsFocused && !IsPaused);
        s32 id = ALooper_pollAll(should_run ? 0 : -1, NULL, &Events,
                                 (void**)&Source);

        if (id < 0)
            break;

        if (Source)
        {
            Source->process(Android, Source);
        }

        // if a sensor has data, we'll process it now.
        if (id == LOOPER_ID_USER)
        {
            ASensorEvent event;
            while (ASensorEventQueue_getEvents(SensorEventQueue, &event, 1) > 0)
            {
                switch (event.type)
                {
                case ASENSOR_TYPE_ACCELEROMETER:
                    SEvent accEvent;
                    accEvent.EventType = EET_ACCELEROMETER_EVENT;
                    
                    if (DefaultOrientation == ORIENTATION_LANDSCAPE)
                    {
                        accEvent.AccelerometerEvent.X = event.acceleration.y;
                        accEvent.AccelerometerEvent.Y = -event.acceleration.x;
                    }
                    else
                    {
                        accEvent.AccelerometerEvent.X = event.acceleration.x;
                        accEvent.AccelerometerEvent.Y = event.acceleration.y;
                    }
                    accEvent.AccelerometerEvent.Z = event.acceleration.z;
                    
                    if (accEvent.AccelerometerEvent.X < 0)
                    {
                        accEvent.AccelerometerEvent.X *= -1;
                        accEvent.AccelerometerEvent.Y *= -1;
                        accEvent.AccelerometerEvent.Z *= -1;
                    }

                    postEventFromUser(accEvent);
                    break;

                case ASENSOR_TYPE_GYROSCOPE:
                    SEvent gyroEvent;
                    gyroEvent.EventType = EET_GYROSCOPE_EVENT;
                    gyroEvent.GyroscopeEvent.X = event.vector.x;
                    gyroEvent.GyroscopeEvent.Y = event.vector.y;
                    gyroEvent.GyroscopeEvent.Z = event.vector.z;

                    postEventFromUser(gyroEvent);
                    break;
                default:
                    break;
                }
            }
        }
    }

    return !Close;
}

void CIrrDeviceAndroid::yield()
{
    struct timespec ts = {0,1};
    nanosleep(&ts, NULL);
}

void CIrrDeviceAndroid::sleep(u32 timeMs, bool pauseTimer)
{
    const bool wasStopped = Timer ? Timer->isStopped() : true;

    struct timespec ts;
    ts.tv_sec = (time_t) (timeMs / 1000);
    ts.tv_nsec = (long) (timeMs % 1000) * 1000000;

    if (pauseTimer && !wasStopped)
        Timer->stop();

    nanosleep(&ts, NULL);

    if (pauseTimer && !wasStopped)
        Timer->start();
}

void CIrrDeviceAndroid::setWindowCaption(const wchar_t* text)
{
}

bool CIrrDeviceAndroid::present(video::IImage* surface, void* windowId,
                                core::rect<s32>* srcClip)
{
    return true;
}

bool CIrrDeviceAndroid::isWindowActive() const
{
    return (IsFocused && !IsPaused);
}

bool CIrrDeviceAndroid::isWindowFocused() const
{
    return IsFocused;
}

bool CIrrDeviceAndroid::isWindowMinimized() const
{
    return IsPaused;
}

void CIrrDeviceAndroid::closeDevice()
{
    Close = true;
}

void CIrrDeviceAndroid::setResizable(bool resize)
{
}

void CIrrDeviceAndroid::minimizeWindow()
{
}

void CIrrDeviceAndroid::maximizeWindow()
{
}

void CIrrDeviceAndroid::restoreWindow()
{
}

bool CIrrDeviceAndroid::moveWindow(int x, int y)
{
    return true;
}

bool CIrrDeviceAndroid::getWindowPosition(int* x, int* y)
{
    *x = 0;
    *y = 0;
    return true;
}

E_DEVICE_TYPE CIrrDeviceAndroid::getType() const
{
    return EIDT_ANDROID;
}

void CIrrDeviceAndroid::handleAndroidCommand(android_app* app, int32_t cmd)
{
    CIrrDeviceAndroid* device = (CIrrDeviceAndroid *)app->userData;
    assert(device != NULL);
    
    switch (cmd)
    {
    case APP_CMD_SAVE_STATE:
        os::Printer::log("Android command APP_CMD_SAVE_STATE", ELL_DEBUG);
        break;
    case APP_CMD_INIT_WINDOW:
        os::Printer::log("Android command APP_CMD_INIT_WINDOW", ELL_DEBUG);
        
        device->getExposedVideoData().OGLESAndroid.Window = app->window;
        
        // If the Android app is resumed, we need to re-create EGL surface
        // to allow to draw something on it again.
        if (device->VideoDriver != NULL && 
            device->CreationParams.DriverType == video::EDT_OGLES2)
        {
            video::COGLES2Driver* driver = (video::COGLES2Driver*)(device->VideoDriver);
            driver->getEGLContext()->reloadEGLSurface(app->window);
        }
        
        IsStarted = true;
        break;
    case APP_CMD_TERM_WINDOW:
        os::Printer::log("Android command APP_CMD_TERM_WINDOW", ELL_DEBUG);
        IsStarted = false;
        break;
    case APP_CMD_GAINED_FOCUS:
        os::Printer::log("Android command APP_CMD_GAINED_FOCUS", ELL_DEBUG);
        IsFocused = true;
        break;
    case APP_CMD_LOST_FOCUS:
        os::Printer::log("Android command APP_CMD_LOST_FOCUS", ELL_DEBUG);
        IsFocused = false;
        break;
    case APP_CMD_DESTROY:
        os::Printer::log("Android command APP_CMD_DESTROY", ELL_DEBUG);
        device->Close = true;
        // Make sure that state variables are set to the default state
        // when the app is destroyed
        IsPaused = true;
        IsFocused = false;
        IsStarted = false;
        break;
    case APP_CMD_PAUSE:
        os::Printer::log("Android command APP_CMD_PAUSE", ELL_DEBUG);
        IsPaused = true;
        break;
    case APP_CMD_RESUME:
        os::Printer::log("Android command APP_CMD_RESUME", ELL_DEBUG);
        IsPaused = false;
        break;
    case APP_CMD_START:
        os::Printer::log("Android command APP_CMD_START", ELL_DEBUG);
        break;
    case APP_CMD_STOP:
        os::Printer::log("Android command APP_CMD_STOP", ELL_DEBUG);
        break;
    case APP_CMD_WINDOW_RESIZED:
        os::Printer::log("Android command APP_CMD_WINDOW_RESIZED", ELL_DEBUG);
        break;
    case APP_CMD_CONFIG_CHANGED:
        os::Printer::log("Android command APP_CMD_CONFIG_CHANGED", ELL_DEBUG);
        break;
    case APP_CMD_LOW_MEMORY:
        os::Printer::log("Android command APP_CMD_LOW_MEMORY", ELL_DEBUG);
        break;
    default:
        os::Printer::log("Android command: ", core::stringc(cmd).c_str(), ELL_DEBUG);
        break;
    }
    
    SEvent event;
    event.EventType = EET_SYSTEM_EVENT;
    event.SystemEvent.EventType = ESET_ANDROID_CMD;
    event.SystemEvent.AndroidCmd.Cmd = cmd;
    device->postEventFromUser(event);
}

s32 CIrrDeviceAndroid::handleInput(android_app* app, AInputEvent* androidEvent)
{
    CIrrDeviceAndroid* device = (CIrrDeviceAndroid*)app->userData;
    assert(device != NULL);
    
    s32 status = 0;
    
    int32_t source = AInputEvent_getSource(androidEvent);
    int32_t type = AInputEvent_getType(androidEvent);
    
    if (source == AINPUT_SOURCE_GAMEPAD ||
        source == AINPUT_SOURCE_JOYSTICK ||
        source == AINPUT_SOURCE_DPAD)
    {
        status = device->handleGamepad(androidEvent);
    }
    else
    {
        switch (type)
        {
        case AINPUT_EVENT_TYPE_MOTION:
        {
            status = device->handleTouch(androidEvent);
            break;
        }
        case AINPUT_EVENT_TYPE_KEY:
        {
            status = device->handleKeyboard(androidEvent);
            break;
        }
        default:
            break;
        }
    }

    return status;
}

s32 CIrrDeviceAndroid::handleTouch(AInputEvent* androidEvent)
{
    s32 status = 0;
    
    SEvent event;
    event.EventType = EET_TOUCH_INPUT_EVENT;

    s32 eventAction = AMotionEvent_getAction(androidEvent);

#if 0
    // Useful for debugging. We might have to pass some of those infos on at some point.
    // but preferably device independent (so iphone can use same irrlicht flags).
    int32_t flags = AMotionEvent_getFlags(androidEvent);
    os::Printer::log("flags: ", core::stringc(flags).c_str(), ELL_DEBUG);
    int32_t metaState = AMotionEvent_getMetaState(androidEvent);
    os::Printer::log("metaState: ", core::stringc(metaState).c_str(), ELL_DEBUG);
    int32_t edgeFlags = AMotionEvent_getEdgeFlags(androidEvent);
    os::Printer::log("edgeFlags: ", core::stringc(flags).c_str(), ELL_DEBUG);
#endif

    bool touchReceived = true;
    bool simulate_mouse = false;
    core::position2d<s32> mouse_pos = core::position2d<s32>(0, 0);

    switch (eventAction & AMOTION_EVENT_ACTION_MASK)
    {
    case AMOTION_EVENT_ACTION_DOWN:
    case AMOTION_EVENT_ACTION_POINTER_DOWN:
        event.TouchInput.Event = ETIE_PRESSED_DOWN;
        break;
    case AMOTION_EVENT_ACTION_MOVE:
        event.TouchInput.Event = ETIE_MOVED;
        break;
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_POINTER_UP:
    case AMOTION_EVENT_ACTION_CANCEL:
        event.TouchInput.Event = ETIE_LEFT_UP;
        break;
    default:
        touchReceived = false;
        break;
    }
    
    if (touchReceived)
    {
        s32 count = 1;
        s32 idx = (eventAction & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> 
                            AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
        
        // Process all touches for move action.
        if (event.TouchInput.Event == ETIE_MOVED)
        {
            count = AMotionEvent_getPointerCount(androidEvent);
            idx = 0;
        }

        for (s32 i = 0; i < count; ++i)
        {
            event.TouchInput.ID = AMotionEvent_getPointerId(androidEvent, i + idx);
            event.TouchInput.X = AMotionEvent_getX(androidEvent, i + idx);
            event.TouchInput.Y = AMotionEvent_getY(androidEvent, i + idx);
            
            if (event.TouchInput.ID >= 32)
                continue;
            
            TouchEventData& event_data = TouchEventsData[event.TouchInput.ID];
            
            // Don't send move event when nothing changed
            if (event_data.event == event.TouchInput.Event &&
                event_data.x == event.TouchInput.X &&
                event_data.y == event.TouchInput.Y)
                continue;
                
            event_data.event = event.TouchInput.Event;
            event_data.x = event.TouchInput.X;
            event_data.y = event.TouchInput.Y;
            
            postEventFromUser(event);
            
            if (event.TouchInput.ID == 0)
            {
                simulate_mouse = true;
                mouse_pos.X = event.TouchInput.X;
                mouse_pos.Y = event.TouchInput.Y;
            }
        }

        status = 1;
    }
    
    // Simulate mouse event for first finger on multitouch device.
    // This allows to click on GUI elements.        
    if (simulate_mouse)
    {
        CursorControl->setPosition(mouse_pos);

        SEvent irrevent;
        bool send_event = true;
        
        switch (event.TouchInput.Event)
        {
        case ETIE_PRESSED_DOWN:
            irrevent.MouseInput.Event = EMIE_LMOUSE_PRESSED_DOWN;
            IsMousePressed = true;
            break;
        case ETIE_LEFT_UP:
            irrevent.MouseInput.Event = EMIE_LMOUSE_LEFT_UP;
            IsMousePressed = false;
            break;
        case ETIE_MOVED:
            irrevent.MouseInput.Event = EMIE_MOUSE_MOVED;
            break;
        default:
            send_event = false;
            break;
        }
        
        if (send_event)
        {
            irrevent.MouseInput.Control = false;
            irrevent.MouseInput.Shift = false;
            irrevent.MouseInput.ButtonStates = IsMousePressed ? 
                                                            irr::EMBSM_LEFT : 0;
            irrevent.EventType = EET_MOUSE_INPUT_EVENT;
            irrevent.MouseInput.X = mouse_pos.X;
            irrevent.MouseInput.Y = mouse_pos.Y;

            postEventFromUser(irrevent);
        }
    }
    
    return status;
}


s32 CIrrDeviceAndroid::handleKeyboard(AInputEvent* androidEvent)
{
    s32 status = 0;

    bool ignore_event = false;

    SEvent event;
    event.EventType = EET_KEY_INPUT_EVENT;
    event.KeyInput.Char = 0;
    event.KeyInput.PressedDown = false;
    event.KeyInput.Key = IRR_KEY_UNKNOWN;

    int32_t keyCode = AKeyEvent_getKeyCode(androidEvent);
    int32_t keyAction = AKeyEvent_getAction(androidEvent);
    int32_t keyMetaState = AKeyEvent_getMetaState(androidEvent);
    int32_t keyRepeat = AKeyEvent_getRepeatCount(androidEvent);
    int32_t scanCode = AKeyEvent_getScanCode(androidEvent);
    
    if (keyAction == AKEY_EVENT_ACTION_DOWN)
    {
        event.KeyInput.PressedDown = true;
    }
    else if (keyAction == AKEY_EVENT_ACTION_UP)
    {
        event.KeyInput.PressedDown = false;
    }
    else if (keyAction == AKEY_EVENT_ACTION_MULTIPLE)
    {
        // TODO: Multiple duplicate key events have occurred in a row,
        // or a complex string is being delivered. The repeat_count
        // property of the key event contains the number of times the
        // given key code should be executed.
        // I guess this might necessary for more complicated i18n key input,
        // but don't see yet how to handle this correctly.
    }

    event.KeyInput.Shift = (keyMetaState & AMETA_SHIFT_ON ||
                            keyMetaState & AMETA_SHIFT_LEFT_ON ||
                            keyMetaState & AMETA_SHIFT_RIGHT_ON);

    event.KeyInput.Control = (keyMetaState & AMETA_CTRL_ON ||
                              keyMetaState & AMETA_CTRL_LEFT_ON ||
                              keyMetaState & AMETA_CTRL_RIGHT_ON);

    event.KeyInput.SystemKeyCode = (u32)keyCode;
    event.KeyInput.Key = KeyMap[keyCode];

    if (event.KeyInput.Key > 0)
    {
        getKeyChar(event);
    }
    
    // If button doesn't return key code, then at least use device-specific
    // scan code, because it's better than nothing
    if (event.KeyInput.Key == 0)
    {
        event.KeyInput.Key = (EKEY_CODE)scanCode;
    }

    // Handle an event when back button in pressed just like an escape key
    // and also avoid repeating the event to avoid some strange behaviour
    if (event.KeyInput.SystemKeyCode == AKEYCODE_BACK &&
        (event.KeyInput.PressedDown == false || keyRepeat > 0))
    {
        ignore_event = true;
    }

    // Mark escape key and gamepad buttons as handled by application to avoid 
    // receiving duplicated events
    if (event.KeyInput.SystemKeyCode == AKEYCODE_ESCAPE ||
        event.KeyInput.SystemKeyCode == AKEYCODE_BACK ||
        (event.KeyInput.SystemKeyCode >= AKEYCODE_BUTTON_A &&
        event.KeyInput.SystemKeyCode <= AKEYCODE_BUTTON_MODE))
    {
        status = 1;
    }

    if (!ignore_event)
    {
        postEventFromUser(event);
    }

    return status;
}

s32 CIrrDeviceAndroid::handleGamepad(AInputEvent* androidEvent)
{
    s32 status = 0;
    
    int32_t type = AInputEvent_getType(androidEvent);
    
    switch (type)
    {
    case AINPUT_EVENT_TYPE_MOTION:
    {
        float axis_x = AMotionEvent_getAxisValue(androidEvent, 
                                                 AMOTION_EVENT_AXIS_HAT_X, 0);

        if (axis_x == 0)
        {
            axis_x = AMotionEvent_getAxisValue(androidEvent, 
                                               AMOTION_EVENT_AXIS_X, 0);
        }
        
        if (axis_x == 0)
        {
            axis_x = AMotionEvent_getAxisValue(androidEvent, 
                                               AMOTION_EVENT_AXIS_Z, 0);
        }

        float axis_y = AMotionEvent_getAxisValue(androidEvent, 
                                                 AMOTION_EVENT_AXIS_HAT_Y, 0);
                                                 
        if (axis_y == 0)
        {
            axis_y = AMotionEvent_getAxisValue(androidEvent, 
                                               AMOTION_EVENT_AXIS_Y, 0);
        }
        
        if (axis_y == 0)
        {
            axis_y = AMotionEvent_getAxisValue(androidEvent, 
                                               AMOTION_EVENT_AXIS_RZ, 0);
        }
                                                 
        SEvent event;
        event.EventType = EET_KEY_INPUT_EVENT;
        event.KeyInput.Char = 0;
        event.KeyInput.Shift = false;
        event.KeyInput.Control = false;
        event.KeyInput.SystemKeyCode = 0;
        
        float deadzone = 0.3f;
        
        axis_x = axis_x > deadzone || axis_x < -deadzone ? axis_x : 0;
        axis_y = axis_y > deadzone || axis_y < -deadzone ? axis_y : 0;
        
        if (axis_x != GamepadAxisX)
        {
            if (GamepadAxisX != 0)
            {
                event.KeyInput.PressedDown = false;
                event.KeyInput.Key = GamepadAxisX < 0 ? IRR_KEY_BUTTON_LEFT
                                                      : IRR_KEY_BUTTON_RIGHT;
                postEventFromUser(event);
            }
            
            if (axis_x != 0)
            {
                event.KeyInput.PressedDown = true;
                event.KeyInput.Key = axis_x < 0 ? IRR_KEY_BUTTON_LEFT 
								                : IRR_KEY_BUTTON_RIGHT;
                postEventFromUser(event);
            }
            
            GamepadAxisX = axis_x;
        }
        
        if (axis_y != GamepadAxisY)
        {
            if (GamepadAxisY != 0)
            {
                event.KeyInput.PressedDown = false;
                event.KeyInput.Key = GamepadAxisY < 0 ? IRR_KEY_BUTTON_UP
                                                      : IRR_KEY_BUTTON_DOWN;
                postEventFromUser(event);
            }
            
            if (axis_y != 0)
            {
                event.KeyInput.PressedDown = true;
                event.KeyInput.Key = axis_y < 0 ? IRR_KEY_BUTTON_UP 
								                : IRR_KEY_BUTTON_DOWN;
                postEventFromUser(event);
            }
            
            GamepadAxisY = axis_y;
        }
        
        status = 1;

        break;
    }
    case AINPUT_EVENT_TYPE_KEY:
    {
        bool ignore = false;
        
        int32_t keyCode = AKeyEvent_getKeyCode(androidEvent);
        int32_t keyAction = AKeyEvent_getAction(androidEvent);
        int32_t keyRepeat = AKeyEvent_getRepeatCount(androidEvent);
        int32_t scanCode = AKeyEvent_getScanCode(androidEvent);
        
        if (keyRepeat == 0)
        {
            bool ignore_event = false;
            
            SEvent event;
            event.EventType = EET_KEY_INPUT_EVENT;
            event.KeyInput.Char = 0;
            event.KeyInput.PressedDown = (keyAction == AKEY_EVENT_ACTION_DOWN);
            event.KeyInput.Shift = false;
            event.KeyInput.Control = false;
            event.KeyInput.SystemKeyCode = (u32)keyCode;
            event.KeyInput.Key = KeyMap[keyCode];
            
            if (event.KeyInput.Key == 0)
            {
                event.KeyInput.Key = (EKEY_CODE)scanCode;
            }
            
            postEventFromUser(event);
        }
        
        status = 1;
        break;
    }
    default:
        break;
    }
    
    return status;
}

video::SExposedVideoData& CIrrDeviceAndroid::getExposedVideoData()
{
    return ExposedVideoData;
}

void CIrrDeviceAndroid::createKeyMap()
{
    KeyMap[AKEYCODE_UNKNOWN] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_SOFT_LEFT] = IRR_KEY_LBUTTON; 
    KeyMap[AKEYCODE_SOFT_RIGHT] = IRR_KEY_RBUTTON; 
    KeyMap[AKEYCODE_HOME] = IRR_KEY_HOME; 
    KeyMap[AKEYCODE_BACK] = IRR_KEY_ESCAPE; 
    KeyMap[AKEYCODE_CALL] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_ENDCALL] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_0] = IRR_KEY_0; 
    KeyMap[AKEYCODE_1] = IRR_KEY_1; 
    KeyMap[AKEYCODE_2] = IRR_KEY_2; 
    KeyMap[AKEYCODE_3] = IRR_KEY_3; 
    KeyMap[AKEYCODE_4] = IRR_KEY_4; 
    KeyMap[AKEYCODE_5] = IRR_KEY_5; 
    KeyMap[AKEYCODE_6] = IRR_KEY_6; 
    KeyMap[AKEYCODE_7] = IRR_KEY_7; 
    KeyMap[AKEYCODE_8] = IRR_KEY_8; 
    KeyMap[AKEYCODE_9] = IRR_KEY_9; 
    KeyMap[AKEYCODE_STAR] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_POUND] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_DPAD_UP] = IRR_KEY_UP; 
    KeyMap[AKEYCODE_DPAD_DOWN] = IRR_KEY_DOWN; 
    KeyMap[AKEYCODE_DPAD_LEFT] = IRR_KEY_LEFT; 
    KeyMap[AKEYCODE_DPAD_RIGHT] = IRR_KEY_RIGHT; 
    KeyMap[AKEYCODE_DPAD_CENTER] = IRR_KEY_RETURN; 
    KeyMap[AKEYCODE_VOLUME_UP] = IRR_KEY_VOLUME_DOWN; 
    KeyMap[AKEYCODE_VOLUME_DOWN] = IRR_KEY_VOLUME_UP; 
    KeyMap[AKEYCODE_POWER] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_CAMERA] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_CLEAR] = IRR_KEY_CLEAR; 
    KeyMap[AKEYCODE_A] = IRR_KEY_A; 
    KeyMap[AKEYCODE_B] = IRR_KEY_B; 
    KeyMap[AKEYCODE_C] = IRR_KEY_C; 
    KeyMap[AKEYCODE_D] = IRR_KEY_D; 
    KeyMap[AKEYCODE_E] = IRR_KEY_E; 
    KeyMap[AKEYCODE_F] = IRR_KEY_F; 
    KeyMap[AKEYCODE_G] = IRR_KEY_G; 
    KeyMap[AKEYCODE_H] = IRR_KEY_H; 
    KeyMap[AKEYCODE_I] = IRR_KEY_I; 
    KeyMap[AKEYCODE_J] = IRR_KEY_J; 
    KeyMap[AKEYCODE_K] = IRR_KEY_K; 
    KeyMap[AKEYCODE_L] = IRR_KEY_L; 
    KeyMap[AKEYCODE_M] = IRR_KEY_M; 
    KeyMap[AKEYCODE_N] = IRR_KEY_N; 
    KeyMap[AKEYCODE_O] = IRR_KEY_O; 
    KeyMap[AKEYCODE_P] = IRR_KEY_P; 
    KeyMap[AKEYCODE_Q] = IRR_KEY_Q; 
    KeyMap[AKEYCODE_R] = IRR_KEY_R; 
    KeyMap[AKEYCODE_S] = IRR_KEY_S; 
    KeyMap[AKEYCODE_T] = IRR_KEY_T; 
    KeyMap[AKEYCODE_U] = IRR_KEY_U; 
    KeyMap[AKEYCODE_V] = IRR_KEY_V; 
    KeyMap[AKEYCODE_W] = IRR_KEY_W; 
    KeyMap[AKEYCODE_X] = IRR_KEY_X; 
    KeyMap[AKEYCODE_Y] = IRR_KEY_Y; 
    KeyMap[AKEYCODE_Z] = IRR_KEY_Z; 
    KeyMap[AKEYCODE_COMMA] = IRR_KEY_COMMA; 
    KeyMap[AKEYCODE_PERIOD] = IRR_KEY_PERIOD; 
    KeyMap[AKEYCODE_ALT_LEFT] = IRR_KEY_MENU; 
    KeyMap[AKEYCODE_ALT_RIGHT] = IRR_KEY_MENU; 
    KeyMap[AKEYCODE_SHIFT_LEFT] = IRR_KEY_LSHIFT; 
    KeyMap[AKEYCODE_SHIFT_RIGHT] = IRR_KEY_RSHIFT; 
    KeyMap[AKEYCODE_TAB] = IRR_KEY_TAB; 
    KeyMap[AKEYCODE_SPACE] = IRR_KEY_SPACE; 
    KeyMap[AKEYCODE_SYM] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_EXPLORER] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_ENVELOPE] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_ENTER] = IRR_KEY_RETURN; 
    KeyMap[AKEYCODE_DEL] = IRR_KEY_BACK; 
    KeyMap[AKEYCODE_GRAVE] = IRR_KEY_OEM_3; 
    KeyMap[AKEYCODE_MINUS] = IRR_KEY_MINUS; 
    KeyMap[AKEYCODE_EQUALS] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_LEFT_BRACKET] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_RIGHT_BRACKET] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_BACKSLASH] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_SEMICOLON] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_APOSTROPHE] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_SLASH] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_AT] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_NUM] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_HEADSETHOOK] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_FOCUS] = IRR_KEY_UNKNOWN;
    KeyMap[AKEYCODE_PLUS] = IRR_KEY_PLUS; 
    KeyMap[AKEYCODE_MENU] = IRR_KEY_MENU; 
    KeyMap[AKEYCODE_NOTIFICATION] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_SEARCH] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_MEDIA_PLAY_PAUSE] = IRR_KEY_MEDIA_PLAY_PAUSE; 
    KeyMap[AKEYCODE_MEDIA_STOP] = IRR_KEY_MEDIA_STOP; 
    KeyMap[AKEYCODE_MEDIA_NEXT] = IRR_KEY_MEDIA_NEXT_TRACK; 
    KeyMap[AKEYCODE_MEDIA_PREVIOUS] = IRR_KEY_MEDIA_PREV_TRACK; 
    KeyMap[AKEYCODE_MEDIA_REWIND] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_MEDIA_FAST_FORWARD] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_MUTE] = IRR_KEY_VOLUME_MUTE; 
    KeyMap[AKEYCODE_PAGE_UP] = IRR_KEY_PRIOR; 
    KeyMap[AKEYCODE_PAGE_DOWN] = IRR_KEY_NEXT; 
    KeyMap[AKEYCODE_PICTSYMBOLS] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_SWITCH_CHARSET] = IRR_KEY_UNKNOWN; 

    // following look like controller inputs
    KeyMap[AKEYCODE_BUTTON_A] = IRR_KEY_BUTTON_A; 
    KeyMap[AKEYCODE_BUTTON_B] = IRR_KEY_BUTTON_B; 
    KeyMap[AKEYCODE_BUTTON_C] = IRR_KEY_BUTTON_C; 
    KeyMap[AKEYCODE_BUTTON_X] = IRR_KEY_BUTTON_X; 
    KeyMap[AKEYCODE_BUTTON_Y] = IRR_KEY_BUTTON_Y; 
    KeyMap[AKEYCODE_BUTTON_Z] = IRR_KEY_BUTTON_Z; 
    KeyMap[AKEYCODE_BUTTON_L1] = IRR_KEY_BUTTON_L1;  
    KeyMap[AKEYCODE_BUTTON_R1] = IRR_KEY_BUTTON_R1; 
    KeyMap[AKEYCODE_BUTTON_L2] = IRR_KEY_BUTTON_L2; 
    KeyMap[AKEYCODE_BUTTON_R2] = IRR_KEY_BUTTON_R2; 
    KeyMap[AKEYCODE_BUTTON_THUMBL] = IRR_KEY_BUTTON_THUMBL; 
    KeyMap[AKEYCODE_BUTTON_THUMBR] = IRR_KEY_BUTTON_THUMBR;  
    KeyMap[AKEYCODE_BUTTON_START] = IRR_KEY_BUTTON_START; 
    KeyMap[AKEYCODE_BUTTON_SELECT] = IRR_KEY_BUTTON_SELECT; 
    KeyMap[AKEYCODE_BUTTON_MODE] = IRR_KEY_BUTTON_MODE; 

    KeyMap[AKEYCODE_ESCAPE] = IRR_KEY_ESCAPE; 
    KeyMap[AKEYCODE_FORWARD_DEL] = IRR_KEY_DELETE; 
    KeyMap[AKEYCODE_CTRL_LEFT] = IRR_KEY_CONTROL; 
    KeyMap[AKEYCODE_CTRL_RIGHT] = IRR_KEY_CONTROL; 
    KeyMap[AKEYCODE_CAPS_LOCK] = IRR_KEY_CAPITAL; 
    KeyMap[AKEYCODE_SCROLL_LOCK] = IRR_KEY_SCROLL; 
    KeyMap[AKEYCODE_META_LEFT] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_META_RIGHT] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_FUNCTION] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_SYSRQ] = IRR_KEY_SNAPSHOT; 
    KeyMap[AKEYCODE_BREAK] = IRR_KEY_PAUSE; 
    KeyMap[AKEYCODE_MOVE_HOME] = IRR_KEY_HOME; 
    KeyMap[AKEYCODE_MOVE_END] = IRR_KEY_END; 
    KeyMap[AKEYCODE_INSERT] = IRR_KEY_INSERT; 
    KeyMap[AKEYCODE_FORWARD] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_MEDIA_PLAY] = IRR_KEY_PLAY; 
    KeyMap[AKEYCODE_MEDIA_PAUSE] = IRR_KEY_MEDIA_PLAY_PAUSE; 
    KeyMap[AKEYCODE_MEDIA_CLOSE] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_MEDIA_EJECT] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_MEDIA_RECORD] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_F1] = IRR_KEY_F1; 
    KeyMap[AKEYCODE_F2] = IRR_KEY_F2; 
    KeyMap[AKEYCODE_F3] = IRR_KEY_F3; 
    KeyMap[AKEYCODE_F4] = IRR_KEY_F4; 
    KeyMap[AKEYCODE_F5] = IRR_KEY_F5; 
    KeyMap[AKEYCODE_F6] = IRR_KEY_F6; 
    KeyMap[AKEYCODE_F7] = IRR_KEY_F7; 
    KeyMap[AKEYCODE_F8] = IRR_KEY_F8; 
    KeyMap[AKEYCODE_F9] = IRR_KEY_F9; 
    KeyMap[AKEYCODE_F10] = IRR_KEY_F10; 
    KeyMap[AKEYCODE_F11] = IRR_KEY_F11; 
    KeyMap[AKEYCODE_F12] = IRR_KEY_F12; 
    KeyMap[AKEYCODE_NUM_LOCK] = IRR_KEY_NUMLOCK; 
    KeyMap[AKEYCODE_NUMPAD_0] = IRR_KEY_NUMPAD0; 
    KeyMap[AKEYCODE_NUMPAD_1] = IRR_KEY_NUMPAD1; 
    KeyMap[AKEYCODE_NUMPAD_2] = IRR_KEY_NUMPAD2; 
    KeyMap[AKEYCODE_NUMPAD_3] = IRR_KEY_NUMPAD3; 
    KeyMap[AKEYCODE_NUMPAD_4] = IRR_KEY_NUMPAD4; 
    KeyMap[AKEYCODE_NUMPAD_5] = IRR_KEY_NUMPAD5; 
    KeyMap[AKEYCODE_NUMPAD_6] = IRR_KEY_NUMPAD6; 
    KeyMap[AKEYCODE_NUMPAD_7] = IRR_KEY_NUMPAD7; 
    KeyMap[AKEYCODE_NUMPAD_8] = IRR_KEY_NUMPAD8; 
    KeyMap[AKEYCODE_NUMPAD_9] = IRR_KEY_NUMPAD9; 
    KeyMap[AKEYCODE_NUMPAD_DIVIDE] = IRR_KEY_DIVIDE; 
    KeyMap[AKEYCODE_NUMPAD_MULTIPLY] = IRR_KEY_MULTIPLY; 
    KeyMap[AKEYCODE_NUMPAD_SUBTRACT] = IRR_KEY_SUBTRACT; 
    KeyMap[AKEYCODE_NUMPAD_ADD] = IRR_KEY_ADD; 
    KeyMap[AKEYCODE_NUMPAD_DOT] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_NUMPAD_COMMA] = IRR_KEY_COMMA; 
    KeyMap[AKEYCODE_NUMPAD_ENTER] = IRR_KEY_RETURN; 
    KeyMap[AKEYCODE_NUMPAD_EQUALS] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_NUMPAD_LEFT_PAREN] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_NUMPAD_RIGHT_PAREN] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_VOLUME_MUTE] = IRR_KEY_VOLUME_MUTE; 
    KeyMap[AKEYCODE_INFO] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_CHANNEL_UP] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_CHANNEL_DOWN] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_ZOOM_IN] = IRR_KEY_ZOOM; 
    KeyMap[AKEYCODE_ZOOM_OUT] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_TV] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_WINDOW] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_GUIDE] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_DVR] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_BOOKMARK] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_CAPTIONS] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_SETTINGS] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_TV_POWER] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_TV_INPUT] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_STB_POWER] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_STB_INPUT] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_AVR_POWER] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_AVR_INPUT] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_PROG_RED] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_PROG_GREEN] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_PROG_YELLOW] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_PROG_BLUE] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_APP_SWITCH] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_BUTTON_1] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_BUTTON_2] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_BUTTON_3] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_BUTTON_4] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_BUTTON_5] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_BUTTON_6] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_BUTTON_7] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_BUTTON_8] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_BUTTON_9] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_BUTTON_10] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_BUTTON_11] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_BUTTON_12] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_BUTTON_13] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_BUTTON_14] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_BUTTON_15] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_BUTTON_16] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_LANGUAGE_SWITCH] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_MANNER_MODE] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_3D_MODE] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_CONTACTS] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_CALENDAR] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_MUSIC] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_CALCULATOR] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_ZENKAKU_HANKAKU] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_EISU] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_MUHENKAN] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_HENKAN] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_KATAKANA_HIRAGANA] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_YEN] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_RO] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_KANA] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_ASSIST] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_BRIGHTNESS_DOWN] = IRR_KEY_UNKNOWN; 
    KeyMap[AKEYCODE_BRIGHTNESS_UP] = IRR_KEY_UNKNOWN;
    KeyMap[AKEYCODE_MEDIA_AUDIO_TRACK] = IRR_KEY_UNKNOWN; 
}

void CIrrDeviceAndroid::getKeyChar(SEvent& event)
{
    // Handle ASCII chars

    event.KeyInput.Char = 0;

    // A-Z
    if (event.KeyInput.SystemKeyCode > 28 && event.KeyInput.SystemKeyCode < 55)
    {
        if (event.KeyInput.Shift)
        {
            event.KeyInput.Char = event.KeyInput.SystemKeyCode + 36;
        }
        else
        {
            event.KeyInput.Char = event.KeyInput.SystemKeyCode + 68;
        }
    }

    // 0-9
    else if (event.KeyInput.SystemKeyCode > 6 && event.KeyInput.SystemKeyCode < 17)
    {
        event.KeyInput.Char = event.KeyInput.SystemKeyCode + 41;
    }

    else if (event.KeyInput.SystemKeyCode == AKEYCODE_BACK)
    {
        event.KeyInput.Char =  8;
    }
    else if (event.KeyInput.SystemKeyCode == AKEYCODE_DEL)
    {
        event.KeyInput.Char =  8;
    }
    else if (event.KeyInput.SystemKeyCode == AKEYCODE_TAB)
    {
        event.KeyInput.Char =  9;
    }
    else if (event.KeyInput.SystemKeyCode == AKEYCODE_ENTER)
    {
        event.KeyInput.Char =  13;
    }
    else if (event.KeyInput.SystemKeyCode == AKEYCODE_SPACE)
    {
        event.KeyInput.Char =  32;
    }
    else if (event.KeyInput.SystemKeyCode == AKEYCODE_COMMA)
    {
        event.KeyInput.Char =  44;
    }
    else if (event.KeyInput.SystemKeyCode == AKEYCODE_PERIOD)
    {
        event.KeyInput.Char =  46;
    }
}

int CIrrDeviceAndroid::getRotation()
{
    JavaVMAttachArgs args;
    args.version = JNI_VERSION_1_6;
    args.name = "NativeThread";
    args.group = NULL;
    JNIEnv* env;

    if (Android->activity->vm->AttachCurrentThread(&env, &args) != JNI_OK) 
    {
        os::Printer::log("Cannot find rotation.", ELL_DEBUG);
        return 0;
    }

    jobject activity_obj = Android->activity->clazz;

    jclass activity = env->GetObjectClass(activity_obj);
    jclass context = env->FindClass("android/content/Context");
    jclass window_manager = env->FindClass("android/view/WindowManager");
    jclass display = env->FindClass("android/view/Display");

    jmethodID get_system_service = env->GetMethodID(activity, "getSystemService", 
                                            "(Ljava/lang/String;)Ljava/lang/Object;");
    jmethodID get_default_display = env->GetMethodID(window_manager, 
                                                   "getDefaultDisplay", 
                                                   "()Landroid/view/Display;");
    jmethodID get_rotation = env->GetMethodID(display, "getRotation", "()I");

    jfieldID window_service = env->GetStaticFieldID(context, "WINDOW_SERVICE", 
                                                        "Ljava/lang/String;");

    jobject window_service_obj = env->GetStaticObjectField(context, window_service);

    jobject window_manager_obj = env->CallObjectMethod(activity_obj, get_system_service, 
                                                   window_service_obj);

    jobject display_obj = env->CallObjectMethod(window_manager_obj, get_default_display);
    
    int rotation = env->CallIntMethod(display_obj, get_rotation);
    
    env->DeleteLocalRef(activity);
    env->DeleteLocalRef(context);
    env->DeleteLocalRef(window_manager);
    env->DeleteLocalRef(display);   
    
    Android->activity->vm->DetachCurrentThread();
    
    return rotation;
}

DeviceOrientation CIrrDeviceAndroid::getDefaultOrientation()
{
    int rotation = getRotation();
    
    int32_t orientation = AConfiguration_getOrientation(Android->config);
    
    if (((rotation == 0 || rotation == 2) && 
        orientation == ACONFIGURATION_ORIENTATION_LAND) ||
        ((rotation == 1 || rotation == 3) && 
        orientation == ACONFIGURATION_ORIENTATION_PORT))
    {
        return ORIENTATION_LANDSCAPE;
    }
    else
    {
        return ORIENTATION_PORTRAIT;
    }
}

bool CIrrDeviceAndroid::activateAccelerometer(float updateInterval)
{
    if (!isAccelerometerAvailable())
        return false;
    
    if (DefaultOrientation == ORIENTATION_UNKNOWN)
    {
        DefaultOrientation = getDefaultOrientation();
    }

    ASensorEventQueue_enableSensor(SensorEventQueue, Accelerometer);
    ASensorEventQueue_setEventRate(SensorEventQueue, Accelerometer,
                    (int32_t)(updateInterval*1000.f*1000.f)); // in microseconds

    os::Printer::log("Activated accelerometer", ELL_DEBUG);
    return true;
}

bool CIrrDeviceAndroid::deactivateAccelerometer()
{
    if (!Accelerometer)
        return false;

    ASensorEventQueue_disableSensor(SensorEventQueue, Accelerometer);
    Accelerometer = 0;
    os::Printer::log("Deactivated accelerometer", ELL_DEBUG);
    return true;
}

bool CIrrDeviceAndroid::isAccelerometerActive()
{
    return (Accelerometer != NULL);
}

bool CIrrDeviceAndroid::isAccelerometerAvailable()
{
    if (!Accelerometer)
    {
        Accelerometer = ASensorManager_getDefaultSensor(SensorManager,
                                                    ASENSOR_TYPE_ACCELEROMETER);
    }

    return (Accelerometer != NULL);
}

bool CIrrDeviceAndroid::activateGyroscope(float updateInterval)
{
    if (!isGyroscopeAvailable())
        return false;

    ASensorEventQueue_enableSensor(SensorEventQueue, Gyroscope);
    ASensorEventQueue_setEventRate(SensorEventQueue, Gyroscope,
                    (int32_t)(updateInterval*1000.f*1000.f)); // in microseconds

    os::Printer::log("Activated gyroscope", ELL_DEBUG);
    return true;
}

bool CIrrDeviceAndroid::deactivateGyroscope()
{
    if (!Gyroscope)
        return false;

    ASensorEventQueue_disableSensor(SensorEventQueue, Gyroscope);
    Gyroscope = 0;
    os::Printer::log("Deactivated gyroscope", ELL_DEBUG);
    return true;
}

bool CIrrDeviceAndroid::isGyroscopeActive()
{
    return (Gyroscope != NULL);
}

bool CIrrDeviceAndroid::isGyroscopeAvailable()
{
    if (!Gyroscope)
    {
        Gyroscope = ASensorManager_getDefaultSensor(SensorManager,
                                                    ASENSOR_TYPE_GYROSCOPE);
    }

    return (Gyroscope != NULL);
}


} // end namespace irr

#endif


