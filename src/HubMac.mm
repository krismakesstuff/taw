#include "HubMac.h"

#import <AVFoundation/AVFoundation.h>

namespace taw::hub::mac
{
juce::String microphoneAccess()
{
    switch ([AVCaptureDevice authorizationStatusForMediaType: AVMediaTypeAudio])
    {
        case AVAuthorizationStatusAuthorized:    return "granted";
        case AVAuthorizationStatusDenied:        return "denied";
        case AVAuthorizationStatusNotDetermined: return "not asked yet";
        case AVAuthorizationStatusRestricted:    return "restricted";
    }

    return "unknown";
}
} // namespace taw::hub::mac
