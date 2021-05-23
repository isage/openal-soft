/**
 * OpenAL cross platform audio library
 * Copyright (C) 2018 by authors.
 * This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the
 *  Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * Or go to http://www.gnu.org/copyleft/lgpl.html
 */

#include "config.h"

#include <psp2/audioin.h>
#include <psp2/audioout.h>
#include <stdlib.h>

#include "alMain.h"
#include "alu.h"
#include "ringbuffer.h"
#include "threads.h"

#include "backends/base.h"

#define AUDIO_SAMPLE_ALIGN(s)   (((s) + 63) & ~63)

static const ALCchar playbackDeviceName[] = "PS Vita Speakers/Headphones";
static const ALCchar captureDeviceName[] = "PS Vita Microphone";

extern unsigned int _oal_thread_priority __attribute__((weak));
extern unsigned int _oal_thread_affinity __attribute__((weak));

// -----------------------------------------------------------------------------
// Playback
// -----------------------------------------------------------------------------
typedef struct ALCvitaPlayback
{
    DERIVE_FROM_TYPE(ALCbackend);

    ATOMIC(int) killNow;
    SceUID thread;
    SceKernelLwMutexWork lock;

    int portNumber;
    ALsizei frameSize;
    void* waveBuffer;

    ALuint Frequency;
    enum DevFmtChannels FmtChans;
    enum DevFmtType     FmtType;
    ALuint UpdateSize;
} ALCvitaPlayback;

static void ALCvitaPlayback_Construct(ALCvitaPlayback *self, ALCdevice *device);
static void ALCvitaPlayback_Destruct(ALCvitaPlayback *self);
static ALCenum ALCvitaPlayback_open(ALCvitaPlayback *self, const ALCchar *name);
static ALCboolean ALCvitaPlayback_reset(ALCvitaPlayback *self);
static ALCboolean ALCvitaPlayback_start(ALCvitaPlayback *self);
static void ALCvitaPlayback_stop(ALCvitaPlayback *self);
static void ALCvitaPlayback_lock(ALCvitaPlayback *self);
static void ALCvitaPlayback_unlock(ALCvitaPlayback *self);

static DECLARE_FORWARD2(ALCvitaPlayback, ALCbackend, ALCenum, captureSamples, void*, ALCuint)
static DECLARE_FORWARD(ALCvitaPlayback, ALCbackend, ALCuint, availableSamples)
static DECLARE_FORWARD(ALCvitaPlayback, ALCbackend, ClockLatency, getClockLatency)
DECLARE_DEFAULT_ALLOCATORS(ALCvitaPlayback)

DEFINE_ALCBACKEND_VTABLE(ALCvitaPlayback);

static void ALCvitaPlayback_Construct(ALCvitaPlayback *self, ALCdevice *device)
{
    ALCbackend_Construct(STATIC_CAST(ALCbackend, self), device);
    SET_VTABLE2(ALCvitaPlayback, ALCbackend, self);

    device->UpdateSize = AUDIO_SAMPLE_ALIGN(device->UpdateSize);

    self->portNumber = 0;
    self->frameSize = FrameSizeFromDevFmt(device->FmtChans, device->FmtType, device->AmbiOrder);
    self->Frequency = device->Frequency;
    self->FmtChans = device->FmtChans;
    self->FmtType = device->FmtType;
    self->UpdateSize = device->UpdateSize;

    sceKernelCreateLwMutex(
        &self->lock,
        "OpenAL Vita playback mutex",
        SCE_KERNEL_MUTEX_ATTR_RECURSIVE, // No SCE_KERNEL_LW_MUTEX_ATTR_RECURSIVE in VitaSDK, but it's the same
        0,
        NULL
    );
}

static void ALCvitaPlayback_Destruct(ALCvitaPlayback *self)
{
    if (self->portNumber)
    {
        sceAudioOutReleasePort(self->portNumber);
        self->portNumber = 0;
    }

    if (self->waveBuffer)
    {
        free(self->waveBuffer);
        self->waveBuffer = NULL;
    }

    sceKernelDeleteLwMutex(&self->lock);

    ALCbackend_Destruct(STATIC_CAST(ALCbackend, self));
}

static int ALCvitaPlayback_MixerProc(SceSize args, void *argp)
{
    (void)args;
    ALCvitaPlayback *self = *(ALCvitaPlayback **) argp;
    ALCdevice *device = STATIC_CAST(ALCbackend, self)->mDevice;

    while (!ATOMIC_LOAD(&self->killNow, almemory_order_acquire))
    {
        ALCvitaPlayback_lock(self);
        aluMixData(device, self->waveBuffer, device->UpdateSize);
        ALCvitaPlayback_unlock(self);
        sceAudioOutOutput(self->portNumber, self->waveBuffer);
    }

    return 0;
}

static ALCenum ALCvitaPlayback_open(ALCvitaPlayback *self, const ALCchar *name)
{
    ALCdevice *device = STATIC_CAST(ALCbackend, self)->mDevice;

    /* Only signed short output sample format is supported */
    device->FmtType = DevFmtShort;

    /* Only mono/stereo channel configurations are supported */
    if (device->FmtChans != DevFmtMono && device->FmtChans != DevFmtStereo)
        device->FmtChans = DevFmtStereo;

    device->UpdateSize = AUDIO_SAMPLE_ALIGN(device->UpdateSize);

    self->portNumber = sceAudioOutOpenPort(
        SCE_AUDIO_OUT_PORT_TYPE_BGM,
        device->UpdateSize,
        device->Frequency,
        device->FmtChans == DevFmtStereo ? SCE_AUDIO_OUT_MODE_STEREO : SCE_AUDIO_OUT_MODE_MONO
    );

    if (self->portNumber < 0)
        return ALC_INVALID_VALUE;


    self->frameSize = FrameSizeFromDevFmt(device->FmtChans, device->FmtType, device->AmbiOrder);
    self->Frequency = device->Frequency;
    self->FmtChans = device->FmtChans;
    self->FmtType = device->FmtType;
    self->UpdateSize = device->UpdateSize;

    self->waveBuffer = calloc(device->UpdateSize * self->frameSize, 1);

    alstr_copy_cstr(&device->DeviceName, name ? name : playbackDeviceName);

    return ALC_NO_ERROR;
}

static ALCboolean ALCvitaPlayback_reset(ALCvitaPlayback *self)
{
    ALCdevice *device = STATIC_CAST(ALCbackend, self)->mDevice;

    if (device->FmtChans != DevFmtMono && device->FmtChans != DevFmtStereo)
        device->FmtChans = DevFmtStereo;

    device->UpdateSize = AUDIO_SAMPLE_ALIGN(device->UpdateSize);

    sceAudioOutSetConfig(
        self->portNumber,
        device->UpdateSize,
        device->Frequency,
        device->FmtChans == DevFmtStereo ? SCE_AUDIO_OUT_MODE_STEREO : SCE_AUDIO_OUT_MODE_MONO
    );

    self->frameSize = FrameSizeFromDevFmt(device->FmtChans, device->FmtType, device->AmbiOrder);
    self->Frequency = device->Frequency;
    self->FmtChans = device->FmtChans;
    self->FmtType = device->FmtType;
    self->UpdateSize = device->UpdateSize;

    if (self->waveBuffer)
    {
        free(self->waveBuffer);
    }

    self->waveBuffer = calloc(device->UpdateSize * self->frameSize, 1);

    SetDefaultWFXChannelOrder(device);

    return ALC_TRUE;
}

static ALCboolean ALCvitaPlayback_start(ALCvitaPlayback *self)
{
    ATOMIC_STORE(&self->killNow, AL_FALSE, almemory_order_release);

    int priority = 32;
    int affinity = 0; // DEFAULT
    int stack_size = 0x10000; // 64Kib

    if (&_oal_thread_priority != NULL) {
        priority = _oal_thread_priority;
    } else {
        SceKernelThreadInfo info;
        info.size = sizeof(SceKernelThreadInfo);
        if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) == 0) {
            priority = info.currentPriority - 1;
        }
    }

    if (&_oal_thread_affinity != NULL) {
        affinity = _oal_thread_affinity;
    }

    self->thread = sceKernelCreateThread("OpenAL Vita playback thread", ALCvitaPlayback_MixerProc,
                           priority, stack_size, 0, affinity, NULL);

    if (self->thread < 0)
        return ALC_FALSE;

    int ret = sceKernelStartThread(self->thread, 4, &self);
    if (ret < 0)
        return ALC_FALSE;

    return ALC_TRUE;
}

static void ALCvitaPlayback_stop(ALCvitaPlayback *self)
{
    if (ATOMIC_EXCHANGE(&self->killNow, AL_TRUE, almemory_order_acq_rel))
        return;

    sceKernelWaitThreadEnd(self->thread, NULL, NULL);
    sceKernelDeleteThread(self->thread);
}

static void ALCvitaPlayback_lock(ALCvitaPlayback *self)
{
    sceKernelLockLwMutex(&self->lock, 1, NULL);
}

static void ALCvitaPlayback_unlock(ALCvitaPlayback *self)
{
    sceKernelUnlockLwMutex(&self->lock, 1);
}

// -----------------------------------------------------------------------------
// Capture
// -----------------------------------------------------------------------------
typedef struct ALCvitaCapture
{
    DERIVE_FROM_TYPE(ALCbackend);

    ATOMIC(int) killNow;
    althrd_t thread;
    SceKernelLwMutexWork lock;

    int portNumber;
    ALsizei frameSize;
    ll_ringbuffer_t *ring;

    ALuint Frequency;
    enum DevFmtChannels FmtChans;
    enum DevFmtType     FmtType;
    ALuint UpdateSize;
} ALCvitaCapture;

static void ALCvitaCapture_Construct(ALCvitaCapture *self, ALCdevice *device);
static void ALCvitaCapture_Destruct(ALCvitaCapture *self);
static ALCenum ALCvitaCapture_open(ALCvitaCapture *self, const ALCchar *name);
static ALCboolean ALCvitaCapture_reset(ALCvitaCapture *self);
static ALCboolean ALCvitaCapture_start(ALCvitaCapture *self);
static void ALCvitaCapture_stop(ALCvitaCapture *self);
static void ALCvitaCapture_lock(ALCvitaCapture *self);
static void ALCvitaCapture_unlock(ALCvitaCapture *self);
static ALCenum ALCvitaCapture_captureSamples(ALCvitaCapture *self, ALCvoid *buffer, ALCuint samples);
static ALCuint ALCvitaCapture_availableSamples(ALCvitaCapture *self);
static DECLARE_FORWARD(ALCvitaCapture, ALCbackend, ClockLatency, getClockLatency)
DECLARE_DEFAULT_ALLOCATORS(ALCvitaCapture)

DEFINE_ALCBACKEND_VTABLE(ALCvitaCapture);

static void ALCvitaCapture_Construct(ALCvitaCapture *self, ALCdevice *device)
{
    ALCbackend_Construct(STATIC_CAST(ALCbackend, self), device);
    SET_VTABLE2(ALCvitaCapture, ALCbackend, self);

    self->portNumber = 0;
    self->frameSize = FrameSizeFromDevFmt(device->FmtChans, device->FmtType, device->AmbiOrder);
    self->Frequency = device->Frequency;
    self->FmtChans = device->FmtChans;
    self->FmtType = device->FmtType;
    self->UpdateSize = device->UpdateSize;
}

static void ALCvitaCapture_Destruct(ALCvitaCapture *self)
{
    if (self->portNumber)
    {
        sceAudioOutReleasePort(self->portNumber);
        self->portNumber = 0;
    }

    if (self->ring)
    {
        ll_ringbuffer_free(self->ring);
        self->ring = NULL;
    }

    ALCbackend_Destruct(STATIC_CAST(ALCbackend, self));
}

static int ALCvitaCapture_MixerProc(void *ptr)
{
    ALCvitaCapture *self = (ALCvitaCapture*)ptr;
    ALCdevice *device = STATIC_CAST(ALCbackend, self)->mDevice;

    void* buf = malloc(self->frameSize * device->UpdateSize);

    while (!ATOMIC_LOAD(&self->killNow, almemory_order_acquire))
    {
        sceAudioInInput(self->portNumber, buf);
        ll_ringbuffer_write(self->ring, buf, self->frameSize * device->UpdateSize);
    }

    free(buf);

    return 0;
}

static ALCenum ALCvitaCapture_open(ALCvitaCapture *self, const ALCchar *name)
{
    ALCdevice *device = STATIC_CAST(ALCbackend, self)->mDevice;

    /* Only signed short output sample format is supported */
    device->FmtType = DevFmtShort;

    /* Only mono channel configuration is supported */
    if (device->FmtChans != DevFmtMono)
        device->FmtChans = DevFmtMono;

    /* TODO: Validate samplerate and update size */

    self->portNumber = sceAudioInOpenPort(
        SCE_AUDIO_IN_PORT_TYPE_RAW,
        device->UpdateSize,
        device->Frequency,
        SCE_AUDIO_IN_PARAM_FORMAT_S16_MONO
    );

    if (self->portNumber < 0)
    {
        return ALC_INVALID_VALUE;
    }

    self->frameSize = FrameSizeFromDevFmt(device->FmtChans, device->FmtType, device->AmbiOrder);
    self->Frequency = device->Frequency;
    self->FmtChans = device->FmtChans;
    self->FmtType = device->FmtType;
    self->UpdateSize = device->UpdateSize;

    self->ring = ll_ringbuffer_create(device->UpdateSize * device->NumUpdates, self->frameSize, false);
    if (self->ring == NULL)
        return ALC_INVALID_VALUE;

    alstr_copy_cstr(&device->DeviceName, name ? name : captureDeviceName);

    return ALC_NO_ERROR;
}

static ALCboolean ALCvitaCapture_reset(ALCvitaCapture *self)
{
    ALCdevice *device = STATIC_CAST(ALCbackend, self)->mDevice;
    SetDefaultWFXChannelOrder(device);
    return ALC_TRUE;
}

static ALCboolean ALCvitaCapture_start(ALCvitaCapture *self)
{
    ATOMIC_STORE(&self->killNow, AL_FALSE, almemory_order_release);
    if (althrd_create(&self->thread, ALCvitaCapture_MixerProc, self) != althrd_success)
        return ALC_FALSE;

    return ALC_TRUE;
}

static void ALCvitaCapture_stop(ALCvitaCapture *self)
{
    int res;

    if (ATOMIC_EXCHANGE(&self->killNow, AL_TRUE, almemory_order_acq_rel))
        return;

    althrd_join(self->thread, &res);
}

static void ALCvitaCapture_lock(ALCvitaCapture *self)
{
    sceKernelLockLwMutex(&self->lock, 1, NULL);
}

static void ALCvitaCapture_unlock(ALCvitaCapture *self)
{
    sceKernelUnlockLwMutex(&self->lock, 1);
}


static ALCuint ALCvitaCapture_availableSamples(ALCvitaCapture *self)
{
    return ll_ringbuffer_read_space(self->ring);
}

static ALCenum ALCvitaCapture_captureSamples(ALCvitaCapture *self, ALCvoid *buffer, ALCuint samples)
{
    ll_ringbuffer_read(self->ring, buffer, samples);
    return ALC_NO_ERROR;
}

// -----------------------------------------------------------------------------
// Backends
// -----------------------------------------------------------------------------

typedef struct ALCvitaBackendFactory {
    DERIVE_FROM_TYPE(ALCbackendFactory);
} ALCvitaBackendFactory;
#define ALCvitaBACKENDFACTORY_INITIALIZER { { GET_VTABLE2(ALCvitaBackendFactory, ALCbackendFactory) } }

ALCbackendFactory *ALCvitaBackendFactory_getFactory(void);

static ALCboolean ALCvitaBackendFactory_init(ALCvitaBackendFactory *self);
static void ALCvitaBackendFactory_deinit(ALCvitaBackendFactory *self);
static ALCboolean ALCvitaBackendFactory_querySupport(ALCvitaBackendFactory *self, ALCbackend_Type type);
static void ALCvitaBackendFactory_probe(ALCvitaBackendFactory *self, enum DevProbe type, al_string *outnames);
static ALCbackend* ALCvitaBackendFactory_createBackend(ALCvitaBackendFactory *self, ALCdevice *device, ALCbackend_Type type);

DEFINE_ALCBACKENDFACTORY_VTABLE(ALCvitaBackendFactory);

ALCbackendFactory *ALCvitaBackendFactory_getFactory(void)
{
    static ALCvitaBackendFactory factory = ALCvitaBACKENDFACTORY_INITIALIZER;
    return STATIC_CAST(ALCbackendFactory, &factory);
}


static ALCboolean ALCvitaBackendFactory_init(ALCvitaBackendFactory* UNUSED(self))
{
    return AL_TRUE;
}

static void ALCvitaBackendFactory_deinit(ALCvitaBackendFactory* UNUSED(self))
{
}

static ALCboolean ALCvitaBackendFactory_querySupport(ALCvitaBackendFactory* UNUSED(self), ALCbackend_Type type)
{
    if (type == ALCbackend_Playback || type == ALCbackend_Capture)
        return ALC_TRUE;

    return ALC_FALSE;
}

static void ALCvitaBackendFactory_probe(ALCvitaBackendFactory* UNUSED(self), enum DevProbe type, al_string *outnames)
{
    if (type == ALL_DEVICE_PROBE)
        alstr_append_range(outnames, playbackDeviceName, playbackDeviceName+sizeof(playbackDeviceName));
    else if (type == CAPTURE_DEVICE_PROBE)
        alstr_append_range(outnames, captureDeviceName, playbackDeviceName+sizeof(captureDeviceName));
}

static ALCbackend* ALCvitaBackendFactory_createBackend(ALCvitaBackendFactory* UNUSED(self), ALCdevice *device, ALCbackend_Type type)
{
    if (type == ALCbackend_Playback)
    {
        ALCvitaPlayback *backend;
        NEW_OBJ(backend, ALCvitaPlayback)(device);

        if (!backend)
            return NULL;

        return STATIC_CAST(ALCbackend, backend);
    }

    if (type == ALCbackend_Capture)
    {
        ALCvitaCapture *backend;
        NEW_OBJ(backend, ALCvitaCapture)(device);

        if (!backend)
            return NULL;

        return STATIC_CAST(ALCbackend, backend);
    }
    return NULL;
}
