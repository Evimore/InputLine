//
//  ILNTritonBLE.m
//  InputLine
//
//  GATT layout and pairing behaviour follow SDL's src/hidapi/ios/hid.m
//  (Copyright Valve Corporation, zlib license).
//

#import "ILNTritonBLE.h"

#import <CoreBluetooth/CoreBluetooth.h>

#include <string.h>

static NSString *const kValveService = @"100F6C32-1735-4313-B402-38567131E5F3";
static NSString *const kReportCharacteristic = @"100F6C34-1735-4313-B402-38567131E5F3";
static NSString *const kInput45Characteristic = @"100F6C7A-1735-4313-B402-38567131E5F3";
static NSString *const kInput47Characteristic = @"100F6C7C-1735-4313-B402-38567131E5F3";
static NSString *const kValveUUIDSuffix = @"-1735-4313-B402-38567131E5F3";
static NSString *const kDeviceInformationService = @"180A";
// Battery: Valve's own report 0x43 (input report N lives at 100F6C(N + 0x35)),
// or the standard Battery Service's level when there is no such characteristic.
static NSString *const kBatteryReportCharacteristic = @"100F6C78-1735-4313-B402-38567131E5F3";
static NSString *const kBatteryService = @"180F";
static NSString *const kBatteryLevelCharacteristic = @"2A19";
static const uint8_t kBatteryReportId = 0x43;
static const size_t kBatteryReportPayload = 14;  // TritonBatteryStatus_t
// EChargeState
static const uint8_t kChargeStateDischarging = 1;
static const uint8_t kChargeStateCharging = 2;
static const uint8_t kChargeStateSourceCheck = 3;  // checking the power source it was just plugged into
static const uint8_t kChargeStateFull = 4;

static const size_t kStateReportPayload = 45;
static const NSTimeInterval kNotifyRetryInterval = 1.0;
// How often to look for paired controllers that iOS has reconnected, so a
// controller switched on mid-stream is picked up.
static const NSTimeInterval kKnownControllerPollInterval = 2.0;
// How often to check on controllers that are being set up.
static const NSTimeInterval kSetupCheckInterval = 0.5;
// A connected controller that isn't streaming after this long gets its setup
// restarted, and after a few tries a fresh connection. Without this, a setup
// interrupted (for example while the device was locked) stays stuck until the
// controller is switched off and on. A new controller gets longer: the user
// may still be answering the pairing prompt.
static const NSTimeInterval kSetupTimeout = 6.0;
static const NSUInteger kMaxSetupRestarts = 2;
// This app's own controllers are paired already and stream within a second.
static const NSTimeInterval kKnownSetupTimeout = 2.0;
static const NSUInteger kKnownMaxSetupRestarts = 1;

/// Tell the delegate something worth a line in its event log.
static void TritonLog(id<ILNTritonBLEDelegate> delegate, NSString *message)
{
    if ([delegate respondsToSelector:@selector(tritonLog:)]) {
        [delegate tritonLog:message];
    }
}

#pragma mark - ILNTritonDevice

@interface ILNTritonDevice () <CBPeripheralDelegate>

@property (nonatomic, strong) CBPeripheral *peripheral;
@property (nonatomic, strong) dispatch_queue_t queue;
@property (nonatomic, weak) id<ILNTritonBLEDelegate> delegate;
@property (nonatomic, strong, nullable) CBCharacteristic *inputCharacteristic;
@property (nonatomic, strong, nullable) CBCharacteristic *reportCharacteristic;
@property (nonatomic, strong) NSMutableDictionary<NSNumber *, CBCharacteristic *> *outputCharacteristics;
@property (nonatomic, assign) BOOL ready;
@property (nonatomic, assign) uint8_t inputReportId;
@property (nonatomic, strong, nullable) dispatch_source_t notifyRetryTimer;
@property (nonatomic, copy, nullable) void (^pendingFeatureRead)(NSData *_Nullable);
@property (nonatomic, strong, nullable) NSError *lastDisconnectError;
@property (nonatomic, strong, nullable) CBCharacteristic *batteryReportCharacteristic;
@property (nonatomic, strong, nullable) CBCharacteristic *batteryLevelCharacteristic;
@property (nonatomic, assign) BOOL batteryFromReport;  // Valve's report seen: ignore the plain level
@property (nonatomic, assign) NSInteger batteryLevel;
@property (nonatomic, assign) BOOL batteryCharging;
@property (nonatomic, assign) CFAbsoluteTime setupStartedAt;  // 0 while not connected
@property (nonatomic, assign) NSUInteger setupRestarts;
@property (nonatomic, copy, nullable) NSString *notifyResult;  // the last one logged during this setup

- (void)didConnect;
- (void)didDisconnect;
- (void)restartSetup;

@end

@implementation ILNTritonDevice

- (instancetype)initWithPeripheral:(CBPeripheral *)peripheral queue:(dispatch_queue_t)queue delegate:(id<ILNTritonBLEDelegate>)delegate
{
    if ((self = [super init])) {
        _peripheral = peripheral;
        _queue = queue;
        _delegate = delegate;
        _outputCharacteristics = [NSMutableDictionary dictionary];
        _batteryLevel = -1;
        peripheral.delegate = self;
    }
    return self;
}

- (NSUUID *)identifier
{
    return self.peripheral.identifier;
}

- (NSString *)name
{
    return self.peripheral.name ?: @"Steam Controller";
}

- (void)didConnect
{
    self.setupRestarts = 0;
    id<ILNTritonBLEDelegate> delegate = self.delegate;
    if ([delegate respondsToSelector:@selector(tritonWillSetUp:)]) {
        [delegate tritonWillSetUp:self];
    }
    [self startSetup];
}

- (void)startSetup
{
    [self cancelNotifyRetry];
    self.inputCharacteristic = nil;
    self.reportCharacteristic = nil;
    [self.outputCharacteristics removeAllObjects];
    self.batteryReportCharacteristic = nil;
    self.batteryLevelCharacteristic = nil;
    self.batteryFromReport = NO;
    self.notifyResult = nil;
    self.setupStartedAt = CFAbsoluteTimeGetCurrent();
    [self.peripheral discoverServices:@[[CBUUID UUIDWithString:kValveService], [CBUUID UUIDWithString:kBatteryService]]];
}

- (void)restartSetup
{
    self.setupRestarts += 1;
    [self startSetup];
}

- (void)didDisconnect
{
    [self cancelNotifyRetry];
    BOOL wasReady = self.ready;
    self.ready = NO;
    self.setupStartedAt = 0;
    self.inputCharacteristic = nil;
    self.reportCharacteristic = nil;
    [self.outputCharacteristics removeAllObjects];
    void (^pending)(NSData *) = self.pendingFeatureRead;
    self.pendingFeatureRead = nil;
    if (pending) {
        pending(nil);
    }
    if (wasReady) {
        [self.delegate tritonDidDisconnect:self];
    }
}

#pragma mark Writes

- (BOOL)sendOutputReport:(NSData *)report
{
    if (report.length < 2) {
        return NO;
    }
    const uint8_t reportId = ((const uint8_t *)report.bytes)[0];
    NSData *payload = [report subdataWithRange:NSMakeRange(1, report.length - 1)];
    dispatch_async(self.queue, ^{
        CBCharacteristic *characteristic = self.outputCharacteristics[@(reportId)];
        if (characteristic == nil || self.peripheral.state != CBPeripheralStateConnected) {
            return;
        }
        // Haptics are latency-sensitive: skip the acknowledgement when the
        // characteristic allows it.
        CBCharacteristicWriteType type = (characteristic.properties & CBCharacteristicPropertyWriteWithoutResponse)
            ? CBCharacteristicWriteWithoutResponse
            : CBCharacteristicWriteWithResponse;
        [self.peripheral writeValue:payload forCharacteristic:characteristic type:type];
    });
    return YES;
}

- (void)sendFeatureReport:(NSData *)report
{
    if (report.length < 2) {
        return;
    }
    NSData *payload = [report subdataWithRange:NSMakeRange(1, MIN(report.length - 1, (NSUInteger)64))];
    dispatch_async(self.queue, ^{
        if (self.reportCharacteristic == nil || self.peripheral.state != CBPeripheralStateConnected) {
            return;
        }
        [self.peripheral writeValue:payload forCharacteristic:self.reportCharacteristic type:CBCharacteristicWriteWithResponse];
    });
}

- (void)queryFeatureReport:(NSData *)request completion:(void (^)(NSData *_Nullable))completion
{
    dispatch_async(self.queue, ^{
        if (self.reportCharacteristic == nil || self.pendingFeatureRead != nil || request.length < 2) {
            completion(nil);
            return;
        }
        self.pendingFeatureRead = completion;
        NSData *payload = [request subdataWithRange:NSMakeRange(1, MIN(request.length - 1, (NSUInteger)64))];
        [self.peripheral writeValue:payload forCharacteristic:self.reportCharacteristic type:CBCharacteristicWriteWithResponse];
        [self.peripheral readValueForCharacteristic:self.reportCharacteristic];

        // Do not wait forever for a controller that never answers.
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(NSEC_PER_SEC / 2)), self.queue, ^{
            void (^pending)(NSData *) = self.pendingFeatureRead;
            if (pending == completion) {
                self.pendingFeatureRead = nil;
                pending(nil);
            }
        });
    });
}

#pragma mark Battery

- (void)readBattery
{
    dispatch_async(self.queue, ^{
        if (self.peripheral.state != CBPeripheralStateConnected) {
            return;
        }
        CBCharacteristic *characteristic = self.batteryReportCharacteristic ?: self.batteryLevelCharacteristic;
        if (characteristic != nil && (characteristic.properties & CBCharacteristicPropertyRead)) {
            [self.peripheral readValueForCharacteristic:characteristic];
        }
    });
}

- (void)watchBattery:(CBCharacteristic *)characteristic
{
    if (characteristic.properties & (CBCharacteristicPropertyNotify | CBCharacteristicPropertyIndicate)) {
        [self.peripheral setNotifyValue:YES forCharacteristic:characteristic];
    }
    if (characteristic.properties & CBCharacteristicPropertyRead) {
        [self.peripheral readValueForCharacteristic:characteristic];
    }
}

- (void)deliverBatteryReport:(const uint8_t *)payload
{
    uint8_t report[1 + kBatteryReportPayload];
    report[0] = kBatteryReportId;
    memcpy(report + 1, payload, kBatteryReportPayload);
    self.batteryLevel = payload[1];
    self.batteryCharging = payload[0] == kChargeStateCharging || payload[0] == kChargeStateSourceCheck || payload[0] == kChargeStateFull;
    id<ILNTritonBLEDelegate> delegate = self.delegate;
    if ([delegate respondsToSelector:@selector(triton:didReceiveBatteryReport:)]) {
        [delegate triton:self didReceiveBatteryReport:[NSData dataWithBytes:report length:sizeof(report)]];
    }
}

#pragma mark Notification retries

// Enabling notifications silently fails until the user accepts the system
// pairing prompt, and CoreBluetooth never says when that happens. Keep asking
// until the first input report arrives.
- (void)startNotifyRetry
{
    [self cancelNotifyRetry];
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, self.queue);
    dispatch_source_set_timer(timer, DISPATCH_TIME_NOW, (uint64_t)(kNotifyRetryInterval * NSEC_PER_SEC), NSEC_PER_SEC / 10);
    __weak ILNTritonDevice *weakSelf = self;
    dispatch_source_set_event_handler(timer, ^{
        ILNTritonDevice *strongSelf = weakSelf;
        if (strongSelf == nil || strongSelf.ready || strongSelf.inputCharacteristic == nil) {
            [strongSelf cancelNotifyRetry];
            return;
        }
        [strongSelf.peripheral setNotifyValue:YES forCharacteristic:strongSelf.inputCharacteristic];
    });
    self.notifyRetryTimer = timer;
    dispatch_resume(timer);
}

- (void)cancelNotifyRetry
{
    if (self.notifyRetryTimer != nil) {
        dispatch_source_cancel(self.notifyRetryTimer);
        self.notifyRetryTimer = nil;
    }
}

#pragma mark CBPeripheralDelegate

- (void)peripheral:(CBPeripheral *)peripheral didDiscoverServices:(NSError *)error
{
    if (error != nil) {
        TritonLog(self.delegate, [NSString stringWithFormat:@"Bluetooth: %@ didn't list its services (%@)", self.name, error.localizedDescription]);
    } else if ([peripheral.services indexOfObjectPassingTest:^BOOL(CBService *service, NSUInteger index, BOOL *stop) {
                   return [service.UUID isEqual:[CBUUID UUIDWithString:kValveService]];
               }] == NSNotFound) {
        TritonLog(self.delegate, [NSString stringWithFormat:@"Bluetooth: %@ didn't list the controller service", self.name]);
    }
    for (CBService *service in peripheral.services) {
        if ([service.UUID isEqual:[CBUUID UUIDWithString:kValveService]]) {
            [peripheral discoverCharacteristics:nil forService:service];
        } else if ([service.UUID isEqual:[CBUUID UUIDWithString:kBatteryService]]) {
            [peripheral discoverCharacteristics:@[[CBUUID UUIDWithString:kBatteryLevelCharacteristic]] forService:service];
        }
    }
}

- (void)peripheral:(CBPeripheral *)peripheral didDiscoverCharacteristicsForService:(CBService *)service error:(NSError *)error
{
    if ([service.UUID isEqual:[CBUUID UUIDWithString:kBatteryService]]) {
        for (CBCharacteristic *characteristic in service.characteristics) {
            if ([characteristic.UUID isEqual:[CBUUID UUIDWithString:kBatteryLevelCharacteristic]]) {
                self.batteryLevelCharacteristic = characteristic;
                [self watchBattery:characteristic];
            }
        }
        return;
    }
    if (![service.UUID isEqual:[CBUUID UUIDWithString:kValveService]]) {
        return;
    }

    CBCharacteristic *input45 = nil;
    CBCharacteristic *input47 = nil;
    for (CBCharacteristic *characteristic in service.characteristics) {
        NSString *uuid = characteristic.UUID.UUIDString.uppercaseString;
        if ([uuid isEqualToString:kInput45Characteristic]) {
            input45 = characteristic;
        } else if ([uuid isEqualToString:kInput47Characteristic]) {
            input47 = characteristic;
        } else if ([uuid isEqualToString:kReportCharacteristic]) {
            self.reportCharacteristic = characteristic;
        } else if ([uuid isEqualToString:kBatteryReportCharacteristic]) {
            self.batteryReportCharacteristic = characteristic;
            [self watchBattery:characteristic];
        } else if ([uuid hasPrefix:@"100F6C"] && [uuid hasSuffix:kValveUUIDSuffix] && uuid.length >= 8) {
            // Output report N lives at 100F6C(N + 0x35).
            unsigned int value = 0;
            NSScanner *scanner = [NSScanner scannerWithString:[uuid substringWithRange:NSMakeRange(6, 2)]];
            if ([scanner scanHexInt:&value] && value > 0x35 && value - 0x35 >= 0x80) {
                self.outputCharacteristics[@(value - 0x35)] = characteristic;
            }
        }
    }

    // Prefer the newer report: it carries a trackpad timestamp and a finer IMU clock.
    if (input47 != nil) {
        self.inputCharacteristic = input47;
        self.inputReportId = 0x47;
    } else if (input45 != nil) {
        self.inputCharacteristic = input45;
        self.inputReportId = 0x45;
    }

    if (self.inputCharacteristic != nil) {
        [peripheral setNotifyValue:YES forCharacteristic:self.inputCharacteristic];
        [self startNotifyRetry];
    }
}

// Logged once per setup (and again if it changes), so a stalled setup shows
// whether the controller refused notifications or just isn't sending.
- (void)peripheral:(CBPeripheral *)peripheral didUpdateNotificationStateForCharacteristic:(CBCharacteristic *)characteristic error:(NSError *)error
{
    if (characteristic != self.inputCharacteristic || self.ready) {
        return;
    }
    NSString *result = error != nil ? [NSString stringWithFormat:@"refused to send input (%@)", error.localizedDescription]
                     : characteristic.isNotifying ? @"agreed to send input"
                                                  : nil;
    if (result != nil && ![result isEqualToString:self.notifyResult]) {
        self.notifyResult = result;
        TritonLog(self.delegate, [NSString stringWithFormat:@"Bluetooth: %@ %@", self.name, result]);
    }
}

- (void)peripheral:(CBPeripheral *)peripheral didUpdateValueForCharacteristic:(CBCharacteristic *)characteristic error:(NSError *)error
{
    if (characteristic == self.inputCharacteristic) {
        NSData *value = characteristic.value;
        if (error != nil || value.length != kStateReportPayload) {
            return;
        }
        if (!self.ready) {
            self.ready = YES;
            [self cancelNotifyRetry];
            [self.delegate tritonDidBecomeReady:self];
        }
        uint8_t report[1 + kStateReportPayload];
        report[0] = self.inputReportId;
        memcpy(report + 1, value.bytes, kStateReportPayload);
        [self.delegate triton:self didReceiveReport:report length:sizeof(report)];
    } else if (characteristic == self.batteryReportCharacteristic) {
        NSData *value = characteristic.value;
        if (error != nil || value.length < kBatteryReportPayload) {
            return;
        }
        // Some firmware puts the report ID first, as for feature reports.
        const uint8_t *bytes = (const uint8_t *)value.bytes;
        if (bytes[0] == kBatteryReportId && value.length >= kBatteryReportPayload + 1) {
            bytes += 1;
        }
        self.batteryFromReport = YES;
        [self deliverBatteryReport:bytes];
    } else if (characteristic == self.batteryLevelCharacteristic) {
        NSData *value = characteristic.value;
        if (error != nil || value.length < 1 || self.batteryFromReport) {
            return;
        }
        // Only a percentage: report it as discharging, with no voltages.
        uint8_t payload[kBatteryReportPayload] = {0};
        payload[0] = kChargeStateDischarging;
        payload[1] = MIN(((const uint8_t *)value.bytes)[0], (uint8_t)100);
        [self deliverBatteryReport:payload];
    } else if (characteristic == self.reportCharacteristic) {
        void (^pending)(NSData *) = self.pendingFeatureRead;
        self.pendingFeatureRead = nil;
        if (pending == nil) {
            return;
        }
        if (error != nil || characteristic.value.length == 0) {
            pending(nil);
            return;
        }
        // Match the USB layout: report ID first. Some firmware includes it
        // in the characteristic value already (command IDs are all >= 0x80).
        NSData *value = characteristic.value;
        if (((const uint8_t *)value.bytes)[0] == 0x01) {
            pending(value);
            return;
        }
        const uint8_t reportId = 0x01;
        NSMutableData *reply = [NSMutableData dataWithBytes:&reportId length:1];
        [reply appendData:value];
        pending(reply);
    }
}

@end

#pragma mark - ILNTritonBLE

@interface ILNTritonBLE () <CBCentralManagerDelegate>

@property (nonatomic, weak) id<ILNTritonBLEDelegate> delegate;
@property (nonatomic, strong) dispatch_queue_t queue;
@property (nonatomic, strong, nullable) CBCentralManager *central;
@property (nonatomic, strong) NSMutableDictionary<NSUUID *, ILNTritonDevice *> *devices;
@property (nonatomic, assign) BOOL running;
@property (nonatomic, assign) NSUInteger scanGeneration;
@property (nonatomic, strong, nullable) dispatch_source_t pollTimer;
@property (nonatomic, assign) CFAbsoluteTime lastKnownPoll;
@property (nonatomic, copy, nullable) NSString *restoreIdentifier;
/// Controllers that streamed to this app since it started.
@property (nonatomic, strong) NSMutableSet<NSUUID *> *streamedIdentifiers;
/// Controllers paired with this device: found connected to it, or handed back by iOS.
@property (nonatomic, strong) NSMutableSet<NSUUID *> *pairedIdentifiers;

@end

@implementation ILNTritonBLE

- (instancetype)initWithDelegate:(id<ILNTritonBLEDelegate>)delegate
{
    return [self initWithDelegate:delegate restoreIdentifier:nil];
}

- (instancetype)initWithDelegate:(id<ILNTritonBLEDelegate>)delegate restoreIdentifier:(NSString *)restoreIdentifier
{
    if ((self = [super init])) {
        _restoreIdentifier = [restoreIdentifier copy];
        _delegate = delegate;
        // Reports must be drained promptly or iOS may quietly stop delivering
        // them, so use a dedicated high-priority serial queue.
        _queue = dispatch_queue_create("com.evimore.inputline.ble", DISPATCH_QUEUE_SERIAL);
        dispatch_set_target_queue(_queue, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0));
        _devices = [NSMutableDictionary dictionary];
        _streamedIdentifiers = [NSMutableSet set];
        _pairedIdentifiers = [NSMutableSet set];
    }
    return self;
}

- (void)start
{
    dispatch_async(self.queue, ^{
        if (self.running) {
            return;
        }
        self.running = YES;
        if (self.central == nil) {
            // Creating the manager triggers centralManagerDidUpdateState:.
            NSDictionary *options = self.restoreIdentifier != nil ? @{CBCentralManagerOptionRestoreIdentifierKey: self.restoreIdentifier} : nil;
            self.central = [[CBCentralManager alloc] initWithDelegate:self queue:self.queue options:options];
        } else if (self.central.state == CBManagerStatePoweredOn) {
            [self lookForControllers];
        }
    });
}

- (void)stop
{
    dispatch_sync(self.queue, ^{
        self.running = NO;
        [self stopPolling];
        [self.central stopScan];
        for (ILNTritonDevice *device in self.devices.allValues) {
            [self.central cancelPeripheralConnection:device.peripheral];
            [device didDisconnect];
        }
        [self.devices removeAllObjects];
    });
}

- (void)scanForNewControllers:(NSTimeInterval)seconds
{
    dispatch_async(self.queue, ^{
        [self refreshLocked];
        [self scanLocked:seconds];
    });
}

- (void)refresh
{
    dispatch_async(self.queue, ^{
        [self refreshLocked];
    });
}

- (NSArray<ILNTritonDevice *> *)readyDevices
{
    __block NSArray<ILNTritonDevice *> *result = nil;
    dispatch_sync(self.queue, ^{
        NSMutableArray *ready = [NSMutableArray array];
        for (ILNTritonDevice *device in self.devices.allValues) {
            if (device.ready) {
                [ready addObject:device];
            }
        }
        result = ready;
    });
    return result;
}

#pragma mark Internals (Bluetooth queue)

/// Bluetooth is on and InputLine is running: get every controller going.
- (void)lookForControllers
{
    // Controllers iOS handed back before Bluetooth was on, when they couldn't
    // be set up: set up the connected ones now, and reconnect the others.
    for (ILNTritonDevice *device in self.devices.allValues) {
        CBPeripheral *peripheral = device.peripheral;
        if (peripheral.state == CBPeripheralStateConnected && device.setupStartedAt == 0) {
            TritonLog(self.delegate, [NSString stringWithFormat:@"Bluetooth: %@ is connected, setting it up", device.name]);
            [device didConnect];
        } else if (peripheral.state == CBPeripheralStateDisconnected) {
            [self.central connectPeripheral:peripheral options:nil];
        }
    }
    [self connectRememberedControllers];
    [self connectKnownControllers];
    [self startPolling];
    [self scanLocked:20];
}

- (void)refreshLocked
{
    if (!self.running || self.central.state != CBManagerStatePoweredOn) {
        return;
    }
    [self connectRememberedControllers];
    [self connectKnownControllers];
    [self checkStalledDevices];
}

/// This app's own controller: it streamed here before (this run or an
/// earlier one), or it is paired with this device.
- (BOOL)isKnown:(NSUUID *)identifier
{
    return [self.streamedIdentifiers containsObject:identifier] || [self.pairedIdentifiers containsObject:identifier] ||
           [self.rememberedIdentifiers containsObject:identifier];
}

/// Controllers that should be streaming but aren't: get each one going again.
- (void)checkStalledDevices
{
    const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    for (ILNTritonDevice *device in self.devices.allValues) {
        CBPeripheral *peripheral = device.peripheral;
        if (device.ready) {
            [self.streamedIdentifiers addObject:peripheral.identifier];
            continue;
        }
        const BOOL known = [self isKnown:peripheral.identifier];
        if (peripheral.state == CBPeripheralStateDisconnected) {
            // No connection pending (an attempt failed). Ask again for our
            // own controllers, so iOS connects them whenever they're on.
            if (known) {
                [self.central connectPeripheral:peripheral options:nil];
            }
            continue;
        }
        // Not connected yet, or connected but not told so yet
        // (didConnectPeripheral: sets it up).
        if (peripheral.state != CBPeripheralStateConnected || device.setupStartedAt == 0) {
            continue;
        }
        if (now - device.setupStartedAt < (known ? kKnownSetupTimeout : kSetupTimeout)) {
            continue;
        }
        // A new controller waits for the user to accept the pairing request;
        // its notification retries cover that.
        if (device.inputCharacteristic != nil && !known) {
            continue;
        }
        if (device.setupRestarts < (known ? kKnownMaxSetupRestarts : kMaxSetupRestarts)) {
            TritonLog(self.delegate, [NSString stringWithFormat:@"Bluetooth: %@ is connected but not sending; setting it up again", device.name]);
            [device restartSetup];
        } else {
            // A fresh connection; didDisconnectPeripheral: asks for it. Wait
            // a full timeout before trying this again.
            TritonLog(self.delegate, [NSString stringWithFormat:@"Bluetooth: %@ still not sending; reconnecting to it", device.name]);
            device.setupStartedAt = now;
            [self.central cancelPeripheralConnection:peripheral];
        }
    }
}

- (void)scanLocked:(NSTimeInterval)seconds
{
    if (!self.running || self.central.state != CBManagerStatePoweredOn) {
        return;
    }
    // The Valve service UUID does not fit in the base advertising packet, so
    // scan everything and filter by name, as SDL does.
    [self.central scanForPeripheralsWithServices:nil options:nil];
    const NSUInteger generation = ++self.scanGeneration;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC)), self.queue, ^{
        if (generation == self.scanGeneration) {
            [self.central stopScan];
        }
    });
}

// A paired controller that wakes up reconnects to iOS on its own; iOS does
// not tell apps, so ask periodically. This is a local query, not a scan.
// Controllers being set up are checked more often, so a stalled setup is
// caught within a second of its timeout.
- (void)startPolling
{
    if (self.pollTimer != nil) {
        return;
    }
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, self.queue);
    const uint64_t interval = (uint64_t)(kSetupCheckInterval * NSEC_PER_SEC);
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, (int64_t)interval), interval, NSEC_PER_SEC / 10);
    __weak ILNTritonBLE *weakSelf = self;
    dispatch_source_set_event_handler(timer, ^{
        ILNTritonBLE *strongSelf = weakSelf;
        if (strongSelf == nil || !strongSelf.running || strongSelf.central.state != CBManagerStatePoweredOn) {
            return;
        }
        const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
        if (now - strongSelf.lastKnownPoll >= kKnownControllerPollInterval) {
            strongSelf.lastKnownPoll = now;
            [strongSelf connectKnownControllers];
        }
        [strongSelf checkStalledDevices];
    });
    self.pollTimer = timer;
    dispatch_resume(timer);
}

- (void)stopPolling
{
    if (self.pollTimer != nil) {
        dispatch_source_cancel(self.pollTimer);
        self.pollTimer = nil;
    }
}

/// A pending connection to each controller seen before, which iOS completes
/// whenever the controller is on (at once if it is connected to this device).
- (void)connectRememberedControllers
{
    NSMutableSet<NSUUID *> *identifiers = [self.streamedIdentifiers mutableCopy];
    [identifiers addObjectsFromArray:self.rememberedIdentifiers ?: @[]];
    if (identifiers.count == 0) {
        return;
    }
    for (CBPeripheral *peripheral in [self.central retrievePeripheralsWithIdentifiers:identifiers.allObjects]) {
        [self connectPeripheral:peripheral found:nil];
    }
}

/// Controllers connected to this device right now (paired with it, working as
/// its mouse). Valve's service identifies them. In case iOS doesn't list that
/// service for a controller yet, also look by the standard services, and
/// check the name.
- (void)connectKnownControllers
{
    NSMutableSet<NSUUID *> *withValveService = [NSMutableSet set];
    NSMutableArray<CBPeripheral *> *found = [NSMutableArray array];
    for (CBPeripheral *peripheral in [self.central retrieveConnectedPeripheralsWithServices:@[[CBUUID UUIDWithString:kValveService]]]) {
        [withValveService addObject:peripheral.identifier];
        [found addObject:peripheral];
    }
    NSArray<CBUUID *> *standard = @[[CBUUID UUIDWithString:kBatteryService], [CBUUID UUIDWithString:kDeviceInformationService]];
    for (CBPeripheral *peripheral in [self.central retrieveConnectedPeripheralsWithServices:standard]) {
        if (![withValveService containsObject:peripheral.identifier] &&
            ([peripheral.name hasPrefix:@"Steam"] || [self isKnown:peripheral.identifier])) {
            [found addObject:peripheral];
        }
    }
    for (CBPeripheral *peripheral in found) {
        [self.pairedIdentifiers addObject:peripheral.identifier];
        [self connectPeripheral:peripheral found:@"connected to this device"];
    }
}

/// @param how For the event log: where the controller was found (nil: don't log).
- (void)connectPeripheral:(CBPeripheral *)peripheral found:(nullable NSString *)how
{
    if (self.devices[peripheral.identifier] != nil) {
        return;
    }
    ILNTritonDevice *device = [[ILNTritonDevice alloc] initWithPeripheral:peripheral queue:self.queue delegate:self.delegate];
    self.devices[peripheral.identifier] = device;
    if (how != nil) {
        TritonLog(self.delegate, [NSString stringWithFormat:@"Bluetooth: found %@ (%@)", device.name, how]);
    }
    [self.central connectPeripheral:peripheral options:nil];
}

#pragma mark CBCentralManagerDelegate

// iOS relaunched the app in the background and hands back the controllers
// it was connected to (this app connects to nothing else). Called before
// centralManagerDidUpdateState:, when commands don't work yet: they're set up
// once Bluetooth is on.
- (void)centralManager:(CBCentralManager *)central willRestoreState:(NSDictionary<NSString *, id> *)state
{
    for (CBPeripheral *peripheral in state[CBCentralManagerRestoredStatePeripheralsKey]) {
        if (self.devices[peripheral.identifier] != nil) {
            continue;
        }
        ILNTritonDevice *device = [[ILNTritonDevice alloc] initWithPeripheral:peripheral queue:self.queue delegate:self.delegate];
        self.devices[peripheral.identifier] = device;
        [self.pairedIdentifiers addObject:peripheral.identifier];
        TritonLog(self.delegate, [NSString stringWithFormat:@"Bluetooth: iOS handed back %@ (%@)", device.name,
                                                            peripheral.state == CBPeripheralStateConnected ? @"connected" : @"not connected"]);
    }
}

- (void)centralManagerDidUpdateState:(CBCentralManager *)central
{
    if (central.state == CBManagerStatePoweredOn) {
        if (self.running) {
            TritonLog(self.delegate, [NSString stringWithFormat:@"Bluetooth: on, looking for controllers (%lu remembered)",
                                                                (unsigned long)self.rememberedIdentifiers.count]);
            [self lookForControllers];
        }
        return;
    }
    [self stopPolling];
    if (central.state == CBManagerStateUnknown) {
        return;
    }
    // Off, resetting or not allowed: every connection is gone, without a
    // didDisconnectPeripheral: for each, and the peripherals are no longer
    // valid. They're looked up again once Bluetooth is back on.
    if (self.devices.count > 0) {
        TritonLog(self.delegate, @"Bluetooth: off; controllers disconnected");
    }
    for (ILNTritonDevice *device in self.devices.allValues) {
        [device didDisconnect];
    }
    [self.devices removeAllObjects];
    if (central.state == CBManagerStateUnauthorized && self.running) {
        id<ILNTritonBLEDelegate> delegate = self.delegate;
        if ([delegate respondsToSelector:@selector(tritonBluetoothUnauthorized)]) {
            [delegate tritonBluetoothUnauthorized];
        }
    }
}

- (void)centralManager:(CBCentralManager *)central didDiscoverPeripheral:(CBPeripheral *)peripheral advertisementData:(NSDictionary<NSString *, id> *)advertisementData RSSI:(NSNumber *)RSSI
{
    NSString *name = advertisementData[CBAdvertisementDataLocalNameKey] ?: peripheral.name;
    if (self.running && [name hasPrefix:@"Steam"]) {
        [self connectPeripheral:peripheral found:@"nearby"];
    }
}

- (void)centralManager:(CBCentralManager *)central didConnectPeripheral:(CBPeripheral *)peripheral
{
    ILNTritonDevice *device = self.devices[peripheral.identifier];
    TritonLog(self.delegate, [NSString stringWithFormat:@"Bluetooth: connected to %@, setting it up", device.name ?: @"a controller"]);
    [device didConnect];
}

- (void)centralManager:(CBCentralManager *)central didFailToConnectPeripheral:(CBPeripheral *)peripheral error:(NSError *)error
{
    ILNTritonDevice *device = self.devices[peripheral.identifier];
    TritonLog(self.delegate, [NSString stringWithFormat:@"Bluetooth: couldn't connect to %@ (%@)", device.name ?: @"a controller",
                                                        error.localizedDescription ?: @"no reason given"]);
    // Our own controllers stay: checkStalledDevices asks again. Others (a
    // stranger's controller in pairing mode) are let go.
    if (![self isKnown:peripheral.identifier]) {
        [self.devices removeObjectForKey:peripheral.identifier];
    }
}

- (void)centralManager:(CBCentralManager *)central didDisconnectPeripheral:(CBPeripheral *)peripheral error:(NSError *)error
{
    ILNTritonDevice *device = self.devices[peripheral.identifier];
    device.lastDisconnectError = error;
    [device didDisconnect];
    if (self.running && device != nil) {
        // A pending connect never times out: the controller reconnects as soon
        // as it wakes up or comes back into range.
        [central connectPeripheral:peripheral options:nil];
    } else {
        [self.devices removeObjectForKey:peripheral.identifier];
    }
}

@end
