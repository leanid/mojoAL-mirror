/**
 * MojoAL; a simple drop-in OpenAL implementation.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon.
 */

#include <execinfo.h>

#define APPNAME "altrace_record"

#ifdef _MSC_VER
  #define AL_API __declspec(dllexport)
  #define ALC_API __declspec(dllexport)
#endif

#include "altrace_common.h"

// not in the headers, natch.
AL_API void AL_APIENTRY alTracePushScope(const ALchar *str);
AL_API void AL_APIENTRY alTracePopScope(void);
AL_API void AL_APIENTRY alTraceMessage(const ALchar *str);
AL_API void AL_APIENTRY alTraceBufferLabel(ALuint name, const ALchar *str);
AL_API void AL_APIENTRY alTraceSourceLabel(ALuint name, const ALchar *str);
AL_API void AL_APIENTRY alcTraceDeviceLabel(ALCdevice *device, const ALchar *str);
AL_API void AL_APIENTRY alcTraceContextLabel(ALCcontext *ctx, const ALchar *str);


static pthread_mutex_t _apilock;
static pthread_mutex_t *apilock;

typedef struct DeviceWrapper
{
    ALCdevice *device;
    ALCenum errorlatch;
    int samplesize;   /* size of a capture device sample in bytes */
    char *extension_string;
} DeviceWrapper;

typedef struct ContextWrapper
{
    ALCcontext *ctx;
    DeviceWrapper *device;
    char *extension_string;
    ALenum errorlatch;
} ContextWrapper;

static DeviceWrapper null_device;
static ContextWrapper *current_context;


static void quit_altrace_record(void) __attribute__((destructor));

// override _exit(), which terminates the process without running library
//  destructors, so we can close our log file, etc.
void _exit(int status)
{
    quit_altrace_record();
    _Exit(status);  // just use _Exit(), which does the same thing but no one really uses.  :P
}


NORETURN static void IO_WRITE_FAIL(void)
{
    fprintf(stderr, APPNAME ": failed to write to log: %s\n", strerror(errno));
    quit_altrace_record();
    _exit(42);
}

static void writele32(const uint32 x)
{
    const uint32 y = swap32(x);
    if (write(logfd, &y, sizeof (y)) != sizeof (y)) {
        IO_WRITE_FAIL();
    }
}

static void writele64(const uint64 x)
{
    const uint64 y = swap64(x);
    if (write(logfd, &y, sizeof (y)) != sizeof (y)) {
        IO_WRITE_FAIL();
    }
}

static void IO_INT32(const int32 x)
{
    union { int32 si32; uint32 ui32; } cvt;
    cvt.si32 = x;
    writele32(cvt.ui32);
}

static void IO_UINT32(const uint32 x)
{
    writele32(x);
}

static void IO_UINT64(const uint64 x)
{
    writele64(x);
}

static void IO_ALCSIZEI(const ALCsizei x)
{
    IO_UINT64((uint64) x);
}

static void IO_ALSIZEI(const ALsizei x)
{
    IO_UINT64((uint64) x);
}

static void IO_FLOAT(const float x)
{
    union { float f; uint32 ui32; } cvt;
    cvt.f = x;
    IO_UINT32(cvt.ui32);
}

static void IO_DOUBLE(const double x)
{
    union { double d; uint64 ui64; } cvt;
    cvt.d = x;
    IO_UINT64(cvt.ui64);
}

static void IO_STRING(const char *str)
{
    if (!str) {
        IO_UINT64(0xFFFFFFFFFFFFFFFFull);
    } else {
        const size_t len = strlen(str);
        IO_UINT64((uint64) len);
        if (len > 0) {
            if (write(logfd, str, len) != len) IO_WRITE_FAIL();
        }
    }
}

static void IO_BLOB(const uint8 *data, const uint64 len)
{
    if (!data) {
        IO_UINT64(0xFFFFFFFFFFFFFFFFull);
    } else {
        const size_t slen = (size_t) len;
        IO_UINT64(len);
        if (len > 0) {
            if (write(logfd, data, slen) != slen) IO_WRITE_FAIL();
        }
    }
}

static void IO_EVENTENUM(const EventEnum x)
{
    IO_UINT32((uint32) x);
}

static void IO_PTR(const void *ptr)
{
    IO_UINT64((uint64) (size_t) ptr);
}

static void IO_ALCENUM(ALCenum e)
{
    IO_UINT32((uint32) e);
}

static void IO_ENUM(ALenum e)
{
    IO_UINT32((uint32) e);
}

static void IO_ALCBOOLEAN(ALCboolean b)
{
    IO_UINT32((uint32) b);
}

static void IO_BOOLEAN(ALboolean b)
{
    IO_UINT32((uint32) b);
}


static void free_hash_item_stackframe(void *from, char *to) { free(to); }
static uint8 hash_stackframe(void *from) {
    // everything is going to end in a multiple of pointer size, so flatten down.
    const size_t val = ((size_t) from) / (sizeof (void *));
    return (uint8) (val & 0xFF);  // good enough, I guess.
}
HASH_MAP(stackframe, void *, char *)

// backtrace_symbols() is pretty expensive, so we don't want to run it
//  dozens of times per-frame. So we call it on individual frames when
//  we haven't seen them before, assuming most of our calls come from a
//  handful of places, and even there we can reuse most of the callstack
//  frames anyhow.
static char *get_callstack_sym(void *frame, int *_seen_before)
{
    char *retval = get_mapped_stackframe(frame);
    if (retval) {
        *_seen_before = 1;
    } else {
        char **syms = backtrace_symbols(&frame, 1);
        retval = syms[0] ? strdup(syms[0]) : NULL;
        free(syms);
        *_seen_before = 0;
        if (retval) {
            add_stackframe_to_map(frame, retval);
        }
    }
    return retval;
}

__attribute__((noinline)) static void IO_ENTRYINFO(const EventEnum entryid)
{
    const uint32 now = NOW();
    const size_t MAX_CALLSTACKS = 32;
    void* callstack[MAX_CALLSTACKS + 2];
    char *new_strings[MAX_CALLSTACKS];
    void *new_strings_ptrs[MAX_CALLSTACKS];
    int frames = backtrace(callstack, MAX_CALLSTACKS);
    int num_new_strings = 0;
    int i;

    frames -= 2;  // skip IO_ENTRYINFO and entry point.
    if (frames < 0) {
        frames = 0;
    }

    for (i = 0; i < frames; i++) {
        int seen_before = 0;
        void *ptr = callstack[i + 2];
        char *str = get_callstack_sym(ptr, &seen_before);
        if ((str == NULL) && !seen_before) {
            break;
        }

        if (!seen_before) {
            new_strings[num_new_strings] = str;
            new_strings_ptrs[num_new_strings] = ptr;
            num_new_strings++;
        }
    }
    frames = i;  /* in case we stopped early. */

    if (num_new_strings > 0) {
        IO_UINT32(now);
        IO_EVENTENUM(ALEE_NEW_CALLSTACK_SYMS);
        IO_UINT32((uint32) num_new_strings);
        for (i = 0; i < num_new_strings; i++) {
            IO_PTR(new_strings_ptrs[i]);
            IO_STRING(new_strings[i]);
        }
    }

    IO_UINT32(now);
    IO_EVENTENUM(entryid);
    IO_UINT64((uint64) pthread_self());

    IO_UINT32((uint32) frames);
    for (i = 0; i < frames; i++) {
        IO_PTR(callstack[i + 2]);
    }
}

static void APILOCK(void)
{
    const int rc = pthread_mutex_lock(apilock);
    if (rc != 0) {
        fprintf(stderr, APPNAME ": Failed to grab API lock: %s\n", strerror(rc));
        quit_altrace_record();
        _exit(42);
    }
}

static void APIUNLOCK(void)
{
    const int rc = pthread_mutex_unlock(apilock);
    if (rc != 0) {
        fprintf(stderr, APPNAME ": Failed to release API lock: %s\n", strerror(rc));
        quit_altrace_record();
        _exit(42);
    }
}

static void check_al_error_events(void)
{
    if (current_context) {
        const ALenum alerr = REAL_alGetError();
        if (alerr != AL_NO_ERROR) {
            IO_UINT32(NOW());
            IO_EVENTENUM(ALEE_ALERROR_TRIGGERED);
            IO_ENUM(alerr);
            if (current_context->errorlatch == AL_NO_ERROR) {
                current_context->errorlatch = alerr;
            }
        }
    }
}

static void check_alc_error_events(DeviceWrapper *device)
{
    if (device) {
        const ALCenum alcerr = REAL_alcGetError(device->device);
        if (alcerr != ALC_NO_ERROR) {
            IO_UINT32(NOW());
            IO_EVENTENUM(ALEE_ALCERROR_TRIGGERED);
            IO_PTR(device);
            IO_ALCENUM(alcerr);
            if (device->errorlatch == ALC_NO_ERROR) {
                device->errorlatch = alcerr;
            }
        }
    }
}

#define IO_START(e) \
    { \
        APILOCK(); \
        IO_ENTRYINFO(ALEE_##e)

#define IO_END() \
        check_al_error_events(); \
        APIUNLOCK(); \
    }

#define IO_END_ALC(dev) \
        check_alc_error_events(dev); \
        APIUNLOCK(); \
    }

static void init_altrace_record(void) __attribute__((constructor));
static void init_altrace_record(void)
{
    int okay = 1;

    fprintf(stderr, "\n\n\n" APPNAME ": starting up...\n");
    fflush(stderr);

    if (!init_clock()) {
        fflush(stderr);
        _exit(42);
    }

    if (!load_real_openal()) {
        _exit(42);
    }

    if (okay) {
        const int rc = pthread_mutex_init(&_apilock, NULL);
        if (rc != 0) {
            fprintf(stderr, APPNAME ": Failed to create mutex: %s\n", strerror(rc));
            okay = 0;
        }
        apilock = &_apilock;
    }

    if (okay) {
        const char *filename = "altrace.trace";
        logfd = open(filename, O_WRONLY | O_TRUNC | O_CREAT, 0644);
        if (logfd == -1) {
            fprintf(stderr, APPNAME ": Failed to open OpenAL log file '%s': %s\n", filename, strerror(errno));
            okay = 0;
        } else {
            fprintf(stderr, APPNAME ": Recording OpenAL session to log file '%s'\n\n\n", filename);
        }
    }

    fflush(stderr);

    if (!okay) {
        quit_altrace_record();
        _exit(42);
    }

    IO_UINT32(ALTRACE_LOG_FILE_MAGIC);
    IO_UINT32(ALTRACE_LOG_FILE_FORMAT);
}

static void quit_altrace_record(void)
{
    const int io = logfd;
    pthread_mutex_t *mutex = apilock;

    logfd = -1;
    apilock = NULL;

    fprintf(stderr, APPNAME ": Shutting down...\n");
    fflush(stderr);

    if (io != -1) {
        const uint32 ticks = swap32(NOW());
        const uint32 eos = swap32((uint32) ALEE_EOS);
        if ((write(io, &ticks, 4) != 4) || (write(io, &eos, 4) != 4)) {
            fprintf(stderr, APPNAME ": Failed to write EOS to OpenAL log file: %s\n", strerror(errno));
        }
        if (close(io) < 0) {
            fprintf(stderr, APPNAME ": Failed to close OpenAL log file: %s\n", strerror(errno));
        }
    }

    if (mutex) {
        pthread_mutex_destroy(mutex);
    }

    #define ENTRYPOINT(ret,name,params,args) REAL_##name = NULL;
    #include "altrace_entrypoints.h"

    close_real_openal();
    free_stackframe_map();

    fflush(stderr);
}



ALCcontext *alcGetCurrentContext(void)
{
    ALCcontext *retval;
    IO_START(alcGetCurrentContext);
    retval = REAL_alcGetCurrentContext();
    (void) retval; // !!! FIXME: assert this hasn't gone out of sync with current_context...
    IO_PTR(current_context);
    IO_END_ALC(NULL);
    return (ALCcontext *) current_context;
}

ALCdevice *alcGetContextsDevice(ALCcontext *_ctx)
{
    ContextWrapper *ctx = (ContextWrapper *) _ctx;
    ALCdevice *retval;
    IO_START(alcGetContextsDevice);
    IO_PTR(ctx);
    retval = REAL_alcGetContextsDevice(ctx->ctx);
    (void) retval; // !!! FIXME: assert this hasn't gone out of sync with current_context...
    IO_PTR(ctx->device);
    IO_END_ALC(ctx->device);
    return (ALCdevice *) ctx->device;
}

ALCboolean alcIsExtensionPresent(ALCdevice *_device, const ALCchar *extname)
{
    DeviceWrapper *device = _device ? (DeviceWrapper *) _device : &null_device;
    ALCboolean retval;
    IO_START(alcIsExtensionPresent);
    IO_PTR(_device);
    IO_STRING(extname);
    if (strcasecmp(extname, "ALC_EXT_trace_info") == 0) {
        retval = ALC_TRUE;
    } else {
        retval = REAL_alcIsExtensionPresent(device->device, extname);
    }
    IO_ALCBOOLEAN(retval);
    IO_END_ALC(device);
    return retval;
}

void *alcGetProcAddress(ALCdevice *_device, const ALCchar *funcname)
{
    DeviceWrapper *device = _device ? (DeviceWrapper *) _device : &null_device;
    void *retval = NULL;
    IO_START(alcGetProcAddress);
    IO_PTR(_device);
    IO_STRING(funcname);

    // always return our entry points, so the app always calls through here.
    if (!funcname || ((funcname[0] != 'a') || (funcname[1] != 'l') || (funcname[2] != 'c'))) {
        // !!! FIXME: should set an error state.
        retval = NULL;
    }
    #define ENTRYPOINT(ret,fn,params,args) else if (strcmp(funcname, #fn) == 0) { retval = (void *) fn; }
    #include "altrace_entrypoints.h"

    IO_PTR(retval);
    IO_END_ALC(device);
    return retval;

}

ALCenum alcGetEnumValue(ALCdevice *_device, const ALCchar *enumname)
{
    DeviceWrapper *device = _device ? (DeviceWrapper *) _device : &null_device;
    ALCenum retval;
    IO_START(alcGetEnumValue);
    IO_PTR(_device);
    IO_STRING(enumname);
    retval = REAL_alcGetEnumValue(device->device, enumname);
    IO_ALCENUM(retval);
    IO_END_ALC(device);
    return retval;
}

const ALCchar *alcGetString(ALCdevice *_device, ALCenum param)
{
    DeviceWrapper *device = _device ? (DeviceWrapper *) _device : &null_device;
    const ALCchar *retval;
    IO_START(alcGetString);
    IO_PTR(_device);
    IO_ALCENUM(param);
    retval = REAL_alcGetString(device->device, param);

    if ((param == ALC_EXTENSIONS) && retval) {
        const char *addstr = "ALC_EXT_trace_info";
        const size_t slen = strlen(retval) + strlen(addstr) + 2;
        char *ptr = (char *) realloc(device->extension_string, slen);
        if (ptr) {
            device->extension_string = ptr;
            snprintf(ptr, slen, "%s%s%s", retval, *retval ? " " : "", addstr);
            retval = (const ALCchar *) ptr;
        }
    }

    IO_STRING(retval);
    IO_END_ALC(device);
    return retval;
}

ALCdevice *alcCaptureOpenDevice(const ALCchar *devicename, ALCuint frequency, ALCenum format, ALCsizei buffersize)
{
    DeviceWrapper *device = (DeviceWrapper *) calloc(1, sizeof (DeviceWrapper));
    ALCdevice *retval;

    if (!device) {
        return NULL;
    }

    IO_START(alcCaptureOpenDevice);
    IO_STRING(devicename);
    IO_UINT32(frequency);
    IO_ALCENUM(format);
    IO_ALSIZEI(buffersize);
    retval = REAL_alcCaptureOpenDevice(devicename, frequency, format, buffersize);
    IO_PTR(retval ? device : NULL);

    if (!retval) {
        free(device);
    } else {
        device->device = retval;
        if (format == AL_FORMAT_MONO8) {
            device->samplesize = 1;
        } else if (format == AL_FORMAT_MONO16) {
            device->samplesize = 2;
        } else if (format == AL_FORMAT_STEREO8) {
            device->samplesize = 2;
        } else if (format == AL_FORMAT_STEREO16) {
            device->samplesize = 4;
            // !!! FIXME: float32
        }
    }

    IO_END_ALC(device);
    return (ALCdevice *) device;
}

ALCboolean alcCaptureCloseDevice(ALCdevice *_device)
{
    DeviceWrapper *device = _device ? (DeviceWrapper *) _device : &null_device;
    ALCboolean retval;
    IO_START(alcCaptureCloseDevice);
    IO_PTR(_device);
    retval = REAL_alcCaptureCloseDevice(device->device);
    IO_ALCBOOLEAN(retval);

    if (retval == ALC_TRUE) {
        free(device->extension_string);
        free(device);
    }

    IO_END_ALC(retval == ALC_TRUE ? NULL : device)
    return retval;
}

ALCdevice *alcOpenDevice(const ALCchar *devicename)
{
    DeviceWrapper *device = (DeviceWrapper *) calloc(1, sizeof (DeviceWrapper));
    ALCdevice *retval;

    if (!device) {
        return NULL;
    }

    IO_START(alcOpenDevice);
    IO_STRING(devicename);
    retval = REAL_alcOpenDevice(devicename);
    IO_PTR(retval ? device : NULL);

    if (!retval) {
        free(device);
    } else {
        device->device = retval;
    }

    IO_END_ALC(device);
    return (ALCdevice *) device;
}

ALCboolean alcCloseDevice(ALCdevice *_device)
{
    DeviceWrapper *device = _device ? (DeviceWrapper *) _device : &null_device;
    ALCboolean retval;
    IO_START(alcCloseDevice);
    IO_PTR(_device);
    retval = REAL_alcCloseDevice(device->device);
    IO_ALCBOOLEAN(retval);

    if (retval == ALC_TRUE) {
        free(device->extension_string);
        free(device);
    }

    IO_END_ALC(retval == ALC_TRUE ? NULL : device)
    return retval;
}

ALCcontext *alcCreateContext(ALCdevice *_device, const ALCint* attrlist)
{
    DeviceWrapper *device = _device ? (DeviceWrapper *) _device : &null_device;
    ContextWrapper *ctx = (ContextWrapper *) calloc(1, sizeof (ContextWrapper));
    ALCcontext *retval;
    uint32 attrcount = 0;
    uint32 i;

    if (!ctx) {
        if (device->errorlatch == ALC_NO_ERROR) {
            device->errorlatch = ALC_OUT_OF_MEMORY;
        }
        return NULL;
    }

    IO_START(alcCreateContext);
    IO_PTR(_device);
    if (attrlist) {
        while (attrlist[attrcount] != 0) { attrcount += 2; }
        attrcount++;
    }
    IO_UINT32(attrcount);
    if (attrlist) {
        for (i = 0; i < attrcount; i++) {
            IO_INT32(attrlist[i]);
        }
    }
    retval = REAL_alcCreateContext(device->device, attrlist);
    IO_PTR(retval ? device : NULL);

    if (retval == NULL) {
        free(ctx);
    } else {
        ctx->ctx = retval;
        ctx->device = device;
    }

    IO_END_ALC(device);
    return (ALCcontext *) ctx;
    
}

ALCboolean alcMakeContextCurrent(ALCcontext *_ctx)
{
    ContextWrapper *ctx = (ContextWrapper *) _ctx;
    ALCboolean retval;
    IO_START(alcMakeContextCurrent);
    IO_PTR(ctx);
    retval = REAL_alcMakeContextCurrent(ctx ? ctx->ctx : NULL);
    IO_ALCBOOLEAN(retval);
    if (retval) {
        current_context = ctx;
    }
    IO_END_ALC(current_context ? current_context->device : NULL);
    return retval;
}

void alcProcessContext(ALCcontext *_ctx)
{
    ContextWrapper *ctx = (ContextWrapper *) _ctx;
    IO_START(alcProcessContext);
    IO_PTR(ctx);
    REAL_alcProcessContext(ctx ? ctx->ctx : NULL);
    IO_END_ALC(ctx ? ctx->device : NULL);
}

void alcSuspendContext(ALCcontext *_ctx)
{
    ContextWrapper *ctx = (ContextWrapper *) _ctx;
    IO_START(alcSuspendContext);
    IO_PTR(ctx);
    REAL_alcSuspendContext(ctx ? ctx->ctx : NULL);
    IO_END_ALC(ctx ? ctx->device : NULL);
}

void alcDestroyContext(ALCcontext *_ctx)
{
    ContextWrapper *ctx = (ContextWrapper *) _ctx;
    DeviceWrapper *device = NULL;
    IO_START(alcDestroyContext);
    IO_PTR(ctx);
    REAL_alcDestroyContext(ctx ? ctx->ctx : NULL);
    if (ctx) {
        device = ctx->device;
        free(ctx->extension_string);
        free(ctx);
    }
    IO_END_ALC(device);
}

ALCenum alcGetError(ALCdevice *_device)
{
    DeviceWrapper *device = _device ? (DeviceWrapper *) _device : &null_device;
    ALCenum retval;
    IO_START(alcGetError);
    IO_PTR(_device);
    retval = device->errorlatch;
    device->errorlatch = ALC_NO_ERROR;
    IO_ALCENUM(retval);
    IO_END_ALC(device);
    return retval;
}

void alcGetIntegerv(ALCdevice *_device, ALCenum param, ALCsizei size, ALCint *values)
{
    DeviceWrapper *device = _device ? (DeviceWrapper *) _device : &null_device;
    ALsizei i;
    IO_START(alcGetIntegerv);
    IO_PTR(_device);
    IO_ALCENUM(param);
    IO_ALCSIZEI(size);
    IO_PTR(values);

    if (values) {
        memset(values, '\0', size * sizeof (ALCint));
    }

    REAL_alcGetIntegerv(device->device, param, size, values);

    if (values) {
        for (i = 0; i < size; i++) {
            IO_INT32(values[i]);
        }
    }

    IO_END_ALC(device);
}

void alcCaptureStart(ALCdevice *_device)
{
    DeviceWrapper *device = _device ? (DeviceWrapper *) _device : &null_device;
    IO_START(alcCaptureStart);
    IO_PTR(_device);
    REAL_alcCaptureStart(device->device);
    IO_END_ALC(device);
}

void alcCaptureStop(ALCdevice *_device)
{
    DeviceWrapper *device = _device ? (DeviceWrapper *) _device : &null_device;
    IO_START(alcCaptureStop);
    IO_PTR(_device);
    REAL_alcCaptureStop(device->device);
    IO_END_ALC(device);
}

void alcCaptureSamples(ALCdevice *_device, ALCvoid *buffer, ALCsizei samples)
{
    DeviceWrapper *device = _device ? (DeviceWrapper *) _device : &null_device;
    IO_START(alcCaptureSamples);
    IO_PTR(_device);
    IO_ALCSIZEI(samples);
    if (samples && device->samplesize) {
        memset(buffer, '\0', samples * device->samplesize);
    }
    REAL_alcCaptureSamples(device->device, buffer, samples);
    IO_BLOB(buffer, samples * device->samplesize);
    IO_END_ALC(device);
}

void alDopplerFactor(ALfloat value)
{
    IO_START(alDopplerFactor);
    IO_FLOAT(value);
    REAL_alDopplerFactor(value);
    IO_END();
}

void alDopplerVelocity(ALfloat value)
{
    IO_START(alDopplerVelocity);
    IO_FLOAT(value);
    REAL_alDopplerVelocity(value);
    IO_END();
}

void alSpeedOfSound(ALfloat value)
{
    IO_START(alSpeedOfSound);
    IO_FLOAT(value);
    REAL_alSpeedOfSound(value);
    IO_END();
}

void alDistanceModel(ALenum model)
{
    IO_START(alDistanceModel);
    IO_ENUM(model);
    REAL_alDistanceModel(model);
    IO_END();
}

void alEnable(ALenum capability)
{
    IO_START(alEnable);
    IO_ENUM(capability);
    REAL_alEnable(capability);
    IO_END();
}

void alDisable(ALenum capability)
{
    IO_START(alDisable);
    IO_ENUM(capability);
    REAL_alDisable(capability);
    IO_END();
}

ALboolean alIsEnabled(ALenum capability)
{
    ALboolean retval;
    IO_START(alIsEnabled);
    IO_ENUM(capability);
    retval = REAL_alIsEnabled(capability);
    IO_BOOLEAN(retval);
    IO_END();
    return retval;
}

const ALchar *alGetString(const ALenum param)
{
    const ALchar *retval;
    IO_START(alGetString);
    IO_ENUM(param);
    retval = REAL_alGetString(param);

    if (param == AL_EXTENSIONS) {
        if (retval && current_context) {
            const char *addstr = "AL_EXT_trace_info";
            const size_t slen = strlen(retval) + strlen(addstr) + 2;
            char *ptr = (char *) realloc(current_context->extension_string, slen);
            if (ptr) {
                current_context->extension_string = ptr;
                snprintf(ptr, slen, "%s%s%s", retval, *retval ? " " : "", addstr);
                retval = (const ALCchar *) ptr;
            }
        }
    }

    IO_STRING(retval);
    IO_END();
    return retval;
}

void alGetBooleanv(ALenum param, ALboolean *values)
{
    ALsizei numvals = 0;
    ALsizei i;
    IO_START(alGetBooleanv);
    IO_ENUM(param);
    IO_PTR(values);

    /* nothing in AL 1.1 uses this. */
    if (!values) { numvals = 0; }
    if (numvals) {
        memset(values, '\0', numvals * sizeof (ALboolean));
    }
    REAL_alGetBooleanv(param, values);
    IO_ALSIZEI(numvals);
    for (i = 0; i < numvals; i++) {
        IO_BOOLEAN(values[i]);
    }
    IO_END();
}

void alGetIntegerv(ALenum param, ALint *values)
{
    ALsizei numvals = 0;
    ALsizei i;
    IO_START(alGetIntegerv);
    IO_ENUM(param);
    IO_PTR(values);

    switch (param) {
        case AL_DISTANCE_MODEL: numvals = 1;
        default: break;
    }
    if (!values) { numvals = 0; }
    if (numvals) {
        memset(values, '\0', numvals * sizeof (ALint));
    }
    REAL_alGetIntegerv(param, values);
    IO_ALSIZEI(numvals);
    for (i = 0; i < numvals; i++) {
        IO_INT32(values[i]);
    }
    IO_END();
}

void alGetFloatv(ALenum param, ALfloat *values)
{
    ALsizei numvals = 0;
    ALsizei i;
    IO_START(alGetFloatv);
    IO_ENUM(param);
    IO_PTR(values);

    switch (param) {
        case AL_DOPPLER_FACTOR: numvals = 1;  break;
        case AL_DOPPLER_VELOCITY: numvals = 1; break;
        case AL_SPEED_OF_SOUND: numvals = 1; break;
        default: break;
    }
    if (!values) { numvals = 0; }
    if (numvals) {
        memset(values, '\0', numvals * sizeof (ALfloat));
    }
    REAL_alGetFloatv(param, values);
    IO_ALSIZEI(numvals);
    for (i = 0; i < numvals; i++) {
        IO_FLOAT(values[i]);
    }
    IO_END();
}

void alGetDoublev(ALenum param, ALdouble *values)
{
    ALsizei numvals = 0;
    ALsizei i;
    IO_START(alGetDoublev);
    IO_ENUM(param);
    IO_PTR(values);
    // nothing in AL 1.1 uses this.
    if (!values) { numvals = 0; }
    if (numvals) {
        memset(values, '\0', numvals * sizeof (ALdouble));
    }
    REAL_alGetDoublev(param, values);
    IO_ALSIZEI(numvals);
    for (i = 0; i < numvals; i++) {
        IO_DOUBLE(values[i]);
    }
    IO_END();
}

ALboolean alGetBoolean(ALenum param)
{
    ALboolean retval;
    IO_START(alGetBoolean);
    IO_ENUM(param);
    retval = REAL_alGetBoolean(param);
    IO_BOOLEAN(retval);
    IO_END();
    return retval;
}

ALint alGetInteger(ALenum param)
{
    ALint retval;
    IO_START(alGetInteger);
    IO_ENUM(param);
    retval = REAL_alGetInteger(param);
    IO_INT32(retval);
    IO_END();
    return retval;
}

ALfloat alGetFloat(ALenum param)
{
    ALfloat retval;
    IO_START(alGetFloat);
    IO_ENUM(param);
    retval = REAL_alGetFloat(param);
    IO_FLOAT(retval);
    IO_END();
    return retval;
}

ALdouble alGetDouble(ALenum param)
{
    ALdouble retval;
    IO_START(alGetDouble);
    IO_ENUM(param);
    retval = REAL_alGetDouble(param);
    IO_DOUBLE(retval);
    IO_END();
    return retval;
}

ALboolean alIsExtensionPresent(const ALchar *extname)
{
    ALboolean retval;
    IO_START(alIsExtensionPresent);
    IO_STRING(extname);
    if (strcasecmp(extname, "AL_EXT_trace_info") == 0) {
        retval = AL_TRUE;
    } else {
        retval = REAL_alIsExtensionPresent(extname);
    }
    IO_BOOLEAN(retval);
    IO_END();
    return retval;
}

ALenum alGetError(void)
{
    ALenum retval;
    IO_START(alGetError);

    if (current_context == NULL) {
        retval = ALC_INVALID_CONTEXT;  //REAL_alGetError(device);
    } else {
        retval = current_context->errorlatch;
        current_context->errorlatch = AL_NO_ERROR;
    }

    IO_ENUM(retval);
    IO_END();
    return retval;
}

void *alGetProcAddress(const ALchar *funcname)
{
    void *retval = NULL;
    IO_START(alGetProcAddress);
    IO_STRING(funcname);

    // always return our entry points, so the app always calls through here.
    if (!funcname || ((funcname[0] != 'a') || (funcname[1] != 'l') || (funcname[2] == 'c'))) {
        // !!! FIXME: should set an error state.
        retval = NULL;
    }
    #define ENTRYPOINT(ret,fn,params,args) else if (strcmp(funcname, #fn) == 0) { retval = (void *) fn; }
    #include "altrace_entrypoints.h"

    IO_PTR(retval);
    IO_END();
    return retval;
}

ALenum alGetEnumValue(const ALchar *enumname)
{
    ALenum retval;
    IO_START(alGetEnumValue);
    IO_STRING(enumname);
    retval = REAL_alGetEnumValue(enumname);
    IO_ENUM(retval);
    IO_END();
    return retval;
}

void alListenerfv(ALenum param, const ALfloat *values)
{
    uint32 numvals = 1;
    uint32 i;
    IO_START(alListenerfv);
    IO_ENUM(param);
    IO_PTR(values);

    switch (param) {
        case AL_GAIN: break;
        case AL_POSITION: numvals = 3; break;
        case AL_VELOCITY: numvals = 3; break;
        case AL_ORIENTATION: numvals = 6; break;
        default: break;   /* uhoh. */
    }

    if (!values) { numvals = 0; }

    IO_UINT32(numvals);
    for (i = 0; i < numvals; i++) {
        IO_FLOAT(values[i]);
    }

    REAL_alListenerfv(param, values);
    IO_END();
}

void alListenerf(ALenum param, ALfloat value)
{
    IO_START(alListenerf);
    IO_ENUM(param);
    IO_FLOAT(value);
    REAL_alListenerf(param, value);
    IO_END();
}

void alListener3f(ALenum param, ALfloat value1, ALfloat value2, ALfloat value3)
{
    IO_START(alListener3f);
    IO_ENUM(param);
    IO_FLOAT(value1);
    IO_FLOAT(value2);
    IO_FLOAT(value3);
    REAL_alListener3f(param, value1, value2, value3);
    IO_END();
}

void alListeneriv(ALenum param, const ALint *values)
{
    uint32 numvals = 1;
    uint32 i;
    IO_START(alListeneriv);
    IO_ENUM(param);
    IO_PTR(values);
    switch (param) {
        case AL_POSITION: numvals = 3; break;
        case AL_VELOCITY: numvals = 3; break;
        case AL_ORIENTATION: numvals = 6; break;
        default: break;   /* uhoh. */
    }

    if (!values) { numvals = 0; }
    IO_UINT32(numvals);
    for (i = 0; i < numvals; i++) {
        IO_INT32(values[i]);
    }

    REAL_alListeneriv(param, values);
    IO_END();
}

void alListeneri(ALenum param, ALint value)
{
    IO_START(alListeneri);
    IO_ENUM(param);
    IO_INT32(value);
    REAL_alListeneri(param, value);
    IO_END();
}

void alListener3i(ALenum param, ALint value1, ALint value2, ALint value3)
{
    IO_START(alListener3i);
    IO_ENUM(param);
    IO_INT32(value1);
    IO_INT32(value2);
    IO_INT32(value3);
    REAL_alListener3i(param, value1, value2, value3);
    IO_END();
}

void alGetListenerfv(ALenum param, ALfloat *values)
{
    uint32 numvals = 1;
    uint32 i;
    IO_START(alGetListenerfv);
    IO_ENUM(param);
    IO_PTR(values);

    switch (param) {
        case AL_POSITION: numvals = 3; break;
        case AL_VELOCITY: numvals = 3; break;
        case AL_ORIENTATION: numvals = 6; break;
        default: break;   /* uhoh. */
    }

    if (!values) { numvals = 0; }
    if (numvals) {
        memset(values, '\0', numvals * sizeof (ALfloat));
    }

    REAL_alGetListenerfv(param, values);

    IO_UINT32(numvals);
    for (i = 0; i < numvals; i++) {
        IO_FLOAT(values[i]);
    }

    IO_END();
}

void alGetListenerf(ALenum param, ALfloat *value)
{
    IO_START(alGetListenerf);
    IO_ENUM(param);
    IO_PTR(value);
    REAL_alGetListenerf(param, value);
    IO_FLOAT(value ? *value : 0.0f);
    IO_END();
}

void alGetListener3f(ALenum param, ALfloat *value1, ALfloat *value2, ALfloat *value3)
{
    IO_START(alGetListener3f);
    IO_ENUM(param);
    IO_PTR(value1);
    IO_PTR(value2);
    IO_PTR(value3);
    REAL_alGetListener3f(param, value1, value2, value3);
    IO_FLOAT(value1 ? *value1 : 0.0f);
    IO_FLOAT(value2 ? *value2 : 0.0f);
    IO_FLOAT(value3 ? *value3 : 0.0f);
    IO_END();
}

void alGetListeneriv(ALenum param, ALint *values)
{
    uint32 numvals = 1;
    uint32 i;
    IO_START(alGetListeneriv);
    IO_ENUM(param);
    IO_PTR(values);

    switch (param) {
        case AL_POSITION: numvals = 3; break;
        case AL_VELOCITY: numvals = 3; break;
        case AL_ORIENTATION: numvals = 6; break;
        default: break;   /* uhoh. */
    }

    if (!values) { numvals = 0; }
    if (numvals) {
        memset(values, '\0', numvals * sizeof (ALdouble));
    }

    REAL_alGetListeneriv(param, values);

    IO_UINT32(numvals);
    for (i = 0; i < numvals; i++) {
        IO_FLOAT(values[i]);
    }

    IO_END();
}

void alGetListeneri(ALenum param, ALint *value)
{
    IO_START(alGetListeneri);
    IO_ENUM(param);
    IO_PTR(value);
    REAL_alGetListeneri(param, value);
    IO_INT32(value ? *value : 0);
    IO_END();
}

void alGetListener3i(ALenum param, ALint *value1, ALint *value2, ALint *value3)
{
    IO_START(alGetListener3i);
    IO_ENUM(param);
    IO_PTR(value1);
    IO_PTR(value2);
    IO_PTR(value3);
    REAL_alGetListener3i(param, value1, value2, value3);
    IO_INT32(value1 ? *value1 : 0);
    IO_INT32(value2 ? *value2 : 0);
    IO_INT32(value3 ? *value3 : 0);
    IO_END();
}

void alGenSources(ALsizei n, ALuint *names)
{
    ALsizei i;
    IO_START(alGenSources);
    IO_ALSIZEI(n);
    memset(names, 0, n * sizeof (ALuint));
    REAL_alGenSources(n, names);
    for (i = 0; i < n; i++) {
        IO_UINT32(names[i]);
    }
    IO_END();
}

void alDeleteSources(ALsizei n, const ALuint *names)
{
    ALsizei i;
    IO_START(alDeleteSources);
    IO_ALSIZEI(n);
    for (i = 0; i < n; i++) {
        IO_UINT32(names[i]);
    }
    REAL_alDeleteSources(n, names);
    IO_END();
}

ALboolean alIsSource(ALuint name)
{
    ALboolean retval;
    IO_START(alIsSource);
    IO_UINT32(name);
    retval = REAL_alIsSource(name);
    IO_BOOLEAN(retval);
    IO_END();
    return retval;
}

void alSourcefv(ALuint name, ALenum param, const ALfloat *values)
{
    uint32 numvals = 1;
    uint32 i;
    IO_START(alSourcefv);
    IO_UINT32(name);
    IO_ENUM(param);
    switch (param) {
        case AL_GAIN: break;
        case AL_POSITION: numvals = 3; break;
        case AL_VELOCITY: numvals = 3; break;
        case AL_DIRECTION: numvals = 3; break;
        case AL_MIN_GAIN: break;
        case AL_MAX_GAIN: break;
        case AL_REFERENCE_DISTANCE: break;
        case AL_ROLLOFF_FACTOR: break;
        case AL_MAX_DISTANCE: break;
        case AL_PITCH: break;
        case AL_CONE_INNER_ANGLE: break;
        case AL_CONE_OUTER_ANGLE: break;
        case AL_CONE_OUTER_GAIN: break;
        default: numvals = 0; break;   /* uhoh. */
    }

    IO_UINT32(numvals);
    for (i = 0; i < numvals; i++) {
        IO_FLOAT(values[i]);
    }

    REAL_alSourcefv(name, param, values);
    IO_END();
}

void alSourcef(ALuint name, ALenum param, ALfloat value)
{
    IO_START(alSourcef);
    IO_UINT32(name);
    IO_ENUM(param);
    IO_FLOAT(value);
    REAL_alSourcef(name, param, value);
    IO_END();
}

void alSource3f(ALuint name, ALenum param, ALfloat value1, ALfloat value2, ALfloat value3)
{
    IO_START(alSource3f);
    IO_UINT32(name);
    IO_ENUM(param);
    IO_FLOAT(value1);
    IO_FLOAT(value2);
    IO_FLOAT(value3);
    REAL_alSource3f(name, param, value1, value2, value3);
    IO_END();
}

void alSourceiv(ALuint name, ALenum param, const ALint *values)
{
    uint32 numvals = 1;
    uint32 i;
    IO_START(alSourceiv);
    IO_UINT32(name);
    IO_ENUM(param);

    switch (param) {
        case AL_BUFFER: break;
        case AL_SOURCE_RELATIVE: break;
        case AL_LOOPING: break;
        case AL_REFERENCE_DISTANCE: break;
        case AL_ROLLOFF_FACTOR: break;
        case AL_MAX_DISTANCE: break;
        case AL_CONE_INNER_ANGLE: break;
        case AL_CONE_OUTER_ANGLE: break;
        case AL_DIRECTION: numvals = 3; break;
        case AL_SEC_OFFSET: break;
        case AL_SAMPLE_OFFSET: break;
        case AL_BYTE_OFFSET: break;
        default: numvals = 0; break;   /* uhoh. */
    }

    IO_UINT32(numvals);
    for (i = 0; i < numvals; i++) {
        IO_INT32(values[i]);
    }

    REAL_alSourceiv(name, param, values);
    IO_END();
}

void alSourcei(ALuint name, ALenum param, ALint value)
{
    IO_START(alSourcei);
    IO_UINT32(name);
    IO_ENUM(param);
    IO_INT32(value);
    REAL_alSourcei(name, param, value);
    IO_END();
}

void alSource3i(ALuint name, ALenum param, ALint value1, ALint value2, ALint value3)
{
    IO_START(alSource3i);
    IO_UINT32(name);
    IO_ENUM(param);
    IO_INT32(value1);
    IO_INT32(value2);
    IO_INT32(value3);
    REAL_alSource3i(name, param, value1, value2, value3);
    IO_END();
}

void alGetSourcefv(ALuint name, ALenum param, ALfloat *values)
{
    uint32 numvals = 1;
    uint32 i;
    IO_START(alGetSourcefv);
    IO_UINT32(name);
    IO_ENUM(param);
    IO_PTR(values);

    switch (param) {
        case AL_GAIN: break;
        case AL_POSITION: numvals = 3; break;
        case AL_VELOCITY: numvals = 3; break;
        case AL_DIRECTION: numvals = 3; break;
        case AL_MIN_GAIN: break;
        case AL_MAX_GAIN: break;
        case AL_REFERENCE_DISTANCE: break;
        case AL_ROLLOFF_FACTOR: break;
        case AL_MAX_DISTANCE: break;
        case AL_PITCH: break;
        case AL_CONE_INNER_ANGLE: break;
        case AL_CONE_OUTER_ANGLE: break;
        case AL_CONE_OUTER_GAIN: break;
        case AL_SEC_OFFSET: break;
        case AL_SAMPLE_OFFSET: break;
        case AL_BYTE_OFFSET: break;
        default: numvals = 0; break;   /* uhoh. */
    }

    if (!values) { numvals = 0; }
    if (numvals) {
        memset(values, '\0', numvals * sizeof (ALfloat));
    }

    REAL_alGetSourcefv(name, param, values);

    IO_UINT32(numvals);
    for (i = 0; i < numvals; i++) {
        IO_FLOAT(values[i]);
    }

    IO_END();
}

void alGetSourcef(ALuint name, ALenum param, ALfloat *value)
{
    IO_START(alGetSourcef);
    IO_UINT32(name);
    IO_ENUM(param);
    IO_PTR(value);
    REAL_alGetSourcef(name, param, value);
    IO_FLOAT(value ? *value : 0.0f);
    IO_END();
}

void alGetSource3f(ALuint name, ALenum param, ALfloat *value1, ALfloat *value2, ALfloat *value3)
{
    IO_START(alGetSource3f);
    IO_UINT32(name);
    IO_ENUM(param);
    IO_PTR(value1);
    IO_PTR(value2);
    IO_PTR(value3);
    REAL_alGetSource3f(name, param, value1, value2, value3);
    IO_FLOAT(value1 ? *value1 : 0.0f);
    IO_FLOAT(value2 ? *value2 : 0.0f);
    IO_FLOAT(value3 ? *value3 : 0.0f);
    IO_END();
}

void alGetSourceiv(ALuint name, ALenum param, ALint *values)
{
    uint32 numvals = 1;
    uint32 i;
    IO_START(alGetSourceiv);
    IO_UINT32(name);
    IO_ENUM(param);
    IO_PTR(values);

    switch (param) {
        case AL_SOURCE_STATE: break;
        case AL_SOURCE_RELATIVE: break;
        case AL_LOOPING: break;
        case AL_BUFFER: break;
        case AL_BUFFERS_QUEUED: break;
        case AL_BUFFERS_PROCESSED: break;
        case AL_SOURCE_TYPE: break;
        case AL_REFERENCE_DISTANCE: break;
        case AL_ROLLOFF_FACTOR: break;
        case AL_MAX_DISTANCE: break;
        case AL_CONE_INNER_ANGLE: break;
        case AL_CONE_OUTER_ANGLE: break;
        case AL_SEC_OFFSET: break;
        case AL_SAMPLE_OFFSET: break;
        case AL_BYTE_OFFSET: break;
        default: numvals = 0; break;   /* uhoh. */
    }

    if (!values) { numvals = 0; }
    if (numvals) {
        memset(values, '\0', numvals * sizeof (ALint));
    }

    REAL_alGetSourceiv(name, param, values);

    IO_UINT32(numvals);
    for (i = 0; i < numvals; i++) {
        IO_INT32(values[i]);
    }

    IO_END();
}

void alGetSourcei(ALuint name, ALenum param, ALint *value)
{
    IO_START(alGetSourcei);
    IO_UINT32(name);
    IO_ENUM(param);
    IO_PTR(value);
    REAL_alGetSourcei(name, param, value);
    IO_INT32(value ? *value : 0);
    IO_END();
}

void alGetSource3i(ALuint name, ALenum param, ALint *value1, ALint *value2, ALint *value3)
{
    IO_START(alGetSource3i);
    IO_UINT32(name);
    IO_ENUM(param);
    IO_PTR(value1);
    IO_PTR(value2);
    IO_PTR(value3);
    REAL_alGetSource3i(name, param, value1, value2, value3);
    IO_INT32(value1 ? *value1 : 0);
    IO_INT32(value2 ? *value2 : 0);
    IO_INT32(value3 ? *value3 : 0);
    IO_END();
}

void alSourcePlay(ALuint name)
{
    IO_START(alSourcePlay);
    IO_UINT32(name);
    REAL_alSourcePlay(name);
    IO_END();
}

void alSourcePlayv(ALsizei n, const ALuint *names)
{
    ALsizei i;
    IO_START(alSourcePlayv);
    IO_ALSIZEI(n);
    for (i = 0; i < n; i++) {
        IO_UINT32(names[i]);
    }
    REAL_alSourcePlayv(n, names);
    IO_END();
}

void alSourcePause(ALuint name)
{
    IO_START(alSourcePause);
    IO_UINT32(name);
    REAL_alSourcePause(name);
    IO_END();
}

void alSourcePausev(ALsizei n, const ALuint *names)
{
    ALsizei i;
    IO_START(alSourcePausev);
    IO_ALSIZEI(n);
    for (i = 0; i < n; i++) {
        IO_UINT32(names[i]);
    }
    REAL_alSourcePausev(n, names);
    IO_END();
}

void alSourceRewind(ALuint name)
{
    IO_START(alSourceRewind);
    IO_UINT32(name);
    REAL_alSourceRewind(name);
    IO_END();
}

void alSourceRewindv(ALsizei n, const ALuint *names)
{
    ALsizei i;
    IO_START(alSourceRewindv);
    IO_ALSIZEI(n);
    for (i = 0; i < n; i++) {
        IO_UINT32(names[i]);
    }
    REAL_alSourceRewindv(n, names);
    IO_END();
}

void alSourceStop(ALuint name)
{
    IO_START(alSourceStop);
    IO_UINT32(name);
    REAL_alSourceStop(name);
    IO_END();
}

void alSourceStopv(ALsizei n, const ALuint *names)
{
    ALsizei i;
    IO_START(alSourceStopv);
    IO_ALSIZEI(n);
    for (i = 0; i < n; i++) {
        IO_UINT32(names[i]);
    }
    REAL_alSourceStopv(n, names);
    IO_END();
}

void alSourceQueueBuffers(ALuint name, ALsizei nb, const ALuint *bufnames)
{
    ALsizei i;
    IO_START(alSourceQueueBuffers);
    IO_UINT32(name);
    IO_ALSIZEI(nb);
    for (i = 0; i < nb; i++) {
        IO_UINT32(bufnames[i]);
    }
    REAL_alSourceQueueBuffers(name, nb, bufnames);
    IO_END();
}

void alSourceUnqueueBuffers(ALuint name, ALsizei nb, ALuint *bufnames)
{
    ALsizei i;
    IO_START(alSourceUnqueueBuffers);
    IO_UINT32(name);
    IO_ALSIZEI(nb);
    memset(bufnames, 0, nb * sizeof (ALuint));
    REAL_alSourceUnqueueBuffers(name, nb, bufnames);
    for (i = 0; i < nb; i++) {
        IO_UINT32(bufnames[i]);
    }
    IO_END();
}

void alGenBuffers(ALsizei n, ALuint *names)
{
    ALsizei i;
    IO_START(alGenBuffers);
    IO_ALSIZEI(n);
    memset(names, 0, n * sizeof (ALuint));
    REAL_alGenBuffers(n, names);
    for (i = 0; i < n; i++) {
        IO_UINT32(names[i]);
    }
    IO_END();
}

void alDeleteBuffers(ALsizei n, const ALuint *names)
{
    ALsizei i;
    IO_START(alDeleteSources);
    IO_ALSIZEI(n);
    for (i = 0; i < n; i++) {
        IO_UINT32(names[i]);
    }
    REAL_alDeleteBuffers(n, names);
    IO_END();
}

ALboolean alIsBuffer(ALuint name)
{
    ALboolean retval;
    IO_START(alIsBuffer);
    IO_UINT32(name);
    retval = REAL_alIsBuffer(name);
    IO_BOOLEAN(retval);
    IO_END();
    return retval;
}

void alBufferData(ALuint name, ALenum alfmt, const ALvoid *data, ALsizei size, ALsizei freq)
{
    IO_START(alBufferData);
    IO_UINT32(name);
    IO_ENUM(alfmt);
    IO_ALSIZEI(freq);
    IO_BLOB(data, size);
    REAL_alBufferData(name, alfmt, data, size, freq);
    IO_END();
}

void alBufferfv(ALuint name, ALenum param, const ALfloat *values)
{
    uint32 numvals = 1;
    uint32 i;
    IO_START(alBufferfv);
    IO_UINT32(name);
    IO_ENUM(param);
    /* nothing uses this at the moment. */
    IO_UINT32(numvals);
    for (i = 0; i < numvals; i++) {
        IO_FLOAT(values[i]);
    }
    REAL_alBufferfv(name, param, values);
    IO_END();
}

void alBufferf(ALuint name, ALenum param, ALfloat value)
{
    IO_START(alBufferf);
    IO_UINT32(name);
    IO_ENUM(param);
    IO_FLOAT(value);
    REAL_alBufferf(name, param, value);
    IO_END();
}

void alBuffer3f(ALuint name, ALenum param, ALfloat value1, ALfloat value2, ALfloat value3)
{
    IO_START(alBuffer3f);
    IO_UINT32(name);
    IO_ENUM(param);
    IO_FLOAT(value1);
    IO_FLOAT(value2);
    IO_FLOAT(value3);
    REAL_alBuffer3f(name, param, value1, value2, value3);
    IO_END();
}

void alBufferiv(ALuint name, ALenum param, const ALint *values)
{
    uint32 numvals = 1;
    uint32 i;
    IO_START(alBufferiv);
    IO_UINT32(name);
    IO_ENUM(param);
    /* nothing uses this at the moment. */
    IO_UINT32(numvals);
    for (i = 0; i < numvals; i++) {
        IO_INT32(values[i]);
    }
    REAL_alBufferiv(name, param, values);
    IO_END();
}

void alBufferi(ALuint name, ALenum param, ALint value)
{
    IO_START(alBufferi);
    IO_UINT32(name);
    IO_ENUM(param);
    IO_INT32(value);
    REAL_alBufferi(name, param, value);
    IO_END();
}

void alBuffer3i(ALuint name, ALenum param, ALint value1, ALint value2, ALint value3)
{
    IO_START(alBuffer3i);
    IO_UINT32(name);
    IO_ENUM(param);
    IO_INT32(value1);
    IO_INT32(value2);
    IO_INT32(value3);
    REAL_alBuffer3i(name, param, value1, value2, value3);
    IO_END();
}

void alGetBufferfv(ALuint name, ALenum param, ALfloat *values)
{
    uint32 numvals = 1;
    uint32 i;
    IO_START(alGetBufferfv);
    IO_UINT32(name);
    IO_ENUM(param);
    IO_PTR(values);

    // nothing uses this in AL 1.1

    if (!values) { numvals = 0; }
    if (numvals) {
        memset(values, '\0', numvals * sizeof (ALfloat));
    }

    REAL_alGetBufferfv(name, param, values);

    IO_UINT32(numvals);
    for (i = 0; i < numvals; i++) {
        IO_FLOAT(values[i]);
    }

    IO_END();
}

void alGetBufferf(ALuint name, ALenum param, ALfloat *value)
{
    IO_START(alGetBufferf);
    IO_UINT32(name);
    IO_ENUM(param);
    IO_PTR(value);
    REAL_alGetBufferf(name, param, value);
    IO_FLOAT(value ? *value : 0.0f);
    IO_END();
}

void alGetBuffer3f(ALuint name, ALenum param, ALfloat *value1, ALfloat *value2, ALfloat *value3)
{
    IO_START(alGetBuffer3f);
    IO_UINT32(name);
    IO_ENUM(param);
    IO_PTR(value1);
    IO_PTR(value2);
    IO_PTR(value3);
    REAL_alGetBuffer3f(name, param, value1, value2, value3);
    IO_FLOAT(value1 ? *value1 : 0.0f);
    IO_FLOAT(value2 ? *value2 : 0.0f);
    IO_FLOAT(value3 ? *value3 : 0.0f);
    IO_END();
}

void alGetBufferi(ALuint name, ALenum param, ALint *value)
{
    IO_START(alGetBufferi);
    IO_UINT32(name);
    IO_ENUM(param);
    IO_PTR(value);
    REAL_alGetBufferi(name, param, value);
    IO_INT32(value ? *value : 0);
    IO_END();
}

void alGetBuffer3i(ALuint name, ALenum param, ALint *value1, ALint *value2, ALint *value3)
{
    IO_START(alGetBuffer3i);
    IO_UINT32(name);
    IO_ENUM(param);
    IO_PTR(value1);
    IO_PTR(value2);
    IO_PTR(value3);
    REAL_alGetBuffer3i(name, param, value1, value2, value3);
    IO_INT32(value1 ? *value1 : 0);
    IO_INT32(value2 ? *value2 : 0);
    IO_INT32(value3 ? *value3 : 0);
    IO_END();
}

void alGetBufferiv(ALuint name, ALenum param, ALint *values)
{
    uint32 numvals = 1;
    uint32 i;
    IO_START(alGetBufferiv);
    IO_UINT32(name);
    IO_ENUM(param);
    IO_PTR(values);

    switch (param) {
        case AL_FREQUENCY: break;
        case AL_SIZE: break;
        case AL_BITS: break;
        case AL_CHANNELS:  break;
        default: numvals = 0; break;
    }

    if (!values) { numvals = 0; }
    if (numvals) {
        memset(values, '\0', numvals * sizeof (ALint));
    }

    REAL_alGetBufferiv(name, param, values);

    IO_UINT32(numvals);
    for (i = 0; i < numvals; i++) {
        IO_INT32(values[i]);
    }

    IO_END();
}

void alTracePushScope(const ALchar *str)
{
    IO_START(alTracePushScope);
    IO_STRING(str);
    IO_END();
}

void alTracePopScope(void)
{
    IO_START(alTracePopScope);
    IO_END();
}

void alTraceMessage(const ALchar *str)
{
    IO_START(alTraceMessage);
    IO_STRING(str);
    IO_END();
}

void alTraceBufferLabel(ALuint name, const ALchar *str)
{
    IO_START(alTraceBufferLabel);
    IO_UINT32(name);
    IO_STRING(str);
    IO_END();
}

void alTraceSourceLabel(ALuint name, const ALchar *str)
{
    IO_START(alTraceSourceLabel);
    IO_UINT32(name);
    IO_STRING(str);
    IO_END();
}

void alcTraceDeviceLabel(ALCdevice *_device, const ALchar *str)
{
    IO_START(alcTraceDeviceLabel);
    IO_PTR(_device);
    IO_STRING(str);
    IO_END();
}

void alcTraceContextLabel(ALCcontext *_ctx, const ALchar *str)
{
    IO_START(alcTraceContextLabel);
    IO_PTR(_ctx);
    IO_STRING(str);
    IO_END();
}

// end of altrace_record.c ...

