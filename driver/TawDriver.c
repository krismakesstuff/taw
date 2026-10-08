// taw.driver: the hub's virtual devices (docs/plan-audio-hub.md, step 3). A CoreAudio
// AudioServerPlugIn, loaded by coreaudiod from /Library/Audio/Plug-Ins/HAL.
//
// Four devices any app can pick, "taw 1" to "taw 4", each stereo both ways: what an app plays
// into one (its speaker side) the hub hears, and what the hub sends it an app records (its mic
// side). The hub reaches all of them through one hidden device, "taw hub", with 8 channels each
// way: its inputs 2N-1, 2N are what apps play into taw N; its outputs 2N-1, 2N go to taw N's mic.
//
// Inside it's plain loopback through ring buffers, one each way per device, indexed by sample
// time. Every device runs on the same clock (one anchor, one period), so sample time t means
// the same instant on all of them: a writer writes the frames it will play at t, the reader
// reads the frames recorded at t, and reading clears them so a writer that stops leaves
// silence, not a loop. coreaudiod runs this sandboxed as its own user: it talks to nothing.

#include <CoreAudio/AudioServerPlugIn.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

//==============================================================================
// The objects.

enum
{
    kNumApps = 4,                  // taw 1 .. taw 4
    kHubDevice = kNumApps,         // the hidden one
    kNumDevices = kNumApps + 1,
    kAppChannels = 2,
    kHubChannels = 2 * kNumApps,
    kRingFrames = 16384,           // also the zero time stamp period
    kClockDomain = 0x74617763      // 'tawc': every device here is on the one clock
};

// Object IDs: the plug-in is 1; device d is 2 + 3d, its input stream 3 + 3d, its output 4 + 3d.
static AudioObjectID deviceID (int d) { return (AudioObjectID) (2 + 3 * d); }
static AudioObjectID inputStreamID (int d) { return (AudioObjectID) (3 + 3 * d); }
static AudioObjectID outputStreamID (int d) { return (AudioObjectID) (4 + 3 * d); }

typedef enum { kNone, kPlugIn, kDevice, kInputStream, kOutputStream } Kind;

static Kind kindOf (AudioObjectID object, int* device)
{
    if (object == kAudioObjectPlugInObject)
        return kPlugIn;

    if (object >= 2 && object < 2 + 3 * kNumDevices)
    {
        *device = (int) (object - 2) / 3;
        return (Kind) (kDevice + (int) (object - 2) % 3);
    }

    return kNone;
}

static UInt32 channelsOf (int d) { return d == kHubDevice ? kHubChannels : kAppChannels; }

//==============================================================================
// State.

static AudioServerPlugInHostRef gHost = NULL;
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static Float64 gSampleRate = 48000.0;
static Float64 gPendingRate = 48000.0;
static UInt64 gAnchorHostTime = 0;
static Float64 gHostTicksPerFrame = 0.0;
static UInt32 gIOCount[kNumDevices];
static UInt32 gRefCount = 0;

// The low ones are Bluetooth headsets with their mic open (16 kHz is the usual).
static const Float64 kRates[] = { 8000.0, 16000.0, 22050.0, 24000.0, 32000.0, 44100.0, 48000.0, 88200.0, 96000.0 };
enum { kNumRates = sizeof (kRates) / sizeof (kRates[0]) };

// A stereo ring buffer: frames by sample time, and up to when they've been written.
typedef struct
{
    float data[kRingFrames * 2];
    _Atomic SInt64 writtenEnd;
} Ring;

static Ring gPlay[kNumApps]; // what apps play into taw N, for the hub
static Ring gMic[kNumApps];  // what the hub sends taw N, for apps

static void setClock (Float64 rate)
{
    mach_timebase_info_data_t timebase;
    mach_timebase_info (&timebase);
    const Float64 hostTicksPerSecond = 1.0e9 * (Float64) timebase.denom / (Float64) timebase.numer;

    gSampleRate = rate;
    gHostTicksPerFrame = hostTicksPerSecond / rate;
    gAnchorHostTime = mach_absolute_time();

    for (int n = 0; n < kNumApps; ++n)
    {
        memset (gPlay[n].data, 0, sizeof (gPlay[n].data));
        memset (gMic[n].data, 0, sizeof (gMic[n].data));
        atomic_store (&gPlay[n].writtenEnd, 0);
        atomic_store (&gMic[n].writtenEnd, 0);
    }
}

// Frames [t, t + frames) from src's channels first, first + 1 (of srcChannels interleaved).
static void ringWrite (Ring* r, SInt64 t, const float* src, UInt32 frames, UInt32 srcChannels, UInt32 first)
{
    for (UInt32 i = 0; i < frames; ++i)
    {
        const SInt64 pos = ((t + (SInt64) i) % kRingFrames + kRingFrames) % kRingFrames;
        r->data[pos * 2] = src[i * srcChannels + first];
        r->data[pos * 2 + 1] = src[i * srcChannels + first + 1];
    }

    const SInt64 end = t + (SInt64) frames;

    if (end > atomic_load (&r->writtenEnd))
        atomic_store (&r->writtenEnd, end);
}

// Frames [t, t + frames) into dst's channels first, first + 1, cleared once read. Frames not
// written (or written a lap ago) are silence.
static void ringRead (Ring* r, SInt64 t, float* dst, UInt32 frames, UInt32 dstChannels, UInt32 first)
{
    const SInt64 end = atomic_load (&r->writtenEnd);

    for (UInt32 i = 0; i < frames; ++i)
    {
        const SInt64 at = t + (SInt64) i;
        float* out = dst + i * dstChannels + first;

        if (at < end && at >= end - kRingFrames)
        {
            const SInt64 pos = (at % kRingFrames + kRingFrames) % kRingFrames;
            out[0] = r->data[pos * 2];
            out[1] = r->data[pos * 2 + 1];
            r->data[pos * 2] = r->data[pos * 2 + 1] = 0.0f;
        }
        else
        {
            out[0] = out[1] = 0.0f;
        }
    }
}

//==============================================================================
// Helpers for properties.

#define REQUIRE_SIZE(type)                       \
    if (inDataSize < sizeof (type))              \
        return kAudioHardwareBadPropertySizeError; \
    *outDataSize = sizeof (type)

static CFStringRef deviceName (int d)
{
    static const char* names[kNumDevices] = { "taw 1", "taw 2", "taw 3", "taw 4", "taw hub" };
    return CFStringCreateWithCString (NULL, names[d], kCFStringEncodingUTF8);
}

static CFStringRef deviceUID (int d)
{
    static const char* uids[kNumDevices] = { "com.taw.driver.1", "com.taw.driver.2", "com.taw.driver.3", "com.taw.driver.4", "com.taw.driver.hub" };
    return CFStringCreateWithCString (NULL, uids[d], kCFStringEncodingUTF8);
}

static AudioStreamBasicDescription formatAt (Float64 rate, UInt32 channels)
{
    AudioStreamBasicDescription f;
    memset (&f, 0, sizeof (f));
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = 4 * channels;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = 4 * channels;
    f.mChannelsPerFrame = channels;
    f.mBitsPerChannel = 32;
    return f;
}

// Every device follows a rate change: the clock is shared.
static OSStatus requestRate (Float64 rate)
{
    bool supported = false;

    for (int i = 0; i < kNumRates; ++i)
        supported = supported || kRates[i] == rate;

    if (! supported)
        return kAudioHardwareIllegalOperationError;

    if (rate == gSampleRate)
        return kAudioHardwareNoError;

    pthread_mutex_lock (&gLock);
    gPendingRate = rate;
    pthread_mutex_unlock (&gLock);

    for (int d = 0; d < kNumDevices; ++d)
        gHost->RequestDeviceConfigurationChange (gHost, deviceID (d), 0, NULL);

    return kAudioHardwareNoError;
}

//==============================================================================
// The interface.

static HRESULT QueryInterface (void* inDriver, REFIID inUUID, LPVOID* outInterface);
static ULONG AddRef (void* inDriver);
static ULONG Release (void* inDriver);
static OSStatus Initialize (AudioServerPlugInDriverRef, AudioServerPlugInHostRef);
static OSStatus CreateDevice (AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo*, AudioObjectID*);
static OSStatus DestroyDevice (AudioServerPlugInDriverRef, AudioObjectID);
static OSStatus AddDeviceClient (AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*);
static OSStatus RemoveDeviceClient (AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*);
static OSStatus PerformDeviceConfigurationChange (AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*);
static OSStatus AbortDeviceConfigurationChange (AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*);
static Boolean HasProperty (AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*);
static OSStatus IsPropertySettable (AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, Boolean*);
static OSStatus GetPropertyDataSize (AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32*);
static OSStatus GetPropertyData (AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, UInt32*, void*);
static OSStatus SetPropertyData (AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, const void*);
static OSStatus StartIO (AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus StopIO (AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus GetZeroTimeStamp (AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64*, UInt64*, UInt64*);
static OSStatus WillDoIOOperation (AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean*, Boolean*);
static OSStatus BeginIOOperation (AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*);
static OSStatus DoIOOperation (AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*, void*, void*);
static OSStatus EndIOOperation (AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*);

static AudioServerPlugInDriverInterface gInterface = {
    NULL,
    QueryInterface, AddRef, Release,
    Initialize, CreateDevice, DestroyDevice, AddDeviceClient, RemoveDeviceClient,
    PerformDeviceConfigurationChange, AbortDeviceConfigurationChange,
    HasProperty, IsPropertySettable, GetPropertyDataSize, GetPropertyData, SetPropertyData,
    StartIO, StopIO, GetZeroTimeStamp, WillDoIOOperation, BeginIOOperation, DoIOOperation, EndIOOperation
};

static AudioServerPlugInDriverInterface* gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef gDriver = &gInterfacePtr;

// The factory named in Info.plist.
void* TawDriver_Create (CFAllocatorRef allocator, CFUUIDRef requestedTypeUUID);

void* TawDriver_Create (CFAllocatorRef allocator, CFUUIDRef requestedTypeUUID)
{
    (void) allocator;
    return CFEqual (requestedTypeUUID, kAudioServerPlugInTypeUUID) ? gDriver : NULL;
}

static HRESULT QueryInterface (void* inDriver, REFIID inUUID, LPVOID* outInterface)
{
    if (inDriver != gDriver || outInterface == NULL)
        return kAudioHardwareBadObjectError;

    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes (NULL, inUUID);
    const bool ok = CFEqual (requested, IUnknownUUID) || CFEqual (requested, kAudioServerPlugInDriverInterfaceUUID);
    CFRelease (requested);

    if (! ok)
        return E_NOINTERFACE;

    pthread_mutex_lock (&gLock);
    ++gRefCount;
    pthread_mutex_unlock (&gLock);
    *outInterface = gDriver;
    return S_OK;
}

static ULONG AddRef (void* inDriver)
{
    if (inDriver != gDriver)
        return 0;

    pthread_mutex_lock (&gLock);
    const ULONG count = ++gRefCount;
    pthread_mutex_unlock (&gLock);
    return count;
}

static ULONG Release (void* inDriver)
{
    if (inDriver != gDriver)
        return 0;

    pthread_mutex_lock (&gLock);
    const ULONG count = gRefCount > 0 ? --gRefCount : 0;
    pthread_mutex_unlock (&gLock);
    return count;
}

static OSStatus Initialize (AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost)
{
    if (inDriver != gDriver)
        return kAudioHardwareBadObjectError;

    gHost = inHost;
    setClock (gSampleRate);
    return kAudioHardwareNoError;
}

static OSStatus CreateDevice (AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo* client, AudioObjectID* out)
{
    (void) d; (void) desc; (void) client; (void) out;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus DestroyDevice (AudioServerPlugInDriverRef d, AudioObjectID device)
{
    (void) d; (void) device;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus AddDeviceClient (AudioServerPlugInDriverRef d, AudioObjectID device, const AudioServerPlugInClientInfo* client)
{
    (void) d; (void) device; (void) client;
    return kAudioHardwareNoError;
}

static OSStatus RemoveDeviceClient (AudioServerPlugInDriverRef d, AudioObjectID device, const AudioServerPlugInClientInfo* client)
{
    (void) d; (void) device; (void) client;
    return kAudioHardwareNoError;
}

static OSStatus PerformDeviceConfigurationChange (AudioServerPlugInDriverRef d, AudioObjectID device, UInt64 action, void* info)
{
    (void) d; (void) device; (void) action; (void) info;
    pthread_mutex_lock (&gLock);

    if (gPendingRate != gSampleRate)
        setClock (gPendingRate); // the first device to change moves the shared clock

    pthread_mutex_unlock (&gLock);
    return kAudioHardwareNoError;
}

static OSStatus AbortDeviceConfigurationChange (AudioServerPlugInDriverRef d, AudioObjectID device, UInt64 action, void* info)
{
    (void) d; (void) device; (void) action; (void) info;
    return kAudioHardwareNoError;
}

//==============================================================================
// Properties.

static Boolean HasProperty (AudioServerPlugInDriverRef inDriver, AudioObjectID object, pid_t pid, const AudioObjectPropertyAddress* address)
{
    UInt32 size = 0;
    return inDriver == gDriver && GetPropertyDataSize (inDriver, object, pid, address, 0, NULL, &size) == kAudioHardwareNoError;
}

static OSStatus IsPropertySettable (AudioServerPlugInDriverRef inDriver, AudioObjectID object, pid_t pid, const AudioObjectPropertyAddress* address, Boolean* outSettable)
{
    if (! HasProperty (inDriver, object, pid, address))
        return kAudioHardwareUnknownPropertyError;

    int d = 0;
    const Kind kind = kindOf (object, &d);
    *outSettable = (kind == kDevice && address->mSelector == kAudioDevicePropertyNominalSampleRate)
                   || ((kind == kInputStream || kind == kOutputStream)
                       && (address->mSelector == kAudioStreamPropertyVirtualFormat || address->mSelector == kAudioStreamPropertyPhysicalFormat));
    return kAudioHardwareNoError;
}

static OSStatus GetPropertyDataSize (AudioServerPlugInDriverRef inDriver, AudioObjectID object, pid_t pid, const AudioObjectPropertyAddress* address,
                                     UInt32 qualifierSize, const void* qualifier, UInt32* outSize)
{
    // Sized by asking for the data into a buffer big enough for any of it.
    union
    {
        char bytes[1024];
        AudioStreamRangedDescription ranges[kNumRates];
        AudioObjectID ids[3 * kNumDevices + 2];
    } scratch;

    const OSStatus status = GetPropertyData (inDriver, object, pid, address, qualifierSize, qualifier, sizeof (scratch), outSize, &scratch);

    // Names and UIDs are made for the caller to release: here that's us.
    int d = 0;
    const Kind kind = kindOf (object, &d);

    if (status == kAudioHardwareNoError && kind == kDevice
        && (address->mSelector == kAudioObjectPropertyName || address->mSelector == kAudioDevicePropertyDeviceUID))
        CFRelease (*(CFStringRef*) scratch.bytes);

    return status;
}

static OSStatus objectList (const AudioObjectID* ids, UInt32 count, UInt32 inDataSize, UInt32* outDataSize, void* outData)
{
    const UInt32 fit = (UInt32) (inDataSize / sizeof (AudioObjectID));
    const UInt32 n = count < fit ? count : fit;
    memcpy (outData, ids, n * sizeof (AudioObjectID));
    *outDataSize = n * sizeof (AudioObjectID);
    return kAudioHardwareNoError;
}

static OSStatus GetPropertyData (AudioServerPlugInDriverRef inDriver, AudioObjectID object, pid_t pid, const AudioObjectPropertyAddress* address,
                                 UInt32 qualifierSize, const void* qualifier, UInt32 inDataSize, UInt32* outDataSize, void* outData)
{
    (void) pid;

    if (inDriver != gDriver)
        return kAudioHardwareBadObjectError;

    int d = 0;
    const Kind kind = kindOf (object, &d);
    const AudioObjectPropertySelector s = address->mSelector;

    switch (kind)
    {
        case kPlugIn:
            switch (s)
            {
                case kAudioObjectPropertyBaseClass:
                    REQUIRE_SIZE (AudioClassID);
                    *(AudioClassID*) outData = kAudioObjectClassID;
                    return kAudioHardwareNoError;
                case kAudioObjectPropertyClass:
                    REQUIRE_SIZE (AudioClassID);
                    *(AudioClassID*) outData = kAudioPlugInClassID;
                    return kAudioHardwareNoError;
                case kAudioObjectPropertyOwner:
                    REQUIRE_SIZE (AudioObjectID);
                    *(AudioObjectID*) outData = kAudioObjectUnknown;
                    return kAudioHardwareNoError;
                case kAudioObjectPropertyManufacturer:
                    REQUIRE_SIZE (CFStringRef);
                    *(CFStringRef*) outData = CFSTR ("taw");
                    return kAudioHardwareNoError;
                case kAudioObjectPropertyOwnedObjects:
                case kAudioPlugInPropertyDeviceList:
                {
                    AudioObjectID ids[kNumDevices];

                    for (int i = 0; i < kNumDevices; ++i)
                        ids[i] = deviceID (i);

                    return objectList (ids, kNumDevices, inDataSize, outDataSize, outData);
                }
                case kAudioPlugInPropertyBoxList:
                    *outDataSize = 0;
                    return kAudioHardwareNoError;
                case kAudioPlugInPropertyTranslateUIDToDevice:
                {
                    REQUIRE_SIZE (AudioObjectID);
                    *(AudioObjectID*) outData = kAudioObjectUnknown;

                    if (qualifierSize == sizeof (CFStringRef) && qualifier != NULL)
                        for (int i = 0; i < kNumDevices; ++i)
                        {
                            CFStringRef uid = deviceUID (i);

                            if (CFStringCompare (*(const CFStringRef*) qualifier, uid, 0) == kCFCompareEqualTo)
                                *(AudioObjectID*) outData = deviceID (i);

                            CFRelease (uid);
                        }

                    return kAudioHardwareNoError;
                }
                case kAudioPlugInPropertyResourceBundle:
                    REQUIRE_SIZE (CFStringRef);
                    *(CFStringRef*) outData = CFSTR ("");
                    return kAudioHardwareNoError;
                default:
                    return kAudioHardwareUnknownPropertyError;
            }

        case kDevice:
        {
            const bool hub = d == kHubDevice;
            const AudioObjectPropertyScope scope = address->mScope;

            switch (s)
            {
                case kAudioObjectPropertyBaseClass:
                    REQUIRE_SIZE (AudioClassID);
                    *(AudioClassID*) outData = kAudioObjectClassID;
                    return kAudioHardwareNoError;
                case kAudioObjectPropertyClass:
                    REQUIRE_SIZE (AudioClassID);
                    *(AudioClassID*) outData = kAudioDeviceClassID;
                    return kAudioHardwareNoError;
                case kAudioObjectPropertyOwner:
                    REQUIRE_SIZE (AudioObjectID);
                    *(AudioObjectID*) outData = kAudioObjectPlugInObject;
                    return kAudioHardwareNoError;
                case kAudioObjectPropertyName:
                    REQUIRE_SIZE (CFStringRef);
                    *(CFStringRef*) outData = deviceName (d);
                    return kAudioHardwareNoError;
                case kAudioObjectPropertyManufacturer:
                    REQUIRE_SIZE (CFStringRef);
                    *(CFStringRef*) outData = CFSTR ("taw");
                    return kAudioHardwareNoError;
                case kAudioDevicePropertyDeviceUID:
                    REQUIRE_SIZE (CFStringRef);
                    *(CFStringRef*) outData = deviceUID (d);
                    return kAudioHardwareNoError;
                case kAudioDevicePropertyModelUID:
                    REQUIRE_SIZE (CFStringRef);
                    *(CFStringRef*) outData = CFSTR ("com.taw.driver.model");
                    return kAudioHardwareNoError;
                case kAudioDevicePropertyTransportType:
                    REQUIRE_SIZE (UInt32);
                    *(UInt32*) outData = kAudioDeviceTransportTypeVirtual;
                    return kAudioHardwareNoError;
                case kAudioDevicePropertyRelatedDevices:
                {
                    const AudioObjectID self = object;
                    return objectList (&self, 1, inDataSize, outDataSize, outData);
                }
                case kAudioDevicePropertyClockDomain:
                    REQUIRE_SIZE (UInt32);
                    *(UInt32*) outData = kClockDomain;
                    return kAudioHardwareNoError;
                case kAudioDevicePropertyDeviceIsAlive:
                case kAudioDevicePropertyClockIsStable:
                    REQUIRE_SIZE (UInt32);
                    *(UInt32*) outData = 1;
                    return kAudioHardwareNoError;
                case kAudioDevicePropertyDeviceIsRunning:
                    REQUIRE_SIZE (UInt32);
                    *(UInt32*) outData = gIOCount[d] > 0 ? 1 : 0;
                    return kAudioHardwareNoError;
                case kAudioDevicePropertyDeviceCanBeDefaultDevice:
                    REQUIRE_SIZE (UInt32);
                    *(UInt32*) outData = hub ? 0 : 1;
                    return kAudioHardwareNoError;
                case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                    REQUIRE_SIZE (UInt32);
                    *(UInt32*) outData = 0;
                    return kAudioHardwareNoError;
                case kAudioDevicePropertyIsHidden:
                    REQUIRE_SIZE (UInt32);
                    *(UInt32*) outData = hub ? 1 : 0;
                    return kAudioHardwareNoError;
                case kAudioDevicePropertyLatency:
                case kAudioDevicePropertySafetyOffset:
                    REQUIRE_SIZE (UInt32);
                    *(UInt32*) outData = 0;
                    return kAudioHardwareNoError;
                case kAudioDevicePropertyZeroTimeStampPeriod:
                    REQUIRE_SIZE (UInt32);
                    *(UInt32*) outData = kRingFrames;
                    return kAudioHardwareNoError;
                case kAudioDevicePropertyNominalSampleRate:
                    REQUIRE_SIZE (Float64);
                    *(Float64*) outData = gSampleRate;
                    return kAudioHardwareNoError;
                case kAudioDevicePropertyAvailableNominalSampleRates:
                {
                    AudioValueRange ranges[kNumRates];

                    for (int i = 0; i < kNumRates; ++i)
                        ranges[i].mMinimum = ranges[i].mMaximum = kRates[i];

                    const UInt32 n = (UInt32) (inDataSize / sizeof (AudioValueRange)) < kNumRates ? (UInt32) (inDataSize / sizeof (AudioValueRange)) : kNumRates;
                    memcpy (outData, ranges, n * sizeof (AudioValueRange));
                    *outDataSize = n * sizeof (AudioValueRange);
                    return kAudioHardwareNoError;
                }
                case kAudioDevicePropertyPreferredChannelsForStereo:
                    if (inDataSize < 2 * sizeof (UInt32))
                        return kAudioHardwareBadPropertySizeError;

                    ((UInt32*) outData)[0] = 1;
                    ((UInt32*) outData)[1] = 2;
                    *outDataSize = 2 * sizeof (UInt32);
                    return kAudioHardwareNoError;
                case kAudioObjectPropertyControlList:
                    *outDataSize = 0;
                    return kAudioHardwareNoError;
                case kAudioObjectPropertyOwnedObjects:
                case kAudioDevicePropertyStreams:
                {
                    AudioObjectID ids[2];
                    UInt32 n = 0;

                    if (scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeInput)
                        ids[n++] = inputStreamID (d);

                    if (scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeOutput)
                        ids[n++] = outputStreamID (d);

                    return objectList (ids, n, inDataSize, outDataSize, outData);
                }
                default:
                    return kAudioHardwareUnknownPropertyError;
            }
        }

        case kInputStream:
        case kOutputStream:
        {
            const bool input = kind == kInputStream;

            switch (s)
            {
                case kAudioObjectPropertyBaseClass:
                    REQUIRE_SIZE (AudioClassID);
                    *(AudioClassID*) outData = kAudioObjectClassID;
                    return kAudioHardwareNoError;
                case kAudioObjectPropertyClass:
                    REQUIRE_SIZE (AudioClassID);
                    *(AudioClassID*) outData = kAudioStreamClassID;
                    return kAudioHardwareNoError;
                case kAudioObjectPropertyOwner:
                    REQUIRE_SIZE (AudioObjectID);
                    *(AudioObjectID*) outData = deviceID (d);
                    return kAudioHardwareNoError;
                case kAudioObjectPropertyOwnedObjects:
                    *outDataSize = 0;
                    return kAudioHardwareNoError;
                case kAudioStreamPropertyIsActive:
                    REQUIRE_SIZE (UInt32);
                    *(UInt32*) outData = 1;
                    return kAudioHardwareNoError;
                case kAudioStreamPropertyDirection:
                    REQUIRE_SIZE (UInt32);
                    *(UInt32*) outData = input ? 1 : 0;
                    return kAudioHardwareNoError;
                case kAudioStreamPropertyTerminalType:
                    REQUIRE_SIZE (UInt32);
                    *(UInt32*) outData = input ? kAudioStreamTerminalTypeMicrophone : kAudioStreamTerminalTypeSpeaker;
                    return kAudioHardwareNoError;
                case kAudioStreamPropertyStartingChannel:
                    REQUIRE_SIZE (UInt32);
                    *(UInt32*) outData = 1;
                    return kAudioHardwareNoError;
                case kAudioStreamPropertyLatency:
                    REQUIRE_SIZE (UInt32);
                    *(UInt32*) outData = 0;
                    return kAudioHardwareNoError;
                case kAudioStreamPropertyVirtualFormat:
                case kAudioStreamPropertyPhysicalFormat:
                    REQUIRE_SIZE (AudioStreamBasicDescription);
                    *(AudioStreamBasicDescription*) outData = formatAt (gSampleRate, channelsOf (d));
                    return kAudioHardwareNoError;
                case kAudioStreamPropertyAvailableVirtualFormats:
                case kAudioStreamPropertyAvailablePhysicalFormats:
                {
                    const UInt32 fit = (UInt32) (inDataSize / sizeof (AudioStreamRangedDescription));
                    const UInt32 n = fit < kNumRates ? fit : kNumRates;

                    for (UInt32 i = 0; i < n; ++i)
                    {
                        AudioStreamRangedDescription* r = (AudioStreamRangedDescription*) outData + i;
                        r->mFormat = formatAt (kRates[i], channelsOf (d));
                        r->mSampleRateRange.mMinimum = r->mSampleRateRange.mMaximum = kRates[i];
                    }

                    *outDataSize = n * sizeof (AudioStreamRangedDescription);
                    return kAudioHardwareNoError;
                }
                default:
                    return kAudioHardwareUnknownPropertyError;
            }
        }

        default:
            return kAudioHardwareBadObjectError;
    }
}

static OSStatus SetPropertyData (AudioServerPlugInDriverRef inDriver, AudioObjectID object, pid_t pid, const AudioObjectPropertyAddress* address,
                                 UInt32 qualifierSize, const void* qualifier, UInt32 inDataSize, const void* inData)
{
    (void) pid; (void) qualifierSize; (void) qualifier;

    if (inDriver != gDriver)
        return kAudioHardwareBadObjectError;

    int d = 0;
    const Kind kind = kindOf (object, &d);
    const AudioObjectPropertySelector s = address->mSelector;

    if (kind == kDevice && s == kAudioDevicePropertyNominalSampleRate)
    {
        if (inDataSize != sizeof (Float64))
            return kAudioHardwareBadPropertySizeError;

        return requestRate (*(const Float64*) inData);
    }

    if ((kind == kInputStream || kind == kOutputStream) && (s == kAudioStreamPropertyVirtualFormat || s == kAudioStreamPropertyPhysicalFormat))
    {
        if (inDataSize != sizeof (AudioStreamBasicDescription))
            return kAudioHardwareBadPropertySizeError;

        const AudioStreamBasicDescription* f = (const AudioStreamBasicDescription*) inData;

        if (f->mFormatID != kAudioFormatLinearPCM || f->mChannelsPerFrame != channelsOf (d) || f->mBitsPerChannel != 32)
            return kAudioDeviceUnsupportedFormatError;

        return requestRate (f->mSampleRate);
    }

    return kAudioHardwareUnknownPropertyError;
}

//==============================================================================
// IO.

static OSStatus StartIO (AudioServerPlugInDriverRef inDriver, AudioObjectID device, UInt32 client)
{
    (void) client;
    int d = 0;

    if (inDriver != gDriver || kindOf (device, &d) != kDevice)
        return kAudioHardwareBadObjectError;

    pthread_mutex_lock (&gLock);
    ++gIOCount[d];
    pthread_mutex_unlock (&gLock);
    return kAudioHardwareNoError;
}

static OSStatus StopIO (AudioServerPlugInDriverRef inDriver, AudioObjectID device, UInt32 client)
{
    (void) client;
    int d = 0;

    if (inDriver != gDriver || kindOf (device, &d) != kDevice)
        return kAudioHardwareBadObjectError;

    pthread_mutex_lock (&gLock);

    if (gIOCount[d] > 0)
        --gIOCount[d];

    pthread_mutex_unlock (&gLock);
    return kAudioHardwareNoError;
}

// One clock for every device: the period count since the anchor, so sample time t is the same
// instant whichever device asks.
static OSStatus GetZeroTimeStamp (AudioServerPlugInDriverRef inDriver, AudioObjectID device, UInt32 client,
                                  Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed)
{
    (void) device; (void) client;

    if (inDriver != gDriver)
        return kAudioHardwareBadObjectError;

    const Float64 ticksPerPeriod = gHostTicksPerFrame * kRingFrames;
    const UInt64 now = mach_absolute_time();
    const UInt64 periods = (UInt64) ((Float64) (now - gAnchorHostTime) / ticksPerPeriod);

    *outSampleTime = (Float64) (periods * kRingFrames);
    *outHostTime = gAnchorHostTime + (UInt64) ((Float64) periods * ticksPerPeriod);
    *outSeed = 1;
    return kAudioHardwareNoError;
}

static OSStatus WillDoIOOperation (AudioServerPlugInDriverRef inDriver, AudioObjectID device, UInt32 client, UInt32 operation,
                                   Boolean* outWillDo, Boolean* outWillDoInPlace)
{
    (void) inDriver; (void) device; (void) client;
    *outWillDo = operation == kAudioServerPlugInIOOperationReadInput || operation == kAudioServerPlugInIOOperationWriteMix;
    *outWillDoInPlace = true;
    return kAudioHardwareNoError;
}

static OSStatus BeginIOOperation (AudioServerPlugInDriverRef d, AudioObjectID device, UInt32 client, UInt32 operation, UInt32 frames,
                                  const AudioServerPlugInIOCycleInfo* info)
{
    (void) d; (void) device; (void) client; (void) operation; (void) frames; (void) info;
    return kAudioHardwareNoError;
}

static OSStatus DoIOOperation (AudioServerPlugInDriverRef inDriver, AudioObjectID device, AudioObjectID stream, UInt32 client, UInt32 operation,
                               UInt32 frames, const AudioServerPlugInIOCycleInfo* info, void* mainBuffer, void* secondaryBuffer)
{
    (void) stream; (void) client; (void) secondaryBuffer;
    int d = 0;

    if (inDriver != gDriver || kindOf (device, &d) != kDevice || mainBuffer == NULL)
        return kAudioHardwareBadObjectError;

    float* audio = (float*) mainBuffer;

    if (operation == kAudioServerPlugInIOOperationReadInput)
    {
        const SInt64 t = (SInt64) info->mInputTime.mSampleTime;

        if (d == kHubDevice)
            for (int n = 0; n < kNumApps; ++n)
                ringRead (&gPlay[n], t, audio, frames, kHubChannels, (UInt32) (2 * n));
        else
            ringRead (&gMic[d], t, audio, frames, kAppChannels, 0);
    }
    else if (operation == kAudioServerPlugInIOOperationWriteMix)
    {
        const SInt64 t = (SInt64) info->mOutputTime.mSampleTime;

        if (d == kHubDevice)
            for (int n = 0; n < kNumApps; ++n)
                ringWrite (&gMic[n], t, audio, frames, kHubChannels, (UInt32) (2 * n));
        else
            ringWrite (&gPlay[d], t, audio, frames, kAppChannels, 0);
    }

    return kAudioHardwareNoError;
}

static OSStatus EndIOOperation (AudioServerPlugInDriverRef d, AudioObjectID device, UInt32 client, UInt32 operation, UInt32 frames,
                                const AudioServerPlugInIOCycleInfo* info)
{
    (void) d; (void) device; (void) client; (void) operation; (void) frames; (void) info;
    return kAudioHardwareNoError;
}
