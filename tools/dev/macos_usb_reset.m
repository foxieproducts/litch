#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <IOUSBHost/IOUSBHost.h>

// Reset only one exact Litch application device. The caller verifies the same
// physical USB parent again before opening serial or writing flash.
int main(int argc, const char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "expected one exact USB serial\n");
        return 2;
    }
    @autoreleasepool {
        NSString *expected = [NSString stringWithUTF8String:argv[1]];
        if (expected == nil || expected.length != 12) {
            fprintf(stderr, "invalid exact USB serial\n");
            return 2;
        }
        NSCharacterSet *hex = [NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdefABCDEF"];
        if ([[expected stringByTrimmingCharactersInSet:hex] length] != 0) {
            fprintf(stderr, "invalid exact USB serial\n");
            return 2;
        }
        io_iterator_t iterator = IO_OBJECT_NULL;
        kern_return_t result = IOServiceGetMatchingServices(
            kIOMainPortDefault, IOServiceMatching("IOUSBHostDevice"), &iterator);
        if (result != KERN_SUCCESS) {
            fprintf(stderr, "USB inventory unavailable: %d\n", result);
            return 3;
        }
        io_service_t exact = IO_OBJECT_NULL;
        io_service_t item;
        while ((item = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
            CFTypeRef serial = IORegistryEntryCreateCFProperty(
                item, CFSTR("USB Serial Number"), kCFAllocatorDefault, 0);
            CFTypeRef vendor = IORegistryEntryCreateCFProperty(
                item, CFSTR("idVendor"), kCFAllocatorDefault, 0);
            CFTypeRef product = IORegistryEntryCreateCFProperty(
                item, CFSTR("idProduct"), kCFAllocatorDefault, 0);
            int vid = -1;
            int pid = -1;
            if (vendor && CFGetTypeID(vendor) == CFNumberGetTypeID()) {
                CFNumberGetValue(vendor, kCFNumberIntType, &vid);
            }
            if (product && CFGetTypeID(product) == CFNumberGetTypeID()) {
                CFNumberGetValue(product, kCFNumberIntType, &pid);
            }
            BOOL matches = serial && CFGetTypeID(serial) == CFStringGetTypeID() &&
                [(__bridge NSString *)serial caseInsensitiveCompare:expected] == NSOrderedSame &&
                vid == 0x303a && pid == 0x4001;
            if (matches) {
                if (exact != IO_OBJECT_NULL) {
                    fprintf(stderr, "multiple USB devices claim the exact Litch serial\n");
                    IOObjectRelease(item);
                    IOObjectRelease(exact);
                    IOObjectRelease(iterator);
                    if (serial) CFRelease(serial);
                    if (vendor) CFRelease(vendor);
                    if (product) CFRelease(product);
                    return 4;
                }
                exact = item;
            } else {
                IOObjectRelease(item);
            }
            if (serial) CFRelease(serial);
            if (vendor) CFRelease(vendor);
            if (product) CFRelease(product);
        }
        IOObjectRelease(iterator);
        if (exact == IO_OBJECT_NULL) {
            fprintf(stderr, "exact Litch application USB device not found\n");
            return 4;
        }
        NSError *error = nil;
        IOUSBHostDevice *device = [[IOUSBHostDevice alloc]
            initWithIOService:exact
            queue:dispatch_get_main_queue()
            error:&error
            interestHandler:nil];
        if (device == nil) {
            fprintf(stderr, "cannot open exact USB device: %s\n",
                    error.localizedDescription.UTF8String);
            IOObjectRelease(exact);
            return 5;
        }
        BOOL reset = [device resetWithError:&error];
        IOObjectRelease(exact);
        if (!reset) {
            fprintf(stderr, "exact USB device reset failed: %s\n",
                    error.localizedDescription.UTF8String);
            return 6;
        }
        return 0;
    }
}
