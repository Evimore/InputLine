//
//  ILNBridge.mm
//  InputLine
//

#import "ILNBridge.h"
#import "ILNDiscovery.h"
#import "ILNTritonBLE.h"

#import <Network/Network.h>
#import <Security/Security.h>
#import <UIKit/UIKit.h>

#include "inputline/link_client.h"
#include "inputline/timing_stats.h"
#include "inputline/version.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <memory>
#include <netdb.h>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

using namespace inputline;

namespace {

    const NSTimeInterval kTickInterval = 0.02;
    const NSTimeInterval kProbeRetry = 0.5;
    const NSTimeInterval kNotFoundAfter = 3.0;
    // Still no answer: open the sockets again, and look names up again.
    const NSTimeInterval kReopenProbesAfter = 10.0;
    // At home the PC can answer both directly and through Tailscale; the
    // direct path is faster, so it gets this long to answer first.
    const NSTimeInterval kPreferDirectFor = 0.25;
    // Network changes come in bursts: act once it has settled.
    const NSTimeInterval kNetworkSettle = 0.25;
    // After the network changed, how long the PC has to answer on the new one
    // before InputLine looks for it at every address it knows.
    const NSTimeInterval kNetworkCheckTimeout = 1.5;
    const NSTimeInterval kPairRetry = 1.0;
    const NSTimeInterval kHelloRetry = 0.5;
    const int kMaxHelloAttempts = 6;
    const NSTimeInterval kPingInterval = 1.0;
    const NSTimeInterval kLinkLostAfter = 4.0;
    const NSTimeInterval kAttachRetry = 0.3;
    const NSTimeInterval kLizardInterval = 2.0;
    // Without a PC for this long, the controller works as this device's
    // mouse again (its built-in mode), so it is never stuck doing nothing.
    const NSTimeInterval kMouseModeAfterLinkDown = 10.0;
    const NSTimeInterval kLiveTimingWindow = 5.0;
    // inputline-host drops a session after 3 s of silence. An app in the
    // background does not run while the controller is idle, so after a longer
    // pause assume the session is gone and start a new one straight away.
    const NSTimeInterval kResumeAfterSilence = 2.5;
    // Asked for even if the controller also sends changes by itself.
    const NSTimeInterval kBatteryReadInterval = 60.0;
    const NSUInteger kMaxEvents = 200;
    const int kMaxControllers = 4;

    NSString *const kKeychainService = @"InputLine";
    // "-v2": pairings from before the safer pairing exchange are not used.
    NSString *const kKeychainAccount = @"pairings-v2";
    NSString *const kAddressDefaultsKey = @"ILNPCAddress";
    NSString *const kDisconnectedDefaultsKey = @"ILNDisconnectedFromPC";  // the user tapped Disconnect
    // Every address a PC answered on (home network, VPN...), newest first, by
    // host name; and the host last connected to. All of them are tried at
    // once, with the saved address.
    NSString *const kKnownAddressesDefaultsKey = @"ILNKnownAddresses";
    NSString *const kLastHostDefaultsKey = @"ILNLastHost";
    const NSUInteger kMaxKnownAddresses = 4;
    NSString *const kRestoreIdentifier = @"com.evimore.InputLine.bluetooth";
    NSString *const kControllersDefaultsKey = @"ILNControllers";
    // What each controller said about itself (serial, firmware attributes), by
    // identifier: used when it doesn't answer in time after a reconnect.
    NSString *const kIdentitiesDefaultsKey = @"ILNControllerIdentities";
    // How long to keep asking a controller who it is before plugging it in on
    // the PC: longest for its serial when it's new (Steam keys its settings
    // by it), shorter once the serial is known or what it said last time is.
    const NSTimeInterval kIdentifyFor = 10.0;
    const NSTimeInterval kIdentifyForKnown = 3.0;
    const NSTimeInterval kIdentifyRetry = 0.25;
    // GET_STRING_ATTRIBUTE (0xAE) index 1, the unit serial; GET_ATTRIBUTES_VALUES (0x83).
    const std::uint8_t kSerialRequest[] = {0x01, 0xAE, 0x01, 0x01};
    const std::uint8_t kAttributesRequest[] = {0x01, 0x83, 0x00};

    const std::uint8_t kSettingLizardMode = 9;

    void SecureRandom(std::uint8_t *out, std::size_t length)
    {
        if (SecRandomCopyBytes(kSecRandomDefault, length, out) != errSecSuccess) {
            arc4random_buf(out, length);
        }
    }

    std::uint64_t RandomNonce()
    {
        std::uint64_t value = 0;
        SecureRandom(reinterpret_cast<std::uint8_t *>(&value), sizeof(value));
        return value;
    }

    std::uint64_t WallClockMicroseconds()
    {
        return (std::uint64_t)([NSDate date].timeIntervalSince1970 * 1e6);
    }

    /// Monotonic microseconds, for timing measurements.
    std::uint64_t MonotonicMicroseconds()
    {
        static mach_timebase_info_data_t timebase;
        if (timebase.denom == 0) {
            mach_timebase_info(&timebase);
        }
        return mach_absolute_time() * timebase.numer / timebase.denom / 1000;
    }

    std::string ToStdString(NSString *text)
    {
        return text ? std::string(text.UTF8String) : std::string();
    }

    NSString *ToNSString(const std::string &text)
    {
        return [NSString stringWithUTF8String:text.c_str()] ?: @"";
    }

    /** The unit serial in a GET_STRING_ATTRIBUTE reply, or nil. */
    NSString *SerialFromReply(NSData *reply)
    {
        const std::uint8_t *bytes = (const std::uint8_t *)reply.bytes;
        if (reply.length <= 4 || bytes[1] != 0xAE || bytes[3] != 0x01) {
            return nil;
        }
        NSString *serial = [[NSString alloc] initWithBytes:bytes + 4 length:strnlen((const char *)bytes + 4, reply.length - 4) encoding:NSASCIIStringEncoding];
        return serial.length > 0 ? serial : nil;
    }

    /** A GET_ATTRIBUTES_VALUES reply (report ID first), or nil. */
    NSData *AttributesFromReply(NSData *reply)
    {
        const std::uint8_t *bytes = (const std::uint8_t *)reply.bytes;
        return reply.length > 3 && bytes[1] == 0x83 ? reply : nil;
    }

    NSData *SettingReport(std::uint8_t setting, std::uint16_t value)
    {
        const std::uint8_t bytes[] = {0x01, 0x87, 0x03, setting, (std::uint8_t)(value & 0xFF), (std::uint8_t)(value >> 8)};
        return [NSData dataWithBytes:bytes length:sizeof(bytes)];
    }

    /// Tailscale hands out 100.64.0.0/10 and fd7a:115c:a1e0::/48.
    bool IsTailscaleAddress(const struct sockaddr *address)
    {
        if (address->sa_family == AF_INET) {
            const std::uint32_t ip = ntohl(reinterpret_cast<const struct sockaddr_in *>(address)->sin_addr.s_addr);
            return (ip & 0xFFC00000u) == 0x64400000u;
        }
        if (address->sa_family == AF_INET6) {
            static const std::uint8_t prefix[] = {0xfd, 0x7a, 0x11, 0x5c, 0xa1, 0xe0};
            return std::memcmp(reinterpret_cast<const struct sockaddr_in6 *>(address)->sin6_addr.s6_addr, prefix, sizeof(prefix)) == 0;
        }
        return false;
    }

    /// The socket addresses for a host and port: numbers only (no lookup), or any.
    NSArray<NSData *> *ResolveAddress(NSString *host, NSString *port, bool numericOnly)
    {
        struct addrinfo hints = {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_flags = numericOnly ? AI_NUMERICHOST : 0;
        struct addrinfo *info = NULL;
        NSMutableArray<NSData *> *addresses = [NSMutableArray array];
        if (getaddrinfo(host.UTF8String, port.UTF8String, &hints, &info) == 0) {
            for (struct addrinfo *entry = info; entry != NULL; entry = entry->ai_next) {
                [addresses addObject:[NSData dataWithBytes:entry->ai_addr length:entry->ai_addrlen]];
            }
            freeaddrinfo(info);
        }
        return addresses;
    }

    /// "Wi-Fi", "cellular, utun4"... for the event log.
    NSString *NetworkDescription(nw_path_t path)
    {
        if (nw_path_get_status(path) != nw_path_status_satisfied) {
            return @"no network";
        }
        NSMutableArray<NSString *> *names = [NSMutableArray array];
        nw_path_enumerate_interfaces(path, ^bool(nw_interface_t interface) {
            NSString *name = nil;
            switch (nw_interface_get_type(interface)) {
                case nw_interface_type_wifi:
                    name = @"Wi-Fi";
                    break;
                case nw_interface_type_cellular:
                    name = @"cellular";
                    break;
                case nw_interface_type_wired:
                    name = @"wired";
                    break;
                case nw_interface_type_loopback:
                    break;
                default: {  // a VPN, for example
                    const char *interfaceName = nw_interface_get_name(interface);
                    name = interfaceName != NULL ? [NSString stringWithUTF8String:interfaceName] : nil;
                    break;
                }
            }
            if (name != nil && ![names containsObject:name]) {
                [names addObject:name];
            }
            return true;
        });
        return names.count > 0 ? [names componentsJoinedByString:@", "] : @"connected";
    }

    /// "host", "host:port", "[v6]", "[v6]:port" or a bare IPv6 address.
    void SplitAddress(NSString *address, NSString **host, NSString **port)
    {
        NSString *trimmed = [address stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        *port = [NSString stringWithFormat:@"%u", link::kDefaultPort];
        if ([trimmed hasPrefix:@"["]) {
            const NSRange close = [trimmed rangeOfString:@"]"];
            if (close.location != NSNotFound) {
                *host = [trimmed substringWithRange:NSMakeRange(1, close.location - 1)];
                NSString *rest = [trimmed substringFromIndex:close.location + 1];
                if ([rest hasPrefix:@":"] && rest.length > 1) {
                    *port = [rest substringFromIndex:1];
                }
                return;
            }
        }
        NSArray<NSString *> *parts = [trimmed componentsSeparatedByString:@":"];
        if (parts.count == 2) {
            *host = parts[0];
            *port = parts[1];
        } else {
            *host = trimmed;
        }
    }

}  // namespace

@implementation ILNStatus
@end

@implementation ILNControllerInfo
@end

#pragma mark - Per-controller state

@interface ILNController : NSObject
@property (nonatomic, strong) ILNTritonDevice *device;
@property (nonatomic, assign) std::uint8_t linkIndex;
@property (nonatomic, copy, nullable) NSString *serial;
@property (nonatomic, copy, nullable) NSData *attributes;
@property (nonatomic, assign) BOOL identified;
@property (nonatomic, assign) CFAbsoluteTime identifyStarted;
@property (nonatomic, copy, nullable) NSDictionary *rememberedIdentity;  // what it said last time
@property (nonatomic, assign) BOOL attached;
@property (nonatomic, assign) CFAbsoluteTime lastAttachSent;
@property (nonatomic, assign) CFAbsoluteTime lastLizardSent;
@property (nonatomic, assign) BOOL mouseModeOn;  // handed back to this device
// Still set up the way Steam left it: attached, and neither disconnected nor
// handed back to this device since. After a network drop the PC then keeps
// its virtual controller instead of plugging in a new one.
@property (nonatomic, assign) BOOL keptSettings;
// The controller's latest battery report (0x43), sent to the PC on attach and on change.
@property (nonatomic, copy, nullable) NSData *batteryReport;
// Each input datagram also carries the previous report, so the PC can make
// up for a lost datagram.
@property (nonatomic, assign) std::uint32_t sequence;
@property (nonatomic, copy, nullable) NSData *previousReport;
@end

@implementation ILNController
@end

#pragma mark - Sockets

/// A UDP socket connected to one of the PC's addresses. Link queue.
@interface ILNLinkSocket : NSObject
@property (nonatomic, copy) NSString *address;  // as typed, remembered or discovered
@property (nonatomic, copy) NSData *peer;       // what it resolved to
@property (nonatomic, assign) int fd;           // -1 once shut
@property (nonatomic, assign) BOOL viaTailscale;
@property (nonatomic, strong, nullable) dispatch_source_t readSource;
- (void)shut;
@end

@implementation ILNLinkSocket

- (instancetype)init
{
    if ((self = [super init])) {
        _fd = -1;
    }
    return self;
}

- (void)shut
{
    if (_readSource != nil) {
        dispatch_source_cancel(_readSource);  // its cancel handler closes the socket
        _readSource = nil;
    } else if (_fd >= 0) {
        close(_fd);
    }
    _fd = -1;
}

- (void)dealloc
{
    [self shut];
}

@end

#pragma mark - ILNBridge

@interface ILNBridge () <ILNTritonBLEDelegate, ILNDiscoveryDelegate>
@end

@implementation ILNBridge {
    dispatch_queue_t _queue;
    dispatch_source_t _timer;
    ILNTritonBLE *_ble;
    ILNDiscovery *_discovery;               // main thread
    NSArray<ILNDiscoveredPC *> *_discovered;  // link queue

    NSString *_address;       // where the PC answered, or the saved address while searching
    NSString *_savedAddress;  // the user's choice, or where the PC last answered
    ILNLinkSocket *_link;     // to _address, once the PC answered there
    // While searching: a socket to each address the PC may be at, the names
    // being looked up, and a PC's answer through Tailscale while the direct
    // addresses get a moment to answer too.
    NSMutableDictionary<NSString *, ILNLinkSocket *> *_probes;
    NSMutableSet<NSString *> *_resolving;
    NSUInteger _probeGeneration;
    CFAbsoluteTime _reopenProbesAt;
    std::optional<link::ProbeReply> _heldReply;
    ILNLinkSocket *_heldReplySocket;
    CFAbsoluteTime _heldReplyUntil;
    nw_path_monitor_t _pathMonitor;
    NSString *_network;                  // the network's description, for the event log
    CFAbsoluteTime _networkChangedAt;    // 0: no change to handle
    CFAbsoluteTime _checkingLinkSince;   // the network changed while connected: 0 once the PC answered
    NSString *_hostName;
    NSString *_clientName;  // read once on the main thread
    ILNLinkState _state;
    UIBackgroundTaskIdentifier _setupTask;  // main thread: time to finish setting up a controller
    CFAbsoluteTime _stateEnteredAt;
    CFAbsoluteTime _lastSend;
    CFAbsoluteTime _lastService;
    CFAbsoluteTime _lastBatteryRead;
    std::uint64_t _probeNonce;
    int _helloAttempts;
    BOOL _everConnected;
    CFAbsoluteTime _lastPing;
    CFAbsoluteTime _lastPong;
    double _rttMs;

    std::unique_ptr<link::ClientSession> _session;
    link::ClientSession::PairingAttempt _pairingAttempt;
    link::Key _hostPairingKey;  // the PC's CPace share for this attempt, from its ProbeReply
    NSString *_enteredCode;  // typed before the PC's share for a new attempt arrived
    BOOL _codeRequested;
    NSString *_versionProblem;  // set when the PC and this app can't talk: which one to update
    NSString *_updateNote;  // they can talk, but run different versions
    BOOL _paused;  // the user tapped Disconnect
    CFAbsoluteTime _linkDownSince;  // 0 while connected
    NSString *_appVersion;
    std::vector<std::uint8_t> _pendingPairRequest;

    NSMutableDictionary<NSUUID *, ILNController *> *_controllers;

    // Bluetooth report timing on this device, per app state.
    BOOL _inBackground;
    TimingStats _timingForeground;
    TimingStats _timingBackground;
    TimingStats _timingLive;
    TimingStats _timingSent;  // when reports actually leave for the PC
    NSString *_lastEvent;
    NSUInteger _eventRepeats;
    CFAbsoluteTime _liveStarted;
    NSString *_liveSummary;

    CFAbsoluteTime _lastDatagramSent;
    NSMutableArray<NSString *> *_events;
    NSDateFormatter *_eventTime;
}

+ (instancetype)shared
{
    static ILNBridge *bridge;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        bridge = [[ILNBridge alloc] initPrivate];
    });
    return bridge;
}

- (instancetype)initPrivate
{
    if ((self = [super init])) {
        _probes = [NSMutableDictionary dictionary];
        _resolving = [NSMutableSet set];
        _setupTask = UIBackgroundTaskInvalid;
        _rttMs = -1;
        _controllers = [NSMutableDictionary dictionary];
        _liveSummary = @"";
        _events = [NSMutableArray array];
        _eventTime = [[NSDateFormatter alloc] init];
        _eventTime.dateFormat = @"HH:mm:ss.SSS";
        NSString *deviceName = [UIDevice currentDevice].name;  // shared is first used on the main thread
        _clientName = deviceName.length > 0 ? deviceName : @"InputLine";
        NSDictionary *info = [NSBundle mainBundle].infoDictionary;
        _appVersion = info[@"InputLineVersion"] ?: info[@"CFBundleShortVersionString"] ?: @"";
        _queue = dispatch_queue_create("com.evimore.inputline.link", DISPATCH_QUEUE_SERIAL);
        dispatch_set_target_queue(_queue, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0));
        _address = [[NSUserDefaults standardUserDefaults] stringForKey:kAddressDefaultsKey];
        _savedAddress = _address;
        _state = _address.length == 0 ? ILNLinkStateNoPC
               : [[NSUserDefaults standardUserDefaults] boolForKey:kDisconnectedDefaultsKey] ? ILNLinkStateDisconnected
                                                                                            : ILNLinkStateSearching;
        _inBackground = [UIApplication sharedApplication].applicationState == UIApplicationStateBackground;

        NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
        [center addObserver:self selector:@selector(didEnterBackground) name:UIApplicationDidEnterBackgroundNotification object:nil];
        [center addObserver:self selector:@selector(willEnterForeground) name:UIApplicationWillEnterForegroundNotification object:nil];
    }
    return self;
}

- (void)start
{
    const BOOL launchedInBackground = [UIApplication sharedApplication].applicationState == UIApplicationStateBackground;
    dispatch_async(_queue, ^{
        if (self->_timer != nil) {
            return;
        }
        [self logEvent:launchedInBackground ? @"InputLine started in the background (iOS launched it for a controller)"
                                            : @"InputLine started"];
        if (self->_address.length > 0 && self->_state != ILNLinkStateDisconnected) {
            [self enterState:ILNLinkStateSearching];
        }
        [self watchNetwork];
        self->_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, self->_queue);
        dispatch_source_set_timer(self->_timer, DISPATCH_TIME_NOW, (uint64_t)(kTickInterval * NSEC_PER_SEC), NSEC_PER_MSEC);
        __weak ILNBridge *weakSelf = self;
        dispatch_source_set_event_handler(self->_timer, ^{
            [weakSelf service];
        });
        dispatch_resume(self->_timer);
    });
    _ble = [[ILNTritonBLE alloc] initWithDelegate:self restoreIdentifier:kRestoreIdentifier];
    NSMutableArray<NSUUID *> *remembered = [NSMutableArray array];
    for (NSString *text in [[NSUserDefaults standardUserDefaults] stringArrayForKey:kControllersDefaultsKey]) {
        NSUUID *identifier = [[NSUUID alloc] initWithUUIDString:text];
        if (identifier != nil) {
            [remembered addObject:identifier];
        }
    }
    _ble.rememberedIdentifiers = remembered;
    [_ble start];

    _discovery = [[ILNDiscovery alloc] initWithDelegate:self];
    if ([UIApplication sharedApplication].applicationState != UIApplicationStateBackground) {
        [_discovery start];
    }
}

- (void)connectToPC:(NSString *)address
{
    NSString *trimmed = [address stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    [[NSUserDefaults standardUserDefaults] setObject:trimmed forKey:kAddressDefaultsKey];
    [[NSUserDefaults standardUserDefaults] removeObjectForKey:kDisconnectedDefaultsKey];
    dispatch_async(_queue, ^{
        if (self->_state == ILNLinkStateConnected && self->_session) {
            [self detachAll];
            [self sendDatagram:self->_session->make_bye()];
        }
        self->_address = trimmed;
        self->_versionProblem = nil;
        self->_updateNote = nil;
        self->_savedAddress = trimmed;
        self->_hostName = nil;
        self->_session.reset();
        self->_everConnected = NO;
        [self closeLink];
        if (trimmed.length == 0) {
            [self enterState:ILNLinkStateNoPC];
            return;
        }
        [self enterState:ILNLinkStateSearching];
    });
}

- (void)disconnectFromPC
{
    [[NSUserDefaults standardUserDefaults] setBool:YES forKey:kDisconnectedDefaultsKey];
    dispatch_async(_queue, ^{
        if (self->_state == ILNLinkStateDisconnected || self->_address.length == 0) {
            return;
        }
        if (self->_state == ILNLinkStateConnected && self->_session) {
            [self detachAll];
            [self sendDatagram:self->_session->make_bye()];
        }
        self->_session.reset();
        self->_pendingPairRequest.clear();
        [self closeLink];
        [self enterState:ILNLinkStateDisconnected];
        [self logEvent:@"Disconnected from the PC: the controller works with this device until you tap Connect"];
    });
}

- (void)scanForNewControllers
{
    [_ble scanForNewControllers:30];
}

- (void)resetTiming
{
    dispatch_async(_queue, ^{
        self->_timingForeground.reset();
        self->_timingBackground.reset();
        self->_timingLive.reset();
        self->_timingSent.reset();
        self->_liveSummary = @"";
    });
}

#pragma mark - Discovery

- (void)discoveryDidChange:(ILNDiscovery *)discovery
{
    NSArray<ILNDiscoveredPC *> *pcs = discovery.pcs;
    dispatch_async(_queue, ^{
        self->_discovered = pcs;
        [self useDiscoveredPairedPC];
        [self probeNewCandidates];
    });
}

- (void)discovery:(ILNDiscovery *)discovery didFailWithCode:(NSInteger)code
{
    dispatch_async(_queue, ^{
        [self logEvent:[NSString stringWithFormat:@"Cannot look for PCs on this network (error %ld). Is Local Network on for InputLine in Settings?", (long)code]];
    });
}

/// If there is no saved address and a PC this device is paired with shows up
/// on the network, connect to it. (While searching, such a PC is tried along
/// with the known addresses: see candidateAddresses.) Link queue.
- (void)useDiscoveredPairedPC
{
    if (_state != ILNLinkStateNoPC) {
        return;
    }
    for (ILNDiscoveredPC *pc in _discovered) {
        link::Pairing pairing;
        if ([pc.address isEqualToString:_address] || ![ILNBridge pairingForHostName:pc.name into:pairing]) {
            continue;
        }
        [self logEvent:[NSString stringWithFormat:@"Found %@ on this network at %@", pc.name, pc.address]];
        [self connectToPC:pc.address];
        return;
    }
}

#pragma mark - Known addresses

/// Where to look for the PC: the saved address, the others the last PC
/// answered on, and paired PCs found on this network. Link queue.
- (NSArray<NSString *> *)candidateAddresses
{
    NSMutableArray<NSString *> *candidates = [NSMutableArray array];
    if (_savedAddress.length > 0) {
        [candidates addObject:_savedAddress];
    }
    NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
    NSString *host = [defaults stringForKey:kLastHostDefaultsKey];
    id known = host.length > 0 ? [defaults dictionaryForKey:kKnownAddressesDefaultsKey][host] : nil;
    if ([known isKindOfClass:[NSArray class]]) {
        for (id address in (NSArray *)known) {
            if ([address isKindOfClass:[NSString class]] && ![candidates containsObject:address]) {
                [candidates addObject:address];
            }
        }
    }
    for (ILNDiscoveredPC *pc in _discovered) {
        link::Pairing pairing;
        if (![candidates containsObject:pc.address] && [ILNBridge pairingForHostName:pc.name into:pairing]) {
            [candidates addObject:pc.address];
        }
    }
    return candidates;
}

/// Connected: keep this address for the PC and make it the saved one. Link queue.
- (void)rememberWorkingAddress
{
    if (_hostName.length == 0 || _address.length == 0) {
        return;
    }
    NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
    NSMutableDictionary *known = [[defaults dictionaryForKey:kKnownAddressesDefaultsKey] mutableCopy] ?: [NSMutableDictionary dictionary];
    NSMutableArray *addresses = [known[_hostName] isKindOfClass:[NSArray class]] ? [known[_hostName] mutableCopy] : [NSMutableArray array];
    [addresses removeObject:_address];
    [addresses insertObject:_address atIndex:0];
    while (addresses.count > kMaxKnownAddresses) {
        [addresses removeLastObject];
    }
    known[_hostName] = addresses;
    [defaults setObject:known forKey:kKnownAddressesDefaultsKey];
    [defaults setObject:_hostName forKey:kLastHostDefaultsKey];
    [defaults setObject:_address forKey:kAddressDefaultsKey];
    _savedAddress = _address;
}

#pragma mark - App state

- (void)didEnterBackground
{
    [_discovery stop];  // Bonjour browsing needs the foreground anyway
    dispatch_async(_queue, ^{
        self->_inBackground = YES;
        self->_timingBackground.break_sequence();  // the switch itself is not a Bluetooth gap
        [self logEvent:@"App moved to the background"];
    });
}

- (void)willEnterForeground
{
    [_discovery start];
    [_ble refresh];
    dispatch_async(_queue, ^{
        self->_inBackground = NO;
        self->_timingForeground.break_sequence();
        [self logEvent:@"App back in the foreground"];
        [self kickLink];
    });
}

#pragma mark - Status

- (ILNStatus *)status
{
    __block ILNStatus *status = nil;
    dispatch_sync(_queue, ^{
        status = [[ILNStatus alloc] init];
        status.linkState = self->_state;
        status.pcAddress = self->_address ?: @"";
        status.pcName = self->_hostName ?: @"";
        status.paused = self->_paused;
        status.rttMs = self->_state == ILNLinkStateConnected ? self->_rttMs : -1;
        status.linkText = [self linkText];
        status.linkUp = self->_state == ILNLinkStateConnected || (self->_state == ILNLinkStateConnecting && self->_everConnected);
        status.updateText = self->_updateNote ?: @"";
        status.viaTailscale = self->_link.viaTailscale;
        status.problemText = self->_state == ILNLinkStateNotFound ? [self linkText] : @"";
        NSMutableArray<NSString *> *controllers = [NSMutableArray array];
        NSMutableArray<ILNControllerInfo *> *infos = [NSMutableArray array];
        for (ILNController *controller in self->_controllers.allValues) {
            ILNControllerInfo *info = [[ILNControllerInfo alloc] init];
            info.name = controller.device.name ?: @"Steam Controller";
            info.state = self->_paused || self->_state == ILNLinkStateDisconnected ? ILNControllerStateOnThisDevice
                       : self->_state != ILNLinkStateConnected ? ILNControllerStateWaitingForPC
                       : controller.attached ? ILNControllerStateOnPC
                       : ILNControllerStateConnecting;
            info.batteryLevel = controller.device.batteryLevel;
            info.batteryCharging = controller.device.batteryCharging;
            [infos addObject:info];
            NSString *state = info.state == ILNControllerStateOnThisDevice ? @"Disconnected from PC"
                            : info.state == ILNControllerStateWaitingForPC ? @"Waiting for PC"
                            : info.state == ILNControllerStateOnPC ? @"Connected to PC"
                            : @"Connecting to PC...";
            NSString *battery = info.batteryLevel >= 0 ? [NSString stringWithFormat:@", battery %ld%%%@%@", (long)info.batteryLevel,
                                                                                    info.batteryCharging ? @", charging" : @"",
                                                                                    controller.device.batteryFromReport ? @"" : @" (level only)"]
                                                       : @", battery unknown";
            [controllers addObject:[NSString stringWithFormat:@"%@: %@%@", info.name, state, battery]];
        }
        status.controllers = controllers;
        status.controllerInfo = infos;
        status.timingNow = self->_liveSummary;
        status.timingForeground = ToNSString(self->_timingForeground.summary().reports > 1 ? self->_timingForeground.summary().to_string() : "");
        status.timingBackground = ToNSString(self->_timingBackground.summary().reports > 1 ? self->_timingBackground.summary().to_string() : "");
        status.timingSent = ToNSString(self->_timingSent.summary().reports > 1 ? self->_timingSent.summary().to_string() : "");
        status.events = [self->_events copy];
        status.discoveredPCs = self->_discovered ?: @[];
    });
    return status;
}

- (NSString *)linkText
{
    switch (_state) {
        case ILNLinkStateNoPC:
            return @"Enter your PC's address.";
        case ILNLinkStateSearching:
            return [NSString stringWithFormat:@"Looking for inputline-host on %@...", _address];
        case ILNLinkStateNotFound:
            if (_versionProblem != nil) {
                return _versionProblem;
            }
            return [NSString stringWithFormat:@"No answer from %@. Is inputline-host running there, and is UDP %u allowed through its firewall? Still trying.", _address, link::kDefaultPort];
        case ILNLinkStatePairing:
            return [NSString stringWithFormat:@"Pairing with %@: enter the code shown on its screen.", _hostName ?: _address];
        case ILNLinkStateConnecting:
            return [NSString stringWithFormat:@"Connecting to %@...", _hostName ?: _address];
        case ILNLinkStateConnected: {
            NSString *path = _link.viaTailscale ? @" through Tailscale" : @"";
            return _rttMs >= 0 ? [NSString stringWithFormat:@"Connected to %@%@ (round trip %.1f ms)", _hostName, path, _rttMs]
                               : [NSString stringWithFormat:@"Connected to %@%@", _hostName, path];
        }
        case ILNLinkStateDisconnected:
            return [NSString stringWithFormat:@"Disconnected from %@. Tap Connect to connect again.", _hostName ?: _address];
    }
    return @"";
}

- (void)tellUser:(NSString *)message
{
    [self logEvent:message];  // always called on the link queue
    dispatch_async(dispatch_get_main_queue(), ^{
        [self.delegate bridge:self showMessage:message];
    });
}

/// Record an event (link queue).
- (void)logEvent:(NSString *)event
{
    NSString *stamped = [NSString stringWithFormat:@"%@  %@", [_eventTime stringFromDate:[NSDate date]], event];
    if ([event isEqualToString:_lastEvent] && _events.count > 0) {
        // Steam repeats some settings every few seconds: fold repeats into one line.
        ++_eventRepeats;
        _events[_events.count - 1] = [NSString stringWithFormat:@"%@ (x%lu)", stamped, (unsigned long)(_eventRepeats + 1)];
        return;
    }
    NSLog(@"InputLine: %@", event);
    _lastEvent = event;
    _eventRepeats = 0;
    [_events addObject:stamped];
    if (_events.count > kMaxEvents) {
        [_events removeObjectAtIndex:0];
    }
}

- (NSString *)report
{
    ILNStatus *status = [self status];
    NSMutableString *text = [NSMutableString string];
    [text appendFormat:@"InputLine %@ on %@ (iOS %@)\n", _appVersion,
                       [UIDevice currentDevice].model, [UIDevice currentDevice].systemVersion];
    [text appendFormat:@"Link: %@\n", status.linkText];
    [text appendFormat:@"Controllers: %@\n", status.controllers.count > 0 ? [status.controllers componentsJoinedByString:@"; "] : @"none"];
    [text appendFormat:@"Bluetooth timing, foreground: %@\n", status.timingForeground];
    [text appendFormat:@"Bluetooth timing, background: %@\n", status.timingBackground];
    [text appendFormat:@"Sent to the PC: %@\n", status.timingSent];
    [text appendString:@"\nEvents:\n"];
    [text appendString:[status.events componentsJoinedByString:@"\n"]];
    return text;
}

#pragma mark - Keychain

+ (NSMutableDictionary<NSString *, NSString *> *)loadPairings
{
    NSDictionary *query = @{
        (__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrService: kKeychainService,
        (__bridge id)kSecAttrAccount: kKeychainAccount,
        (__bridge id)kSecReturnData: @YES,
        (__bridge id)kSecMatchLimit: (__bridge id)kSecMatchLimitOne,
    };
    CFTypeRef result = NULL;
    if (SecItemCopyMatching((__bridge CFDictionaryRef)query, &result) != errSecSuccess || result == NULL) {
        return [NSMutableDictionary dictionary];
    }
    NSData *data = (__bridge_transfer NSData *)result;
    id plist = [NSPropertyListSerialization propertyListWithData:data options:NSPropertyListMutableContainers format:NULL error:NULL];
    return [plist isKindOfClass:[NSMutableDictionary class]] ? plist : [NSMutableDictionary dictionary];
}

+ (void)storePairings:(NSDictionary<NSString *, NSString *> *)pairings
{
    NSData *data = [NSPropertyListSerialization dataWithPropertyList:pairings format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
    if (data == nil) {
        return;
    }
    NSDictionary *query = @{
        (__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrService: kKeychainService,
        (__bridge id)kSecAttrAccount: kKeychainAccount,
    };
    NSDictionary *update = @{(__bridge id)kSecValueData: data};
    if (SecItemUpdate((__bridge CFDictionaryRef)query, (__bridge CFDictionaryRef)update) == errSecItemNotFound) {
        NSMutableDictionary *add = [query mutableCopy];
        add[(__bridge id)kSecValueData] = data;
        // Readable in the background once the device was unlocked after boot.
        add[(__bridge id)kSecAttrAccessible] = (__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly;
        SecItemAdd((__bridge CFDictionaryRef)add, NULL);
    }
}

+ (BOOL)pairingForHostName:(NSString *)hostName into:(link::Pairing &)pairing
{
    NSArray<NSString *> *parts = [[self loadPairings][hostName] componentsSeparatedByString:@":"];
    if (parts.count != 2) {
        return NO;
    }
    const auto key = link::key_from_hex(ToStdString(parts[1]));
    const auto clientId = (std::uint32_t)strtoul(parts[0].UTF8String, NULL, 16);
    if (!key || clientId == 0) {
        return NO;
    }
    pairing.client_id = clientId;
    pairing.key = *key;
    return YES;
}

+ (void)savePairing:(const link::Pairing &)pairing forHostName:(NSString *)hostName
{
    NSMutableDictionary *pairings = [self loadPairings];
    const std::string keyHex = link::to_hex(pairing.key.data(), pairing.key.size());
    pairings[hostName] = [NSString stringWithFormat:@"%08x:%s", pairing.client_id, keyHex.c_str()];
    [self storePairings:pairings];
}

#pragma mark - Sockets (link queue)

/// A socket connected to the first of @p peers this device has a route to.
/// What it receives goes to handleDatagram:length:from:.
- (nullable ILNLinkSocket *)socketTo:(NSArray<NSData *> *)peers address:(NSString *)address
{
    for (NSData *peer in peers) {
        const struct sockaddr *destination = (const struct sockaddr *)peer.bytes;
        const int fd = socket(destination->sa_family, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, destination, (socklen_t)peer.length) != 0) {
            close(fd);
            continue;
        }
        // Low latency beats throughput for these tiny datagrams.
        const int serviceClass = NET_SERVICE_TYPE_VO;
        setsockopt(fd, SOL_SOCKET, SO_NET_SERVICE_TYPE, &serviceClass, sizeof(serviceClass));
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

        ILNLinkSocket *linkSocket = [[ILNLinkSocket alloc] init];
        linkSocket.address = address;
        linkSocket.peer = peer;
        linkSocket.fd = fd;
        linkSocket.viaTailscale = IsTailscaleAddress(destination);
        dispatch_source_t readSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0, _queue);
        __weak ILNBridge *weakSelf = self;
        __weak ILNLinkSocket *weakSocket = linkSocket;
        dispatch_source_set_event_handler(readSource, ^{
            std::uint8_t buffer[link::kMaxDatagram + 1];
            for (;;) {
                const ssize_t received = recv(fd, buffer, sizeof(buffer), 0);
                ILNLinkSocket *from = weakSocket;
                if (received <= 0 || from == nil || from.fd < 0) {
                    break;
                }
                [weakSelf handleDatagram:buffer length:(size_t)received from:from];
            }
        });
        dispatch_source_set_cancel_handler(readSource, ^{
            close(fd);
        });
        linkSocket.readSource = readSource;
        dispatch_resume(readSource);
        return linkSocket;
    }
    return nil;
}

- (void)closeLink
{
    [_link shut];
    _link = nil;
}

/// A fresh socket to the same address: after the network changed, or when
/// iOS invalidated the old one while the app was suspended.
- (void)reopenLink
{
    if (_link == nil) {
        return;
    }
    ILNLinkSocket *fresh = [self socketTo:@[_link.peer] address:_link.address];
    [_link shut];
    if (fresh != nil) {
        _link = fresh;
    }  // else no route right now: the next send tries again
}

- (void)sendDatagram:(const std::vector<std::uint8_t> &)datagram
{
    if (datagram.empty() || _link == nil) {
        return;
    }
    if (_link.fd < 0) {
        [self reopenLink];
        if (_link.fd < 0) {
            return;
        }
    }
    _lastDatagramSent = CFAbsoluteTimeGetCurrent();
    if (send(_link.fd, datagram.data(), datagram.size(), 0) < 0 && errno != EAGAIN && errno != ENOBUFS) {
        // iOS can invalidate sockets of apps that were suspended; start over.
        [self logEvent:[NSString stringWithFormat:@"Network send failed (%s); reopening the socket", strerror(errno)]];
        [self reopenLink];
    }
}

#pragma mark - Looking for the PC (link queue)

/// Probe every address the PC may be at, all at once; the first PC to answer
/// is the one to use (see handleProbeReply:).
- (void)startProbing
{
    [self stopProbing];
    [self closeLink];
    if (_savedAddress.length > 0) {
        _address = _savedAddress;  // until the PC answers somewhere
    }
    const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    _reopenProbesAt = now + kReopenProbesAfter;
    _lastSend = now;  // each socket sends its first probe as soon as it's open
    NSArray<NSString *> *candidates = [self candidateAddresses];
    if (candidates.count > 1) {
        [self logEvent:[NSString stringWithFormat:@"Link: trying %@", [candidates componentsJoinedByString:@", "]]];
    }
    for (NSString *address in candidates) {
        [self probeAddress:address];
    }
}

- (void)stopProbing
{
    ++_probeGeneration;  // drops lookups still running
    for (ILNLinkSocket *probe in _probes.allValues) {
        [probe shut];
    }
    [_probes removeAllObjects];
    [_resolving removeAllObjects];
    _heldReply.reset();
    _heldReplySocket = nil;
}

/// While searching: also try addresses that became known since it started.
- (void)probeNewCandidates
{
    if (_state != ILNLinkStateSearching && _state != ILNLinkStateNotFound) {
        return;
    }
    for (NSString *address in [self candidateAddresses]) {
        if (_probes[address] == nil && ![_resolving containsObject:address]) {
            [self probeAddress:address];
        }
    }
}

- (void)probeAddress:(NSString *)address
{
    NSString *host = nil;
    NSString *port = nil;
    SplitAddress(address, &host, &port);
    if (host.length == 0) {
        return;
    }
    NSArray<NSData *> *numeric = ResolveAddress(host, port, true);
    if (numeric.count > 0) {
        [self addProbe:address peers:numeric];
        return;
    }
    // A name can take seconds to look up, or to fail (a VPN name while the
    // VPN is off): look it up elsewhere, so the other addresses go ahead.
    [_resolving addObject:address];
    const NSUInteger generation = _probeGeneration;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSArray<NSData *> *resolved = ResolveAddress(host, port, false);
        dispatch_async(self->_queue, ^{
            if (generation != self->_probeGeneration) {
                return;
            }
            [self->_resolving removeObject:address];
            if (resolved.count == 0) {
                [self logEvent:[NSString stringWithFormat:@"Cannot resolve %@", address]];
                return;
            }
            [self addProbe:address peers:resolved];
        });
    });
}

- (void)addProbe:(NSString *)address peers:(NSArray<NSData *> *)peers
{
    ILNLinkSocket *probe = [self socketTo:peers address:address];
    if (probe == nil) {
        return;  // no route to it from this network
    }
    _probes[address] = probe;
    const auto datagram = link::ClientSession::make_probe(_probeNonce);
    send(probe.fd, datagram.data(), datagram.size(), 0);
}

- (void)sendProbes
{
    const auto datagram = link::ClientSession::make_probe(_probeNonce);
    for (ILNLinkSocket *probe in _probes.allValues) {
        send(probe.fd, datagram.data(), datagram.size(), 0);  // fails quietly where there's no route
    }
}

- (BOOL)probingDirectAddress
{
    for (ILNLinkSocket *probe in _probes.allValues) {
        if (!probe.viaTailscale) {
            return YES;
        }
    }
    return NO;
}

/// A PC answered at one of the addresses being tried.
- (void)handleProbeReply:(const std::uint8_t *)data length:(size_t)length from:(ILNLinkSocket *)socket
{
    const auto reply = link::ClientSession::parse_probe_reply(data, length, _probeNonce);
    if (!reply) {
        return;
    }
    // Pairing only ever starts at the saved address. At the others, only a PC
    // this device is paired with counts: another network can have another
    // PC at the same address.
    if (![socket.address isEqualToString:_savedAddress]) {
        link::Pairing pairing;
        NSString *hostName = reply->host_name.empty() ? socket.address : ToNSString(reply->host_name);
        if (![ILNBridge pairingForHostName:hostName into:pairing]) {
            return;
        }
    }
    if (socket.viaTailscale && [self probingDirectAddress]) {
        if (!_heldReply) {
            _heldReply = *reply;
            _heldReplySocket = socket;
            _heldReplyUntil = CFAbsoluteTimeGetCurrent() + kPreferDirectFor;
        }
        return;
    }
    [self useProbeReply:*reply from:socket];
}

/// Go ahead with the PC that answered at @p socket's address.
- (void)useProbeReply:(const link::ProbeReply &)reply from:(ILNLinkSocket *)socket
{
    _hostName = ToNSString(reply.host_name);
    if (_hostName.length == 0) {
        _hostName = socket.address;
    }
    NSString *hostVersion = reply.software_version.empty() ? @"an older version" : ToNSString(reply.software_version);
    NSString *problem = nil;
    switch (link::check_compatibility(reply)) {
        case link::Compatibility::kUpdateHost:
            problem = [NSString stringWithFormat:@"%@ runs InputLine %@, which is older than this app. Install the latest InputLine on the PC.", _hostName, hostVersion];
            break;
        case link::Compatibility::kUpdateClient:
            problem = [NSString stringWithFormat:@"%@ runs InputLine %@, which needs a newer version of this app. Update the app.", _hostName, hostVersion];
            break;
        case link::Compatibility::kCompatible:
            break;
    }
    if (problem != nil) {
        if (![problem isEqualToString:_versionProblem]) {
            [self logEvent:problem];
        }
        _versionProblem = problem;
        _state = ILNLinkStateNotFound;  // keep probing: it connects once updated
        return;
    }
    _versionProblem = nil;
    [self noteVersionOfPC:reply.software_version];
    [self logEvent:[NSString stringWithFormat:@"Link: %@ answered at %@ after %.1f s", _hostName, socket.address,
                                              CFAbsoluteTimeGetCurrent() - _stateEnteredAt]];

    // From now on, talk to the PC at this address only.
    [_probes removeObjectForKey:socket.address];
    [self stopProbing];
    [self closeLink];
    _link = socket;
    _address = socket.address;
    link::Pairing pairing;
    if ([ILNBridge pairingForHostName:_hostName into:pairing]) {
        [self startSessionWithPairing:pairing];
    } else {
        [self beginPairing];
    }
}

/// Something suggests the PC may be reachable now (a controller just
/// connected, the app came to the front): try it at once, not at the next retry.
- (void)kickLink
{
    switch (_state) {
        case ILNLinkStateNotFound:
            [self startProbing];  // fresh sockets: the network may have changed meanwhile
            break;
        case ILNLinkStateSearching:
        case ILNLinkStateConnecting:
            _lastSend = 0;
            break;
        default:
            break;
    }
}

#pragma mark - Network changes (link queue)

- (void)watchNetwork
{
    _pathMonitor = nw_path_monitor_create();
    nw_path_monitor_set_queue(_pathMonitor, _queue);
    __weak ILNBridge *weakSelf = self;
    nw_path_monitor_set_update_handler(_pathMonitor, ^(nw_path_t path) {
        [weakSelf networkDidChange:path];
    });
    nw_path_monitor_start(_pathMonitor);
}

/// The first call describes the network InputLine started on.
- (void)networkDidChange:(nw_path_t)path
{
    NSString *network = NetworkDescription(path);
    if (_network == nil) {
        [self logEvent:[NSString stringWithFormat:@"Network: %@", network]];
    } else {
        [self logEvent:[NSString stringWithFormat:@"Network changed: %@", network]];
        _networkChangedAt = CFAbsoluteTimeGetCurrent();  // service handles it once it has settled
    }
    _network = network;
}

/// The device moved to another network (Wi-Fi to cellular, a VPN on or off):
/// sockets may be stale, and the PC may answer at another address now.
- (void)handleNetworkChange
{
    switch (_state) {
        case ILNLinkStateSearching:
        case ILNLinkStateNotFound:
        case ILNLinkStateConnecting:
            [self enterState:ILNLinkStateSearching];
            break;
        case ILNLinkStatePairing:
            [self reopenLink];
            break;
        case ILNLinkStateConnected:
            // Keep the session if the PC still answers at this address (it
            // follows this device's new one); otherwise look for it.
            [self reopenLink];
            _checkingLinkSince = CFAbsoluteTimeGetCurrent();
            _lastPing = 0;
            break;
        case ILNLinkStateNoPC:
        case ILNLinkStateDisconnected:
            break;
    }
}

#pragma mark - State machine (link queue)

- (void)enterState:(ILNLinkState)state
{
    if (state != _state) {
        static NSString *const names[] = {@"no PC", @"searching", @"not found", @"pairing", @"connecting", @"connected", @"disconnected"};
        [self logEvent:[NSString stringWithFormat:@"Link: %@", names[state]]];
    }
    if (state == ILNLinkStateConnected) {
        _linkDownSince = 0;
    } else if (_state == ILNLinkStateConnected || _linkDownSince == 0) {
        _linkDownSince = CFAbsoluteTimeGetCurrent();
    }
    _state = state;
    _stateEnteredAt = CFAbsoluteTimeGetCurrent();
    _lastSend = 0;
    _checkingLinkSince = 0;
    if (state != ILNLinkStateSearching) {
        [self stopProbing];
    }
    switch (state) {
        case ILNLinkStateSearching:
            _probeNonce = RandomNonce();
            [self startProbing];
            break;
        case ILNLinkStateConnecting:
            _helloAttempts = 0;
            break;
        case ILNLinkStateConnected:
            _everConnected = YES;
            _lastPong = CFAbsoluteTimeGetCurrent();
            for (ILNController *controller in _controllers.allValues) {
                controller.attached = NO;
                controller.lastAttachSent = 0;
            }
            break;
        default:
            break;
    }
}

/// Periodic work. Runs from the timer and from every Bluetooth report,
/// because timers are unreliable while the app is in the background.
- (void)service
{
    const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    if (now - _lastService < kTickInterval * 0.5) {
        return;
    }
    if (_lastService > 0 && now - _lastService > kResumeAfterSilence && _state == ILNLinkStateConnected) {
        // iOS pauses InputLine in the background while no controller is on,
        // and the PC has since ended the session: start a new one now.
        [self logEvent:[NSString stringWithFormat:@"Awake again after %.0f s paused by iOS: reconnecting", now - _lastService]];
        _lastService = now;
        [self enterState:ILNLinkStateConnecting];
    }
    _lastService = now;
    if (_networkChangedAt > 0 && now - _networkChangedAt >= kNetworkSettle) {
        _networkChangedAt = 0;
        [self handleNetworkChange];
    }
    const CFAbsoluteTime inState = now - _stateEnteredAt;
    if (now - _lastBatteryRead >= kBatteryReadInterval) {
        _lastBatteryRead = now;
        for (ILNController *controller in _controllers.allValues) {
            [controller.device readBattery];
        }
    }

    switch (_state) {
        case ILNLinkStateNoPC:
        case ILNLinkStateDisconnected:
            break;

        case ILNLinkStateSearching:
        case ILNLinkStateNotFound:
            if (_heldReply && now >= _heldReplyUntil) {
                // Only Tailscale answered: go with it.
                const link::ProbeReply reply = *_heldReply;
                ILNLinkSocket *socket = _heldReplySocket;
                _heldReply.reset();
                _heldReplySocket = nil;
                [self useProbeReply:reply from:socket];
                break;
            }
            if (_state == ILNLinkStateSearching && inState > kNotFoundAfter) {
                _state = ILNLinkStateNotFound;  // keep the nonce; keep probing
                [self logEvent:[NSString stringWithFormat:@"Link: no answer from %@ yet", [[self candidateAddresses] componentsJoinedByString:@", "]]];
            }
            if (_state == ILNLinkStateNotFound && now >= _reopenProbesAt) {
                [self startProbing];
            }
            if (now - _lastSend >= (_state == ILNLinkStateNotFound ? 2.0 : kProbeRetry)) {
                _lastSend = now;
                [self sendProbes];
            }
            break;

        case ILNLinkStatePairing:
            if (now - _lastSend >= kPairRetry) {
                _lastSend = now;
                if (_pendingPairRequest.empty()) {
                    [self sendDatagram:link::ClientSession::make_pair_start(_pairingAttempt, ToStdString([self clientName]))];
                } else {
                    [self sendDatagram:_pendingPairRequest];
                }
            }
            break;

        case ILNLinkStateConnecting:
            if (now - _lastSend >= kHelloRetry) {
                if (_helloAttempts >= kMaxHelloAttempts) {
                    if (!_everConnected) {
                        // The PC answers probes but ignores our pairing: it no
                        // longer knows this device. Pair again.
                        [self beginPairing];
                    } else {
                        [self enterState:ILNLinkStateSearching];
                    }
                    break;
                }
                ++_helloAttempts;
                _lastSend = now;
                [self sendDatagram:_session->make_hello(WallClockMicroseconds())];
            }
            break;

        case ILNLinkStateConnected:
            if (_checkingLinkSince > 0 && _lastPong >= _checkingLinkSince) {
                _checkingLinkSince = 0;  // still there after the network changed
            }
            if (_checkingLinkSince > 0 && now - _checkingLinkSince > kNetworkCheckTimeout) {
                [self logEvent:@"Link: no answer from the PC on this network; looking for it"];
                [self enterState:ILNLinkStateSearching];
                break;
            }
            if (now - _lastPong > kLinkLostAfter) {
                [self logEvent:@"Link: no answer from the PC for 4 s, reconnecting"];
                [self enterState:ILNLinkStateConnecting];
                break;
            }
            if (now - _lastPing >= kPingInterval) {
                _lastPing = now;
                [self sendDatagram:_session->make_ping(WallClockMicroseconds())];
            }
            for (ILNController *controller in _controllers.allValues) {
                if (!self->_paused && !controller.attached && controller.identified && now - controller.lastAttachSent >= kAttachRetry) {
                    [self sendAttach:controller];
                }
            }
            break;
    }

    // While the controller goes to the PC, keep it out of its built-in
    // keyboard/mouse mode (it would also move this device's pointer). Otherwise
    // hand that mode back.
    const BOOL toPC = !_paused && _state != ILNLinkStateDisconnected &&
                      (_state == ILNLinkStateConnected || (_linkDownSince > 0 && now - _linkDownSince < kMouseModeAfterLinkDown));
    for (ILNController *controller in _controllers.allValues) {
        if (toPC) {
            if (controller.mouseModeOn || now - controller.lastLizardSent >= kLizardInterval) {
                controller.lastLizardSent = now;
                controller.mouseModeOn = NO;
                [controller.device sendFeatureReport:SettingReport(kSettingLizardMode, 0)];
            }
        } else if (!controller.mouseModeOn) {
            controller.mouseModeOn = YES;
            controller.keptSettings = NO;
            [controller.device sendFeatureReport:SettingReport(kSettingLizardMode, 1)];
            [self logEvent:[NSString stringWithFormat:@"%@ works as this device's mouse again", controller.device.name]];
        }
    }

    if (now - _liveStarted >= kLiveTimingWindow) {
        _liveStarted = now;
        const auto live = _timingLive.summary();
        _liveSummary = live.reports > 1 ? ToNSString(live.to_string()) : @"";
        _timingLive.reset();
    }
}

- (void)handleDatagram:(const std::uint8_t *)data length:(size_t)length from:(ILNLinkSocket *)socket
{
    if (_state == ILNLinkStateSearching || _state == ILNLinkStateNotFound) {
        [self handleProbeReply:data length:length from:socket];
        return;
    }
    if (socket != _link) {
        return;  // a probe socket on its way out
    }
    switch (_state) {
        case ILNLinkStateSearching:
        case ILNLinkStateNotFound:
            return;

        case ILNLinkStatePairing: {
            if (_pendingPairRequest.empty()) {
                const auto reply = link::ClientSession::parse_probe_reply(data, length, _pairingAttempt.nonce);
                if (reply && reply->pairing_open) {
                    _hostPairingKey = reply->pairing_public_key;
                    if (_enteredCode != nil && _hostPairingKey != link::Key {}) {
                        NSString *code = _enteredCode;
                        _enteredCode = nil;
                        [self useCode:code];
                    }
                }
                if (reply && !_codeRequested) {
                    _codeRequested = YES;
                    NSString *message = reply->pairing_open
                        ? [NSString stringWithFormat:@"%@ is showing a 6-digit code on its screen. Not at the PC? Open your streaming app to see its screen, then come back here and enter the code.", _hostName]
                        : [NSString stringWithFormat:@"%@ is not accepting new devices. Run 'inputline-host pair' on it, then enter the code it shows.", _hostName];
                    [self askForCode:message];
                }
                return;
            }
            const auto result = link::ClientSession::parse_pair_result(data, length, _pairingAttempt);
            if (!result) {
                return;
            }
            if (*result) {
                const link::Pairing pairing = _pairingAttempt.pairing();
                [ILNBridge savePairing:pairing forHostName:_hostName];
                [self tellUser:[NSString stringWithFormat:@"Paired with %@.", _hostName]];
                [self startSessionWithPairing:pairing];
            } else {
                // The PC discards its side of an attempt after a wrong code:
                // start a new one (a new PairStart) while the user types again.
                [self logEvent:@"Pairing: the code did not match"];
                _pairingAttempt = link::ClientSession::begin_pairing(SecureRandom);
                _hostPairingKey = link::Key {};
                _pendingPairRequest.clear();
                _lastSend = 0;
                [self askForCode:@"That code did not match. Enter the code shown on the PC's screen."];
            }
            return;
        }

        case ILNLinkStateConnecting:
        case ILNLinkStateConnected: {
            if (!_session) {
                return;
            }
            const auto event = _session->handle(data, length);
            if (event) {
                [self handleSessionEvent:*event];
            }
            return;
        }

        case ILNLinkStateNoPC:
        case ILNLinkStateDisconnected:
            return;
    }
}

/// The PC and this app can talk; if one runs an older version, say which to
/// update. Link queue.
- (void)noteVersionOfPC:(const std::string &)hostVersion
{
    const std::string appVersion = ToStdString(_appVersion);
    NSString *note = nil;
    if (inputline::is_version(hostVersion) && inputline::is_version(appVersion)) {
        const int order = inputline::compare_versions(hostVersion, appVersion);
        if (order < 0) {
            note = [NSString stringWithFormat:@"%@ runs InputLine %@, older than this app (%@). Install the latest InputLine on the PC when you can.",
                                              _hostName, ToNSString(hostVersion), _appVersion];
        } else if (order > 0) {
            note = [NSString stringWithFormat:@"%@ runs InputLine %@, newer than this app (%@). Update this app when you can.",
                                              _hostName, ToNSString(hostVersion), _appVersion];
        }
    }
    if (note != nil && ![note isEqualToString:_updateNote]) {
        [self logEvent:note];
    }
    _updateNote = note;
}

- (void)startSessionWithPairing:(const link::Pairing &)pairing
{
    _session = std::make_unique<link::ClientSession>(pairing, ToStdString([self clientName]), SecureRandom, ToStdString(_appVersion));
    [self enterState:ILNLinkStateConnecting];
}

- (void)handleSessionEvent:(const link::ClientSession::Event &)event
{
    using EventType = link::ClientSession::EventType;
    switch (event.type) {
        case EventType::kHelloAck:
            if (_state != ILNLinkStateConnected) {
                [self enterState:ILNLinkStateConnected];
                [self rememberWorkingAddress];
            }
            break;

        case EventType::kAttachAck:
            for (ILNController *controller in _controllers.allValues) {
                if (controller.linkIndex != event.attach_ack.controller) {
                    continue;
                }
                const BOOL wasAttached = controller.attached;
                controller.attached = event.attach_ack.status == link::AttachStatus::kOk;
                if (controller.attached) {
                    controller.keptSettings = YES;
                    [self sendBattery:controller];  // Steam shows it straight away
                }
                if (!controller.attached && event.attach_ack.status == link::AttachStatus::kBackendUnavailable && !wasAttached) {
                    [self tellUser:@"The PC could not create the virtual controller. Is usbip-win2 installed?"];
                }
            }
            break;

        case EventType::kNeedAttach:
            for (ILNController *controller in _controllers.allValues) {
                if (controller.linkIndex == event.need_attach.controller) {
                    controller.attached = NO;
                    controller.lastAttachSent = 0;
                }
            }
            break;

        case EventType::kPong: {
            _lastPong = CFAbsoluteTimeGetCurrent();
            // A ping sent just before iOS paused the app comes back much
            // later; that's not the network's round trip.
            const double rtt = (double)(WallClockMicroseconds() - event.pong.client_time_us) / 1000.0;
            if (rtt >= 0 && rtt < kLinkLostAfter * 1000.0) {
                _rttMs = rtt;
            }
            break;
        }

        case EventType::kHidOutput:
            for (ILNController *controller in _controllers.allValues) {
                if (controller.linkIndex != event.output.controller) {
                    continue;
                }
                NSData *report = [NSData dataWithBytes:event.output.report.data() length:event.output.report.size()];
                if (event.output.kind == link::OutputKind::kOutputReport) {
                    [controller.device sendOutputReport:report];
                } else {
                    [self logSettingsFromPC:event.output.report];
                    [controller.device sendFeatureReport:report];
                }
            }
            break;
    }
}

/// Settings Steam sends reach the controller through us: note each one, so
/// a disconnect right after one of them shows up in the event log.
- (void)logSettingsFromPC:(const std::vector<std::uint8_t> &)report
{
    if (report.size() < 2) {
        return;
    }
    if (report[1] == 0x87 && report.size() > 2) {
        NSMutableArray<NSString *> *settings = [NSMutableArray array];
        for (size_t i = 3; i + 3 <= report.size() && i < 3u + report[2]; i += 3) {
            [settings addObject:[NSString stringWithFormat:@"%u=%u", report[i], report[i + 1] | (report[i + 2] << 8)]];
        }
        [self logEvent:[NSString stringWithFormat:@"PC set controller settings %@", [settings componentsJoinedByString:@", "]]];
    } else {
        [self logEvent:[NSString stringWithFormat:@"PC sent controller command 0x%02x", report[1]]];
    }
}

- (void)sendAttach:(ILNController *)controller
{
    link::Attach attach;
    attach.controller = controller.linkIndex;
    attach.transport = link::Transport::kBluetoothLe;
    attach.flags = controller.keptSettings ? link::kAttachKeptSettings : 0;
    const char *serial = controller.serial.UTF8String;
    if (serial != NULL) {
        strncpy(attach.unit_serial.data(), serial, attach.unit_serial.size() - 1);
    }
    if (controller.attributes.length >= 3) {
        memcpy(attach.attributes_reply.data(), controller.attributes.bytes, MIN(controller.attributes.length, attach.attributes_reply.size()));
    }
    controller.lastAttachSent = CFAbsoluteTimeGetCurrent();
    [self sendDatagram:_session->make_attach(attach)];
}

- (void)setPaused:(BOOL)paused
{
    dispatch_async(_queue, ^{
        if (self->_paused == paused) {
            return;
        }
        self->_paused = paused;
        if (paused) {
            if (self->_state == ILNLinkStateConnected && self->_session) {
                [self detachAll];
            }
            for (ILNController *controller in self->_controllers.allValues) {
                controller.attached = NO;
                controller.keptSettings = NO;
            }
            [self logEvent:@"Disconnected: the controller works with this device until you tap Connect to PC or switch it off and on"];
        } else {
            [self logEvent:@"Connected: sending the controller to the PC again"];
        }
        [self service];
    });
}

- (void)prepareForTermination
{
    // Best effort: iOS gives little time, and nothing at all when it ends a
    // suspended app. Switching the controller off and on restores it too.
    dispatch_sync(_queue, ^{
        for (ILNController *controller in self->_controllers.allValues) {
            [controller.device sendFeatureReport:SettingReport(kSettingLizardMode, 1)];
        }
    });
}

- (void)detachAll
{
    for (ILNController *controller in _controllers.allValues) {
        [self sendDatagram:_session->make_detach(controller.linkIndex)];
    }
}

#pragma mark - Pairing (the PC shows the code, the user types it here)

- (NSString *)clientName
{
    return _clientName;
}

- (void)beginPairing
{
    _pairingAttempt = link::ClientSession::begin_pairing(SecureRandom);
    _hostPairingKey = link::Key {};
    _enteredCode = nil;
    _pendingPairRequest.clear();
    _codeRequested = NO;
    _session.reset();
    [self enterState:ILNLinkStatePairing];
}

- (void)askForCode:(NSString *)message
{
    dispatch_async(dispatch_get_main_queue(), ^{
        [self.delegate bridge:self needsPairingCodeWithMessage:message];
    });
}

- (void)submitPairingCode:(NSString *)text
{
    dispatch_async(_queue, ^{
        if (self->_state != ILNLinkStatePairing) {
            return;
        }
        NSMutableString *digits = [NSMutableString string];
        for (NSUInteger i = 0; i < text.length; ++i) {
            const unichar c = [text characterAtIndex:i];
            if (c >= '0' && c <= '9') {
                [digits appendFormat:@"%C", c];
            }
        }
        if (digits.length != link::kPinDigits) {
            [self askForCode:[NSString stringWithFormat:@"The code has %d digits. Enter the code shown on the PC's screen.", (int)link::kPinDigits]];
            return;
        }
        if (self->_hostPairingKey == link::Key {}) {
            self->_enteredCode = [digits copy];  // used as soon as the PC's share arrives
            return;
        }
        [self useCode:digits];
    });
}

/// Run the pairing exchange with the code the user typed. Link queue.
- (void)useCode:(NSString *)digits
{
    if (!link::ClientSession::enter_pin(_pairingAttempt, _hostPairingKey, ToStdString(digits), ToStdString([self clientName]))) {
        [self askForCode:[NSString stringWithFormat:@"%@ isn't accepting new devices right now. Tap Connect to try again.", _hostName]];
        return;
    }
    _pendingPairRequest = link::ClientSession::make_pair_request(_pairingAttempt, ToStdString([self clientName]));
    _lastSend = 0;
}

- (void)cancelPairing
{
    dispatch_async(_queue, ^{
        if (self->_state == ILNLinkStatePairing) {
            // Try again later by tapping Connect.
            [self closeLink];
            [self enterState:ILNLinkStateNoPC];  // Connect tries again
        }
    });
}

#pragma mark - Controllers (link queue)

- (std::uint8_t)freeLinkIndex
{
    for (std::uint8_t index = 0; index < kMaxControllers; ++index) {
        BOOL used = NO;
        for (ILNController *controller in _controllers.allValues) {
            used |= controller.linkIndex == index;
        }
        if (!used) {
            return index;
        }
    }
    return kMaxControllers;
}

- (void)rememberController:(NSUUID *)identifier
{
    NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
    NSMutableArray<NSString *> *known = [[defaults stringArrayForKey:kControllersDefaultsKey] mutableCopy] ?: [NSMutableArray array];
    if (![known containsObject:identifier.UUIDString]) {
        [known addObject:identifier.UUIDString];
        while (known.count > 8) {
            [known removeObjectAtIndex:0];
        }
        [defaults setObject:known forKey:kControllersDefaultsKey];
    }
}

/// What this controller said about itself last time, or nil.
- (nullable NSDictionary *)rememberedIdentityOf:(NSUUID *)identifier
{
    id entry = [[NSUserDefaults standardUserDefaults] dictionaryForKey:kIdentitiesDefaultsKey][identifier.UUIDString];
    if (![entry isKindOfClass:[NSDictionary class]] || ![entry[@"serial"] isKindOfClass:[NSString class]] ||
        ![entry[@"attributes"] isKindOfClass:[NSData class]]) {
        return nil;
    }
    return entry;
}

- (void)rememberIdentity:(ILNController *)controller
{
    NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
    NSMutableDictionary *identities = [NSMutableDictionary dictionary];
    // Only for the controllers InputLine remembers (rememberController:).
    NSDictionary *old = [defaults dictionaryForKey:kIdentitiesDefaultsKey];
    for (NSString *known in [defaults stringArrayForKey:kControllersDefaultsKey]) {
        if (old[known] != nil) {
            identities[known] = old[known];
        }
    }
    identities[controller.device.identifier.UUIDString] = @{@"serial": controller.serial, @"attributes": controller.attributes};
    [defaults setObject:identities forKey:kIdentitiesDefaultsKey];
}

/// Ask the controller who it is: its unit serial, which Steam keys its
/// settings by and the PC uses to recognise it after a drop, and its firmware
/// version, which Steam compares with its own. It isn't plugged in on the PC
/// until then. A controller that just woke up can take a few seconds to
/// answer; if it doesn't, what it said last time is used, so Steam doesn't
/// see another controller with old firmware. Link queue.
- (void)identify:(ILNController *)controller
{
    controller.identifyStarted = CFAbsoluteTimeGetCurrent();
    controller.rememberedIdentity = [self rememberedIdentityOf:controller.device.identifier];
    [self identifyStep:controller];
}

- (void)identifyStep:(ILNController *)controller
{
    if (controller.identified || _controllers[controller.device.identifier] != controller) {
        return;  // done, or the controller went away
    }
    const NSTimeInterval limit = controller.rememberedIdentity != nil || controller.serial != nil ? kIdentifyForKnown : kIdentifyFor;
    if ((controller.serial != nil && controller.attributes != nil) || CFAbsoluteTimeGetCurrent() - controller.identifyStarted >= limit) {
        [self finishIdentify:controller];
        return;
    }
    const BOOL askSerial = controller.serial == nil;
    NSData *request = askSerial ? [NSData dataWithBytes:kSerialRequest length:sizeof(kSerialRequest)]
                                : [NSData dataWithBytes:kAttributesRequest length:sizeof(kAttributesRequest)];
    __weak ILNController *weakController = controller;
    [controller.device queryFeatureReport:request completion:^(NSData *reply) {
        dispatch_async(self->_queue, ^{
            ILNController *strong = weakController;
            if (strong == nil) {
                return;
            }
            NSString *serial = askSerial ? SerialFromReply(reply) : nil;
            NSData *attributes = askSerial ? nil : AttributesFromReply(reply);
            if (serial != nil || attributes != nil) {
                if (serial != nil) {
                    strong.serial = serial;
                } else {
                    strong.attributes = attributes;
                }
                [self identifyStep:strong];
                return;
            }
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(kIdentifyRetry * NSEC_PER_SEC)), self->_queue, ^{
                ILNController *again = weakController;
                if (again != nil) {
                    [self identifyStep:again];
                }
            });
        });
    }];
}

/// Plug it in with what it said, filled in from last time if it didn't say everything. Link queue.
- (void)finishIdentify:(ILNController *)controller
{
    NSDictionary *remembered = controller.rememberedIdentity;
    NSString *name = controller.device.name;
    if (controller.serial != nil && controller.attributes != nil) {
        [self rememberIdentity:controller];
    } else if (remembered != nil && (controller.serial == nil || [controller.serial isEqualToString:remembered[@"serial"]])) {
        [self logEvent:[NSString stringWithFormat:@"%@ didn't say %@ in time: using what it said last time", name,
                                                  controller.serial == nil ? @"who it is" : @"its firmware version"]];
        controller.serial = controller.serial ?: remembered[@"serial"];
        controller.attributes = controller.attributes ?: remembered[@"attributes"];
    } else {
        [self logEvent:[NSString stringWithFormat:@"%@ didn't say %@ in time: Steam may see it as another controller", name,
                                                  controller.serial == nil ? @"who it is" : @"its firmware version"]];
    }
    controller.identified = YES;
}

#pragma mark - ILNTritonBLEDelegate (Bluetooth queue)

- (void)tritonDidBecomeReady:(ILNTritonDevice *)device
{
    dispatch_async(dispatch_get_main_queue(), ^{
        [self endSetupTask];
    });
    dispatch_async(_queue, ^{
        if (self->_controllers[device.identifier] != nil) {
            return;
        }
        const std::uint8_t index = [self freeLinkIndex];
        if (index >= kMaxControllers) {
            return;
        }
        ILNController *controller = [[ILNController alloc] init];
        controller.device = device;
        controller.linkIndex = index;
        self->_controllers[device.identifier] = controller;
        [self logEvent:[NSString stringWithFormat:@"Controller connected: %@ (reports 0x%02x)", device.name, device.inputReportId]];
        [self rememberController:device.identifier];
        if (self->_paused) {
            // Switching the controller back on after Disconnect: to the PC again.
            self->_paused = NO;
            [self logEvent:@"Controller reconnected: sending it to the PC again"];
        }

        // Mouse mode is on after power-up; the next service tick decides.
        controller.mouseModeOn = YES;
        [self identify:controller];
        [controller.device readBattery];  // a report sent during setup arrived before this
        [self kickLink];
        [self service];
    });
}

- (void)tritonDidDisconnect:(ILNTritonDevice *)device
{
    dispatch_async(_queue, ^{
        ILNController *controller = self->_controllers[device.identifier];
        if (controller == nil) {
            return;
        }
        NSError *error = device.lastDisconnectError;
        [self logEvent:[NSString stringWithFormat:@"Controller disconnected: %@", error != nil
                            ? [NSString stringWithFormat:@"%@ (CoreBluetooth error %ld)", error.localizedDescription, (long)error.code]
                            : @"no reason given"]];
        if (self->_state == ILNLinkStateConnected && self->_session) {
            // The PC unplugs its virtual controller, as for a real one.
            [self sendDatagram:self->_session->make_detach(controller.linkIndex)];
        }
        [self->_controllers removeObjectForKey:device.identifier];
    });
}

// Setting a controller up takes a few Bluetooth round trips. When it connects
// while InputLine is in the background (or the device is locked), ask iOS for
// the time to finish; once it streams, its reports keep InputLine running.
- (void)tritonWillSetUp:(ILNTritonDevice *)device
{
    dispatch_async(dispatch_get_main_queue(), ^{
        if (self->_setupTask != UIBackgroundTaskInvalid) {
            return;
        }
        self->_setupTask = [[UIApplication sharedApplication] beginBackgroundTaskWithName:@"Controller setup" expirationHandler:^{
            [self endSetupTask];
        }];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(25 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            [self endSetupTask];
        });
    });
}

- (void)endSetupTask
{
    if (_setupTask != UIBackgroundTaskInvalid) {
        [[UIApplication sharedApplication] endBackgroundTask:_setupTask];
        _setupTask = UIBackgroundTaskInvalid;
    }
}

- (void)triton:(ILNTritonDevice *)device didReceiveBatteryReport:(NSData *)report
{
    dispatch_async(_queue, ^{
        ILNController *controller = self->_controllers[device.identifier];
        if (controller == nil) {
            return;  // not set up yet; it is read again once it is
        }
        if (controller.batteryReport == nil) {
            [self logEvent:[NSString stringWithFormat:@"%@: battery %ld%%, %@", device.name, (long)device.batteryLevel,
                                                      device.batteryFromReport ? @"from the controller's battery report"
                                                                               : @"from the standard battery level (no charging state)"]];
        }
        controller.batteryReport = report;
        [self sendBattery:controller];
    });
}

/// The controller's battery to the PC, which passes it to Steam. Link queue.
- (void)sendBattery:(ILNController *)controller
{
    if (controller.batteryReport == nil || !controller.attached || self->_paused || self->_state != ILNLinkStateConnected || !self->_session) {
        return;
    }
    NSData *report = controller.batteryReport;
    [self sendDatagram:self->_session->make_input(controller.linkIndex, (const std::uint8_t *)report.bytes, report.length)];
}

- (void)tritonLog:(NSString *)message
{
    dispatch_async(_queue, ^{
        [self logEvent:message];
    });
}

- (void)tritonBluetoothUnauthorized
{
    dispatch_async(_queue, ^{
        [self tellUser:@"InputLine needs Bluetooth. Allow it in Settings > InputLine."];
    });
}

- (void)triton:(ILNTritonDevice *)device didReceiveReport:(const uint8_t *)report length:(size_t)length
{
    const std::uint64_t arrived = MonotonicMicroseconds();
    NSData *copy = [NSData dataWithBytes:report length:length];
    dispatch_async(_queue, ^{
        (self->_inBackground ? self->_timingBackground : self->_timingForeground).add(arrived);
        self->_timingLive.add(arrived);

        ILNController *controller = self->_controllers[device.identifier];
        const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
        if (self->_state == ILNLinkStateConnected && now - self->_lastDatagramSent > kResumeAfterSilence) {
            [self logEvent:[NSString stringWithFormat:@"Resuming after %.1f s of silence: starting a new session", now - self->_lastDatagramSent]];
            [self enterState:ILNLinkStateConnecting];
        }
        if (controller != nil && self->_state == ILNLinkStateConnected && controller.attached && !self->_paused) {
            controller.sequence += 1;
            NSData *previous = controller.previousReport;
            [self sendDatagram:self->_session->make_input_bundle(controller.linkIndex, controller.sequence, (const std::uint8_t *)copy.bytes, copy.length,
                                                                (const std::uint8_t *)previous.bytes, previous.length)];
            controller.previousReport = copy;
            self->_timingSent.add(MonotonicMicroseconds());
        }
        [self service];
    });
}

@end
