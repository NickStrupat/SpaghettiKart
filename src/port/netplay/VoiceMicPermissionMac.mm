// macOS's permission to use the microphone, for VoiceDeviceMac.cpp: the one part of voice chat that needs
// Objective-C. The game's Info.plist says why it asks (NSMicrophoneUsageDescription), and its entitlements allow the
// microphone under the hardened runtime (cmake/macos/entitlements.plist).

#import <AVFoundation/AVFoundation.h>

extern "C" {

/** @returns 0 while the player has not been asked, 1 when the microphone is allowed, 2 when it is refused. */
int sk_mic_permission(void) {
    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]) {
        case AVAuthorizationStatusAuthorized:
            return 1;
        case AVAuthorizationStatusNotDetermined:
            return 0;
        default:
            return 2;
    }
}

/** Has macOS ask the player; sk_mic_permission() changes once they answer. */
void sk_mic_request_permission(void) {
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                             completionHandler:^(BOOL) {
                             }];
}

}
